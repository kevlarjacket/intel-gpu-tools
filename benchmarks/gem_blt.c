/*
 * Copyright © 2011 Intel Corporation
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
 *    Chris Wilson <chris@chris-wilson.co.uk>
 *
 */

#include "igt.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>

#include "drm.h"

#define LOCAL_I915_EXEC_NO_RELOC (1<<11)
#define LOCAL_I915_EXEC_HANDLE_LUT (1<<12)

#define COPY_BLT_CMD		(2<<29|0x53<<22|0x6)
#define BLT_WRITE_ALPHA		(1<<21)
#define BLT_WRITE_RGB		(1<<20)
#define BLT_SRC_TILED		(1<<15)
#define BLT_DST_TILED		(1<<11)

static int has_64bit_reloc;

static int gem_linear_blt(int fd,
			  uint32_t *batch,
			  int offset,
			  uint32_t src,
			  uint32_t dst,
			  uint32_t length,
			  struct drm_i915_gem_relocation_entry *reloc)
{
	uint32_t *b = batch + offset/4;
	int height = length / (16 * 1024);

	igt_assert_lte(height, 1 << 16);

	if (height) {
		int i = 0;
		b[i++] = COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
		if (has_64bit_reloc)
			b[i-1]+=2;
		b[i++] = 0xcc << 16 | 1 << 25 | 1 << 24 | (16*1024);
		b[i++] = 0;
		b[i++] = height << 16 | (4*1024);
		b[i++] = 0;
		reloc->offset = (b-batch+4) * sizeof(uint32_t);
		reloc->delta = 0;
		reloc->target_handle = dst;
		reloc->read_domains = I915_GEM_DOMAIN_RENDER;
		reloc->write_domain = I915_GEM_DOMAIN_RENDER;
		reloc->presumed_offset = 0;
		reloc++;
		if (has_64bit_reloc)
			b[i++] = 0; /* FIXME */

		b[i++] = 0;
		b[i++] = 16*1024;
		b[i++] = 0;
		reloc->offset = (b-batch+7) * sizeof(uint32_t);
		if (has_64bit_reloc)
			reloc->offset += sizeof(uint32_t);
		reloc->delta = 0;
		reloc->target_handle = src;
		reloc->read_domains = I915_GEM_DOMAIN_RENDER;
		reloc->write_domain = 0;
		reloc->presumed_offset = 0;
		reloc++;
		if (has_64bit_reloc)
			b[i++] = 0; /* FIXME */

		b += i;
		length -= height * 16*1024;
	}

	if (length) {
		int i = 0;
		b[i++] = COPY_BLT_CMD | BLT_WRITE_ALPHA | BLT_WRITE_RGB;
		if (has_64bit_reloc)
			b[i-1]+=2;
		b[i++] = 0xcc << 16 | 1 << 25 | 1 << 24 | (16*1024);
		b[i++] = height << 16;
		b[i++] = (1+height) << 16 | (length / 4);
		b[i++] = 0;
		reloc->offset = (b-batch+4) * sizeof(uint32_t);
		reloc->delta = 0;
		reloc->target_handle = dst;
		reloc->read_domains = I915_GEM_DOMAIN_RENDER;
		reloc->write_domain = I915_GEM_DOMAIN_RENDER;
		reloc->presumed_offset = 0;
		reloc++;
		if (has_64bit_reloc)
			b[i++] = 0; /* FIXME */

		b[i++] = height << 16;
		b[i++] = 16*1024;
		b[i++] = 0;
		reloc->offset = (b-batch+7) * sizeof(uint32_t);
		if (has_64bit_reloc)
			reloc->offset += sizeof(uint32_t);
		reloc->delta = 0;
		reloc->target_handle = src;
		reloc->read_domains = I915_GEM_DOMAIN_RENDER;
		reloc->write_domain = 0;
		reloc->presumed_offset = 0;
		reloc++;
		if (has_64bit_reloc)
			b[i++] = 0; /* FIXME */

		b += i;
	}

	b[0] = MI_BATCH_BUFFER_END;
	b[1] = 0;

	return (b+2 - batch) * sizeof(uint32_t);
}

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return (end->tv_sec - start->tv_sec) + 1e-9*(end->tv_nsec - start->tv_nsec);
}

static int __gem_execbuf(int fd, struct drm_i915_gem_execbuffer2 *execbuf)
{
	int err = 0;
	if (drmIoctl(fd, DRM_IOCTL_I915_GEM_EXECBUFFER2, execbuf))
		err = -errno;
	return err;
}

