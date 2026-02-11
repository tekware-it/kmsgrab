// SPDX-License-Identifier: GPL-2.0-only
/*
 * KMS/DRM screenshot tool
 *
 * Copyright (c) 2021 Paul Cercueil <paul@crapouillou.net>
 */

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <png.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <jpeglib.h>

typedef struct {
	uint8_t r, g, b;
} uint24_t;

static inline uint24_t rgb16_to_24(uint16_t px)
{
	uint24_t pixel;

	pixel.b = (px & 0x1f)   << 3;
	pixel.g = (px & 0x7e0)  >> 3;
	pixel.r = (px & 0xf800) >> 8;

	return pixel;
}


static inline uint24_t rgb32_to_24(uint32_t px)
{
	uint24_t pixel;

	pixel.b = px & 0xff;
	pixel.g = (px >> 8) & 0xff;
	pixel.r = (px >> 16) & 0xff;

	return pixel;
}


static inline void convert_to_24(drmModeFB *fb, uint24_t *to, void *from)
{
	unsigned int len = fb->width * fb->height;

	if (fb->bpp == 16) {
		uint16_t *ptr = from;
		while (len--)
			*to++ = rgb16_to_24(*ptr++);
	} else {
		uint32_t *ptr = from;
		while (len--)
			*to++ = rgb32_to_24(*ptr++);
	}
}

static int save_png(drmModeFB *fb, int prime_fd, uint32_t pitch,
		    const char *png_fn)
{
	png_bytep *row_pointers;
	png_structp png;
	png_infop info;
	FILE *pngfile;
	void *buffer, *picture, *linear;
	unsigned int i;
	int ret;
	size_t bytes_per_pixel = fb->bpp >> 3;
	size_t linear_size = (size_t)fb->width * fb->height * bytes_per_pixel;
	size_t mmap_size = (size_t)pitch * fb->height;

	fprintf(stderr, "[debug] save_png: fb_id=%"PRIu32" width=%"PRIu32" height=%"PRIu32" bpp=%"PRIu32" depth=%"PRIu32" handle=%"PRIu32"\n",
		fb->fb_id, fb->width, fb->height, fb->bpp, fb->depth, fb->handle);
	fprintf(stderr, "[debug] save_png: prime_fd=%d pitch=%"PRIu32" png_fn=%s\n",
		prime_fd, pitch, png_fn);

	picture = malloc(fb->width * fb->height * 4);
	if (!picture)
		return -ENOMEM;

	linear = malloc(linear_size);
	if (!linear) {
		ret = -ENOMEM;
		goto out_free_picture;
	}

	buffer = mmap(NULL, mmap_size,
		      PROT_READ, MAP_PRIVATE, prime_fd, 0);
	if (buffer == MAP_FAILED) {
		ret = -errno;
		fprintf(stderr, "Unable to mmap prime buffer\n");
		goto out_free_linear;
	}

	fprintf(stderr, "[debug] save_png: mmap length=%zu buffer=%p\n",
		mmap_size, buffer);

	/* Drop privileges, to write PNG with user rights */
	seteuid(getuid());

	pngfile = fopen(png_fn, "w+");
	if (!pngfile) {
		ret = -errno;
		goto out_unmap_buffer;
	}

	png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
				NULL, NULL, NULL);
	if (!png) {
		ret = -errno;
		goto out_fclose;
	}

	info = png_create_info_struct(png);
	if (!info) {
		ret = -errno;
		goto out_free_png;
	}

	png_init_io(png, pngfile);
	png_set_IHDR(png, info, fb->width, fb->height, 8,
				PNG_COLOR_TYPE_RGB,
				PNG_INTERLACE_NONE,
				PNG_COMPRESSION_TYPE_BASE,
				PNG_FILTER_TYPE_BASE);
	png_write_info(png, info);

	// Copy framebuffer using pitch to a linear buffer, then convert to rgb888.
	for (i = 0; i < fb->height; i++)
		memcpy((uint8_t *)linear + i * fb->width * bytes_per_pixel,
		       (uint8_t *)buffer + i * pitch,
		       fb->width * bytes_per_pixel);

	convert_to_24(fb, picture, linear);

	row_pointers = malloc(sizeof(*row_pointers) * fb->height);
	if (!row_pointers) {
		ret = -ENOMEM;
		goto out_free_info;
	}

	// And save the final image
	for (i = 0; i < fb->height; i++)
		row_pointers[i] = picture + i * fb->width * 3;

	fprintf(stderr, "[debug] save_png: writing PNG rows=%"PRIu32" row_bytes=%"PRIu32"\n",
		fb->height, fb->width * 3);

	png_write_image(png, row_pointers);
	png_write_end(png, info);

	ret = 0;

	free(row_pointers);
