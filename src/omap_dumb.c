/*
 * Copyright © 2012 ARM Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#include <stdlib.h>
#include <sys/mman.h>
#include <assert.h>
#include <errno.h>

#include <xf86.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <exynos_drm.h>
#include <libdrm/exynos_drmif.h>

#include "omap_dumb.h"
#include "omap_msg.h"

struct omap_device {
	struct exynos_device exynos_dev;
	ScrnInfoPtr pScrn;
};

struct omap_bo {
	struct omap_device *dev;
	struct exynos_bo *exynos_bo;
	uint32_t fb_id;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint8_t depth;
	uint8_t bpp;
	uint32_t pixel_format;
	int refcnt;
	int acquired_exclusive;
	int acquire_cnt;
	int dirty;
};

enum {
	DRM_EXYNOS_GEM_CPU_ACQUIRE_SHARED = 0x0,
	DRM_EXYNOS_GEM_CPU_ACQUIRE_EXCLUSIVE = 0x1,
};

struct drm_exynos_gem_cpu_acquire {
	unsigned int handle;
	unsigned int flags;
};

struct drm_exynos_gem_cpu_release {
	unsigned int handle;
};

/* TODO: use exynos_drm.h kernel headers (http://crosbug.com/37294) */
#define DRM_EXYNOS_GEM_CPU_ACQUIRE	0x08
#define DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_EXYNOS_GEM_CPU_ACQUIRE, struct drm_exynos_gem_cpu_acquire)
#define DRM_EXYNOS_GEM_CPU_RELEASE	0x09
#define DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE	DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_EXYNOS_GEM_CPU_RELEASE, struct drm_exynos_gem_cpu_release)

/* device related functions:
 */

struct omap_device *omap_device_new(int fd, ScrnInfoPtr pScrn)
{
	struct omap_device *new_dev = calloc(1, sizeof *new_dev);
	if (!new_dev)
		return NULL;

	new_dev->exynos_dev.fd = fd;
	new_dev->pScrn = pScrn;

	return new_dev;
}

void omap_device_del(struct omap_device *dev)
{
	free(dev);
}

/* buffer-object related functions:
 */

static struct omap_bo *omap_bo_new(struct omap_device *dev, uint32_t width,
		uint32_t height, uint8_t depth, uint8_t bpp,
		uint32_t pixel_format)
{
	ScrnInfoPtr pScrn = dev->pScrn;
	struct omap_bo *new_buf;
	uint32_t pitch;
	size_t size;
	const uint32_t flags = EXYNOS_BO_NONCONTIG;
	int ret;

	new_buf = calloc(1, sizeof(*new_buf));
	if (!new_buf)
		return NULL;

	/* align to 64 bytes since Mali requires it.
	 */
	pitch = ((((width * bpp + 7) / 8) + 63) / 64) * 64;
	size = height * pitch;

	new_buf->exynos_bo = exynos_bo_create(&dev->exynos_dev, size, flags);
	if (!new_buf->exynos_bo)
	{
		free(new_buf);
		ERROR_MSG("EXYNOS_BO_CREATE(size: %zu flags: 0x%x) failed: %s",
				size, flags, strerror(errno));
		return NULL;
	}

	DEBUG_MSG("Created [BO:%u] {size: %u flags: 0x%x}",
			new_buf->exynos_bo->handle, new_buf->exynos_bo->size,
			flags);

	if (depth) {
		ret = drmModeAddFB(dev->exynos_dev.fd, width, height, depth,
				bpp, pitch, new_buf->exynos_bo->handle,
				&new_buf->fb_id);
		if (ret < 0) {
			ERROR_MSG("[BO:%u] add FB {%ux%u depth: %u bpp: %u pitch: %u} failed: %s",
					new_buf->exynos_bo->handle, width,
					height, depth, bpp, pitch,
					strerror(errno));
			goto destroy_bo;
		}
		DEBUG_MSG("Created [FB:%u] {%ux%u depth: %u bpp: %u pitch: %u} using [BO:%u]",
				new_buf->fb_id, width, height, depth, bpp,
				pitch, new_buf->exynos_bo->handle);
	} else {
		uint32_t handles[4] = { new_buf->exynos_bo->handle };
		uint32_t pitches[4] = { pitch };
		uint32_t offsets[4] = { 0 };

		ret = drmModeAddFB2(dev->exynos_dev.fd, width, height,
				pixel_format, handles, pitches, offsets,
				&new_buf->fb_id, 0);
		if (ret < 0) {
			ERROR_MSG("[BO:%u] add FB {%ux%u format: %.4s pitch: %u} failed: %s",
					new_buf->exynos_bo->handle, width,
					height, (char *)&pixel_format, pitch,
					strerror(errno));
			goto destroy_bo;
		}
		/* print pixel_format as a 'four-cc' ASCII code */
		DEBUG_MSG("[BO:%u] [FB:%u] Added FB: {%ux%u format: %.4s pitch: %u}",
				new_buf->exynos_bo->handle, new_buf->fb_id,
				width, height, (char *)&pixel_format, pitch);
	}

	new_buf->dev = dev;
	new_buf->width = width;
	new_buf->height = height;
	new_buf->pitch = pitch;
	new_buf->depth = depth;
	new_buf->bpp = bpp;
	new_buf->pixel_format = pixel_format;
	new_buf->refcnt = 1;
	new_buf->acquired_exclusive = 0;
	new_buf->acquire_cnt = 0;
	new_buf->dirty = TRUE;

	return new_buf;

destroy_bo:
	exynos_bo_destroy(new_buf->exynos_bo);
	free(new_buf);
	return NULL;
}

