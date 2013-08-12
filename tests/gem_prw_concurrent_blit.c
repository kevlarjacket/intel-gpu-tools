/*
 * Copyright © 2009,2012,2013 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Eric Anholt <eric@anholt.net>
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *    Daniel Vetter <daniel.vetter@ffwll.ch>
 *
 */

/** @file gem_prw_concurrent_blit.c
 *
 * This is a test of pread/pwrite behavior when writing to active
 * buffers.
 *
 * Based on gem_gtt_concurrent_blt.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "drm.h"
#include "i915_drm.h"
#include "drmtest.h"
#include "intel_bufmgr.h"
#include "intel_batchbuffer.h"
#include "intel_gpu_tools.h"

static void
set_bo(drm_intel_bo *bo, uint32_t val, int width, int height)
{
	int size = width * height;
	uint32_t *vaddr, *tmp;

	vaddr = tmp = malloc(size*4);
	while (size--)
		*vaddr++ = val;
	drm_intel_bo_subdata(bo, 0, width*height*4, tmp);
	free(tmp);
}

static void
cmp_bo(drm_intel_bo *bo, uint32_t val, int width, int height)
{
	int size = width * height;
	uint32_t *vaddr, *tmp;

	vaddr = tmp = malloc(size*4);
	drm_intel_bo_get_subdata(bo, 0, size*4, tmp);
	while (size--)
		assert(*vaddr++ == val);
	free(tmp);
}

static drm_intel_bo *
create_bo(drm_intel_bufmgr *bufmgr, uint32_t val, int width, int height)
{
	drm_intel_bo *bo;

	bo = drm_intel_bo_alloc(bufmgr, "bo", 4*width*height, 0);
	assert(bo);

	return bo;
}

int
main(int argc, char **argv)
{
	drm_intel_bufmgr *bufmgr;
	struct intel_batchbuffer *batch;
	int num_buffers = 128, max;
	drm_intel_bo *src[128], *dst[128], *dummy = NULL;
	int width = 512, height = 512;
	int fd, loop, i;

	drmtest_subtest_init(argc, argv);
	drmtest_skip_on_simulation();

	fd = drm_open_any();

	max = gem_aperture_size (fd) / (1024 * 1024) / 2;
	if (num_buffers > max)
		num_buffers = max;

	bufmgr = drm_intel_bufmgr_gem_init(fd, 4096);
	drm_intel_bufmgr_gem_enable_reuse(bufmgr);
	batch = intel_batchbuffer_alloc(bufmgr, intel_get_drm_devid(fd));

	if (!drmtest_only_list_subtests()) {
		for (i = 0; i < num_buffers; i++) {
			src[i] = create_bo(bufmgr, i, width, height);
			dst[i] = create_bo(bufmgr, ~i, width, height);
		}
		dummy = create_bo(bufmgr, 0, width, height);
	}

	/* try to overwrite the source values */
	drmtest_subtest("overwrite-source") {
		for (i = 0; i < num_buffers; i++) {
			set_bo(src[i], i, width, height);
			set_bo(dst[i], i, width, height);
		}
		for (i = 0; i < num_buffers; i++)
			intel_copy_bo(batch, dst[i], src[i], width, height);
		for (i = num_buffers; i--; )
			set_bo(src[i], 0xdeadbeef, width, height);
		for (i = 0; i < num_buffers; i++)
			cmp_bo(dst[i], i, width, height);
	}

	/* try to read the results before the copy completes */
	drmtest_subtest("early-read") {
		for (i = num_buffers; i--; )
			set_bo(src[i], 0xdeadbeef, width, height);
		for (i = 0; i < num_buffers; i++)
			intel_copy_bo(batch, dst[i], src[i], width, height);
		for (i = num_buffers; i--; )
			cmp_bo(dst[i], 0xdeadbeef, width, height);
	}

	/* and finally try to trick the kernel into loosing the pending write */
	drmtest_subtest("gpu-read-after-write") {
		for (i = num_buffers; i--; )
			set_bo(src[i], 0xabcdabcd, width, height);
		for (i = 0; i < num_buffers; i++)
			intel_copy_bo(batch, dst[i], src[i], width, height);
		for (i = num_buffers; i--; )
			intel_copy_bo(batch, dummy, dst[i], width, height);
		for (i = num_buffers; i--; )
			cmp_bo(dst[i], 0xabcdabcd, width, height);
	}

	drmtest_fork_signal_helper();

	/* try to read the results before the copy completes */
	drmtest_subtest("overwrite-source-interruptible") {
		for (loop = 0; loop < 10; loop++) {
			gem_quiescent_gpu(fd);
			for (i = 0; i < num_buffers; i++) {
				set_bo(src[i], i, width, height);
				set_bo(dst[i], i, width, height);
			}
			for (i = 0; i < num_buffers; i++)
				intel_copy_bo(batch, dst[i], src[i], width, height);
			for (i = num_buffers; i--; )
				set_bo(src[i], 0xdeadbeef, width, height);
			for (i = 0; i < num_buffers; i++)
				cmp_bo(dst[i], i, width, height);
		}
	}

	/* try to read the results before the copy completes */
	drmtest_subtest("early-read-interruptible") {
		for (loop = 0; loop < 10; loop++) {
			gem_quiescent_gpu(fd);
			for (i = num_buffers; i--; )
				set_bo(src[i], 0xdeadbeef, width, height);
			for (i = 0; i < num_buffers; i++)
				intel_copy_bo(batch, dst[i], src[i], width, height);
			for (i = num_buffers; i--; )
				cmp_bo(dst[i], 0xdeadbeef, width, height);
		}
	}

	/* and finally try to trick the kernel into loosing the pending write */
	drmtest_subtest("gpu-read-after-write-interruptible") {
		for (loop = 0; loop < 10; loop++) {
			gem_quiescent_gpu(fd);
			for (i = num_buffers; i--; )
				set_bo(src[i], 0xabcdabcd, width, height);
			for (i = 0; i < num_buffers; i++)
				intel_copy_bo(batch, dst[i], src[i], width, height);
			for (i = num_buffers; i--; )
				intel_copy_bo(batch, dummy, dst[i], width, height);
			for (i = num_buffers; i--; )
				cmp_bo(dst[i], 0xabcdabcd, width, height);
		}
	}

	drmtest_stop_signal_helper();

	return 0;
}