out_free_info:
	png_destroy_write_struct(NULL, &info);
out_free_png:
	png_destroy_write_struct(&png, NULL);
out_fclose:
	fclose(pngfile);
out_unmap_buffer:
	munmap(buffer, mmap_size);
out_free_linear:
	free(linear);
out_free_picture:
	free(picture);
	return ret;
}

static int save_jpg(drmModeFB *fb, int prime_fd, uint32_t pitch,
		    const char *jpg_fn, int quality)
{
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	FILE *jpgfile;
	void *buffer, *picture, *linear;
	unsigned int i;
	int ret;
	size_t bytes_per_pixel = fb->bpp >> 3;
	size_t linear_size = (size_t)fb->width * fb->height * bytes_per_pixel;
	size_t mmap_size = (size_t)pitch * fb->height;
	JSAMPROW row_pointer[1];

	fprintf(stderr, "[debug] save_jpg: fb_id=%"PRIu32" width=%"PRIu32" height=%"PRIu32" bpp=%"PRIu32" depth=%"PRIu32" handle=%"PRIu32"\n",
		fb->fb_id, fb->width, fb->height, fb->bpp, fb->depth, fb->handle);
	fprintf(stderr, "[debug] save_jpg: prime_fd=%d pitch=%"PRIu32" jpg_fn=%s quality=%d\n",
		prime_fd, pitch, jpg_fn, quality);

	picture = malloc(fb->width * fb->height * 4);
	if (!picture)
		return -ENOMEM;

	linear = malloc(linear_size);
	if (!linear) {
		ret = -ENOMEM;
		goto out_free_picture;
	}

	buffer = mmap(NULL, mmap_size, PROT_READ, MAP_PRIVATE, prime_fd, 0);
	if (buffer == MAP_FAILED) {
		ret = -errno;
		fprintf(stderr, "Unable to mmap prime buffer\n");
		goto out_free_linear;
	}

	fprintf(stderr, "[debug] save_jpg: mmap length=%zu buffer=%p\n",
		mmap_size, buffer);

	/* Drop privileges, to write JPEG with user rights */
	seteuid(getuid());

	jpgfile = fopen(jpg_fn, "w+");
	if (!jpgfile) {
		ret = -errno;
		goto out_unmap_buffer;
	}

	// Copy framebuffer using pitch to a linear buffer, then convert to rgb888.
	for (i = 0; i < fb->height; i++)
		memcpy((uint8_t *)linear + i * fb->width * bytes_per_pixel,
		       (uint8_t *)buffer + i * pitch,
		       fb->width * bytes_per_pixel);

	convert_to_24(fb, picture, linear);

	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	jpeg_stdio_dest(&cinfo, jpgfile);

	cinfo.image_width = fb->width;
	cinfo.image_height = fb->height;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;

	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	while (cinfo.next_scanline < cinfo.image_height) {
		row_pointer[0] = (JSAMPROW)((uint8_t *)picture +
				cinfo.next_scanline * fb->width * 3);
		jpeg_write_scanlines(&cinfo, row_pointer, 1);
	}

	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);

	ret = 0;

	fclose(jpgfile);
out_unmap_buffer:
	munmap(buffer, mmap_size);
out_free_linear:
	free(linear);
out_free_picture:
	free(picture);
	return ret;
}