struct omap_bo *omap_bo_new_with_depth(struct omap_device *dev, uint32_t width,
		uint32_t height, uint8_t depth, uint8_t bpp)
{
	return omap_bo_new(dev, width, height, depth, bpp, 0);
}

struct omap_bo *omap_bo_new_with_format(struct omap_device *dev, uint32_t width,
		uint32_t height, uint32_t pixel_format, uint8_t bpp)
{
	return omap_bo_new(dev, width, height, 0, bpp, pixel_format);
}

static void omap_bo_del(struct omap_bo *bo)
{
	ScrnInfoPtr pScrn;
	int res;

	if (!bo)
		return;

	pScrn = bo->dev->pScrn;

	DEBUG_MSG("[BO:%u] [FB:%u] [FLINK:%u] mmap: %p size: %u",
			bo->exynos_bo->handle, bo->fb_id, bo->exynos_bo->name,
			bo->exynos_bo->vaddr, bo->exynos_bo->size);

	res = drmModeRmFB(bo->dev->exynos_dev.fd, bo->fb_id);
	if (res)
		ERROR_MSG("[BO:%u] Remove [FB:%u] failed: %s",
				bo->exynos_bo->handle, bo->fb_id,
				strerror(errno));
	assert(res == 0);
	exynos_bo_destroy(bo->exynos_bo);
	free(bo);
}

void omap_bo_unreference(struct omap_bo *bo)
{
	if (!bo)
		return;

	assert(bo->refcnt > 0);
	if (--bo->refcnt == 0)
		omap_bo_del(bo);
}

void omap_bo_reference(struct omap_bo *bo)
{
	assert(bo->refcnt > 0);
	bo->refcnt++;
}

uint32_t omap_bo_get_name(struct omap_bo *bo)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	uint32_t name;
	int ret;

	if (bo->exynos_bo->name)
		return bo->exynos_bo->name;

	ret = exynos_bo_get_name(bo->exynos_bo, &name);
	if (ret) {
		ERROR_MSG("[BO:%u] EXYNOS_BO_GET_NAME failed: %s",
				bo->exynos_bo->handle, strerror(errno));
		return 0;
	}

	DEBUG_MSG("[BO:%u] [FB:%u] [FLINK:%u] mmap: %p",
			bo->exynos_bo->handle, bo->fb_id, bo->exynos_bo->name,
			bo->exynos_bo->vaddr);

	return bo->exynos_bo->name;
}