static int run(int object, int batch, int count, int set, int reps)
{
	struct drm_i915_gem_execbuffer2 execbuf;
	struct drm_i915_gem_exec_object2 exec[3];
	struct drm_i915_gem_relocation_entry *reloc;
	uint32_t *buf, handle, src, dst;
	int fd, len, gen, size;
	int ring;

	size = ALIGN(batch * 64, 4096);
	reloc = malloc(sizeof(*reloc)*size/32*2);

	fd = drm_open_driver(DRIVER_INTEL);
	handle = gem_create(fd, size);
	buf = gem_mmap__cpu(fd, handle, 0, size, PROT_WRITE);

	gen = intel_gen(intel_get_drm_devid(fd));
	has_64bit_reloc = gen >= 8;

	src = gem_create(fd, object);
	dst = gem_create(fd, object);

	len = gem_linear_blt(fd, buf, 0, 0, 1, object, reloc);

	memset(exec, 0, sizeof(exec));
	exec[0].handle = src;
	exec[1].handle = dst;

	exec[2].handle = handle;
	if (has_64bit_reloc)
		exec[2].relocation_count = len > 56 ? 4 : 2;
	else
		exec[2].relocation_count = len > 40 ? 4 : 2;
	exec[2].relocs_ptr = (uintptr_t)reloc;

	ring = 0;
	if (gen >= 6)
		ring = I915_EXEC_BLT;

	memset(&execbuf, 0, sizeof(execbuf));
	execbuf.buffers_ptr = (uintptr_t)exec;
	execbuf.buffer_count = 3;
	execbuf.batch_len = len;
	execbuf.flags = ring;
	execbuf.flags |= LOCAL_I915_EXEC_HANDLE_LUT;
	execbuf.flags |= LOCAL_I915_EXEC_NO_RELOC;

	if (__gem_execbuf(fd, &execbuf)) {
		gem_set_domain(fd, handle, I915_GEM_DOMAIN_CPU, I915_GEM_DOMAIN_CPU);
		len = gem_linear_blt(fd, buf, 0, src, dst, object, reloc);
		igt_assert(len == execbuf.batch_len);
		execbuf.flags = ring;
		gem_execbuf(fd, &execbuf);
	}
	gem_sync(fd, handle);

	if (batch > 1) {
		int nreloc = exec[2].relocation_count;

		if (execbuf.flags & LOCAL_I915_EXEC_HANDLE_LUT) {
			src = 0;
			dst = 1;
		}

		for (int i = 1; i < batch; i++) {
			len = gem_linear_blt(fd, buf, len - 8,
					     src, dst, object,
					     reloc + exec[2].relocation_count);
			exec[2].relocation_count += nreloc;
		}
		execbuf.batch_len = len;

		gem_execbuf(fd, &execbuf);
		gem_sync(fd, handle);
	}

	while (reps--) {
		double min = HUGE_VAL;

		for (int s = 0; s < set; s++) {
			struct timespec start, end;
			double t;

			clock_gettime(CLOCK_MONOTONIC, &start);
			for (int loop = 0; loop < count; loop++)
				gem_execbuf(fd, &execbuf);
			gem_sync(fd, handle);
			clock_gettime(CLOCK_MONOTONIC, &end);

			t = elapsed(&start, &end);
			if (t < min)
				min = t;
		}

		printf("%7.3f\n", object/(1024*1024.)*batch*count/min);
	}

	close(fd);
	return 0;
}

int main(int argc, char **argv)
{
	int size = 1024*1024;
	int count = 1;
	int reps = 13;
	int set = 30;
	int batch = 1;
	int c;

	while ((c = getopt (argc, argv, "c:r:s:b:S:")) != -1) {
		switch (c) {
		case 'c':
			count = atoi(optarg);
			if (count < 1)
				count = 1;
			break;

		case 's':
			size = atoi(optarg);
			if (size < 4096)
				size = 4096;
			break;

		case 'S':
			set = atoi(optarg);
			if (set < 1)
				set = 1;
			break;

		case 'r':
			reps = atoi(optarg);
			if (reps < 1)
				reps = 1;
			break;

		case 'b':
			batch = atoi(optarg);
			if (batch < 1)
				batch = 1;
			break;

		default:
			break;
		}
	}

	return run(size, batch, count, set, reps);
}