int main(int argc, char **argv)
{
	int err, drm_fd, prime_fd, retval = EXIT_FAILURE;
	unsigned int i, card;
	uint32_t fb_id, crtc_id;
	uint32_t plane_id = 0;
	drmModePlaneRes *plane_res;
	drmModePlane *plane;
	drmModeFB *fb;
	drmModeFB2 *fb2;
	uint32_t handle, pitch;
	char buf[256];
	uint64_t has_dumb;

	if (argc < 2) {
		printf("Usage: kmsgrab <output.png|output.jpg>\n");
		goto out_return;
	}

	for (card = 0; ; card++) {
		snprintf(buf, sizeof(buf), "/dev/dri/card%u", card);

		drm_fd = open(buf, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0) {
			fprintf(stderr, "Could not open KMS/DRM device.\n");
			goto out_return;
		}

		if (drmGetCap(drm_fd, DRM_CAP_DUMB_BUFFER, &has_dumb) >= 0 &&
		    has_dumb)
			break;

		close(drm_fd);
	}

	drm_fd = open(buf, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		fprintf(stderr, "Could not open KMS/DRM device.\n");
		goto out_return;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		fprintf(stderr, "Unable to set atomic cap.\n");
		goto out_close_fd;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
		fprintf(stderr, "Unable to set universal planes cap.\n");
		goto out_close_fd;
	}

	plane_res = drmModeGetPlaneResources(drm_fd);
	if (!plane_res) {
		fprintf(stderr, "Unable to get plane resources.\n");
		goto out_close_fd;
	}

	for (i = 0; i < plane_res->count_planes; i++) {
		plane = drmModeGetPlane(drm_fd, plane_res->planes[i]);
		if (!plane) {
			fprintf(stderr, "[debug] plane[%u] id=%"PRIu32": drmModeGetPlane failed\n",
				i, plane_res->planes[i]);
			continue;
		}
		fprintf(stderr, "[debug] plane[%u] id=%"PRIu32" fb_id=%"PRIu32" crtc_id=%"PRIu32" crtc_x=%"PRIu32" crtc_y=%"PRIu32"\n",
			i, plane->plane_id, plane->fb_id, plane->crtc_id,
			plane->crtc_x, plane->crtc_y);
		fb_id = plane->fb_id;
		crtc_id = plane->crtc_id;
		plane_id = plane->plane_id;
		drmModeFreePlane(plane);

		if (fb_id != 0 && crtc_id != 0)
			break;
	}

	if (i == plane_res->count_planes) {
		fprintf(stderr, "No planes found\n");
		goto out_free_resources;
	}

	fb = drmModeGetFB(drm_fd, fb_id);
	if (!fb) {
		fprintf(stderr, "Failed to get framebuffer %"PRIu32": %s\n",
			fb_id, strerror(errno));
		goto out_free_resources;
	}

	fprintf(stderr, "[debug] using plane_id=%"PRIu32" fb_id=%"PRIu32" crtc_id=%"PRIu32"\n",
		plane_id, fb_id, crtc_id);

	fb2 = drmModeGetFB2(drm_fd, fb_id);
	if (!fb2) {
		fprintf(stderr, "[debug] drmModeGetFB2 failed for fb_id=%"PRIu32": %s\n",
			fb_id, strerror(errno));
		handle = fb->handle;
		pitch = fb->width * (fb->bpp >> 3);
	} else {
		fprintf(stderr, "[debug] fb2: w=%"PRIu32" h=%"PRIu32" pixel_format=0x%"PRIx32" flags=0x%"PRIx32"\n",
			fb2->width, fb2->height, fb2->pixel_format, fb2->flags);
		fprintf(stderr, "[debug] fb2: handles={%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"}\n",
			fb2->handles[0], fb2->handles[1], fb2->handles[2], fb2->handles[3]);
		fprintf(stderr, "[debug] fb2: pitches={%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"}\n",
			fb2->pitches[0], fb2->pitches[1], fb2->pitches[2], fb2->pitches[3]);
		fprintf(stderr, "[debug] fb2: offsets={%"PRIu32",%"PRIu32",%"PRIu32",%"PRIu32"}\n",
			fb2->offsets[0], fb2->offsets[1], fb2->offsets[2], fb2->offsets[3]);
		fprintf(stderr, "[debug] fb2: modifier not printed (libdrm ABI varies)\n");
		handle = fb2->handles[0];
		pitch = fb2->pitches[0];
		drmModeFreeFB2(fb2);
	}

	err = drmPrimeHandleToFD(drm_fd, handle, O_RDONLY, &prime_fd);
	if (err < 0) {
		fprintf(stderr, "Failed to retrieve prime handler: %s\n",
			strerror(-err));
		goto out_free_fb;
	}

	if (strstr(argv[1], ".jpg") || strstr(argv[1], ".jpeg"))
		err = save_jpg(fb, prime_fd, pitch, argv[1], 90);
	else
		err = save_png(fb, prime_fd, pitch, argv[1]);
	if (err < 0) {
		fprintf(stderr, "Failed to take screenshot: %s\n",
			strerror(-err));
		goto out_close_prime_fd;
	}

	retval = EXIT_SUCCESS;

out_close_prime_fd:
	close(prime_fd);
out_free_fb:
	drmModeFreeFB(fb);
out_free_resources:
	drmModeFreePlaneResources(plane_res);
out_close_fd:
	close(drm_fd);
out_return:
	return retval;
}