uint32_t omap_bo_handle(struct omap_bo *bo)
{
	return bo->exynos_bo->handle;
}

uint32_t omap_bo_width(struct omap_bo *bo)
{
	return bo->width;
}

uint32_t omap_bo_height(struct omap_bo *bo)
{
	return bo->height;
}

uint32_t omap_bo_bpp(struct omap_bo *bo)
{
	return bo->bpp;
}

/* Bytes per pixel */
uint32_t omap_bo_Bpp(struct omap_bo *bo)
{
	return (bo->bpp + 7) / 8;
}

uint32_t omap_bo_pitch(struct omap_bo *bo)
{
	return bo->pitch;
}

uint32_t omap_bo_depth(struct omap_bo *bo)
{
	return bo->depth;
}

uint32_t omap_bo_fb(struct omap_bo *bo)
{
	return bo->fb_id;
}

void *omap_bo_map(struct omap_bo *bo)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	void *map_addr;

	if (bo->exynos_bo->vaddr)
		return bo->exynos_bo->vaddr;

	map_addr = exynos_bo_map(bo->exynos_bo);
	if (!map_addr) {
		ERROR_MSG("[BO:%u] EXYNOS_BO_MAP failed: %s",
				bo->exynos_bo->handle, strerror(errno));
		return NULL;
	}

	DEBUG_MSG("[BO:%u] [FB:%u] [FLINK:%u] mmap: %p mapped %zu bytes",
			bo->exynos_bo->handle, bo->fb_id, bo->exynos_bo->name,
			bo->exynos_bo->vaddr, bo->exynos_bo->size);
	return map_addr;
}

int omap_bo_cpu_prep(struct omap_bo *bo, enum omap_gem_op op)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_exynos_gem_cpu_acquire acquire;
	int ret;

	if (bo->acquire_cnt) {
		if ((op & OMAP_GEM_WRITE) && !bo->acquired_exclusive) {
			ERROR_MSG("attempting to acquire read locked surface for write");
			return 1;
		}
		bo->acquire_cnt++;
		return 0;
	}
	acquire.handle = bo->exynos_bo->handle;
	acquire.flags = (op & OMAP_GEM_WRITE)
		? DRM_EXYNOS_GEM_CPU_ACQUIRE_EXCLUSIVE
		: DRM_EXYNOS_GEM_CPU_ACQUIRE_SHARED;
	ret = drmIoctl(bo->dev->exynos_dev.fd, DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE,
			&acquire);
	if (ret) {
		ERROR_MSG("DRM_IOCTL_EXYNOS_GEM_CPU_ACQUIRE failed: %s",
				strerror(errno));
	} else {
		bo->acquired_exclusive = op & OMAP_GEM_WRITE;
		bo->acquire_cnt++;
		if (bo->acquired_exclusive) {
			bo->dirty = TRUE;
		}
	}
	return ret;
}

int omap_bo_cpu_fini(struct omap_bo *bo, enum omap_gem_op op)
{
	ScrnInfoPtr pScrn = bo->dev->pScrn;
	struct drm_exynos_gem_cpu_release release;
	int ret;

	assert(bo->acquire_cnt > 0);

	if (--bo->acquire_cnt != 0) {
		return 0;
	}
	release.handle = bo->exynos_bo->handle;
	ret = drmIoctl(bo->dev->exynos_dev.fd, DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE,
			&release);
	if (ret)
		ERROR_MSG("DRM_IOCTL_EXYNOS_GEM_CPU_RELEASE failed: %s",
				strerror(errno));
	return ret;
}

int omap_bo_get_dirty(struct omap_bo *bo)
{
	return bo->dirty;
}

void omap_bo_clear_dirty(struct omap_bo *bo)
{
	bo->dirty = FALSE;
}

