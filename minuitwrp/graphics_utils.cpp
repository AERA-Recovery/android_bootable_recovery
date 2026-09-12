/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>
#include <pixelflinger/pixelflinger.h>
#include <linux/fb.h>
#include <string.h>

#include "minuitwrp/minui.h"

struct fb_var_screeninfo vi;
extern GGLSurface gr_mem_surface;
extern GRSurface* gr_draw;
extern unsigned int gr_rotation;

struct aera_jpeg_error {
    struct jpeg_error_mgr base;
    jmp_buf jump;
};

static void aera_jpeg_error_exit(j_common_ptr info)
{
    aera_jpeg_error *error = (aera_jpeg_error *)info->err;
    longjmp(error->jump, 1);
}

static bool gr_capture_source(GGLSurface *source)
{
    *source = gr_mem_surface;
    const gr_surface presented = gr_drm_get_presented_surface();
    if (presented != NULL) {
        const GRSurface *drm_surface = (const GRSurface *)presented;
        source->version = sizeof(*source);
        source->width = drm_surface->width;
        source->height = drm_surface->height;
        source->stride = drm_surface->row_bytes / drm_surface->pixel_bytes;
        source->data = drm_surface->data;
        source->format = drm_surface->format;
    }
    return source->data != NULL && source->width != 0 &&
           source->height != 0 && source->stride != 0;
}

static bool gr_rgb_row(const GGLSurface *source, uint32_t source_y,
                       uint32_t output_width, uint8_t *destination)
{
    const uint32_t pixel_bytes =
        source->format == GGL_PIXEL_FORMAT_RGB_565 ? 2 : 4;
    const uint8_t *source_row = source->data +
        (size_t)source_y * source->stride * pixel_bytes;
    uint8_t *dst = destination;
    for (uint32_t x = 0; x < output_width; ++x) {
        const uint32_t source_x = (uint32_t)((uint64_t)x * source->width /
                                              output_width);
        const uint8_t *src = source_row + (size_t)source_x * pixel_bytes;
        if (source->format == GGL_PIXEL_FORMAT_BGRA_8888) {
            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
        } else if (source->format == GGL_PIXEL_FORMAT_RGBA_8888 ||
                   source->format == GGL_PIXEL_FORMAT_RGBX_8888) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        } else if (source->format == GGL_PIXEL_FORMAT_RGB_565) {
            const uint16_t pixel = src[0] | ((uint16_t)src[1] << 8);
            dst[0] = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
            dst[1] = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
            dst[2] = (uint8_t)((pixel & 0x1f) * 255 / 31);
        } else {
            return false;
        }
        dst += 3;
    }
    return true;
}

static int gr_save_screenshot_internal(const char *dest,
                                       unsigned int max_width,
                                       bool fast)
{
    uint32_t y;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    volatile int res = -1;
    uint8_t * volatile png_row = NULL;
    FILE * volatile fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    GGLSurface capture_source;

    // DRM direct scanout does not use minui's legacy gr_draw surface. Use
    // the dimensions and mapped pixels of the actually presented buffer;
    // dereferencing gr_draw here caused a null-pointer crash in AERA's GPU
    // UI and left a zero-byte PNG behind.
    if (!gr_capture_source(&capture_source))
        goto exit;

    output_width = max_width > 0 && capture_source.width > max_width
        ? max_width : capture_source.width;
    output_height = output_width == capture_source.width
        ? capture_source.height
        : (uint32_t)(((uint64_t)capture_source.height * output_width +
                      capture_source.width / 2) / capture_source.width);

    fp = fopen(dest, "wb");
    if(!fp)
        goto exit;

    png_row = (uint8_t *)malloc((size_t)output_width * 3);
    if (!png_row) {
        printf("gr_save_screenshot failed to allocate PNG row\n");
        goto exit;
    }

    png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr)
        goto exit;

    info_ptr = png_create_info_struct(png_ptr);
    if (info_ptr == NULL)
        goto exit;

    if (setjmp(png_jmpbuf(png_ptr)))
        goto exit;

    png_init_io(png_ptr, fp);
    if (fast) {
        png_set_compression_level(png_ptr, 1);
        png_set_filter(png_ptr, PNG_FILTER_TYPE_BASE, PNG_FILTER_NONE);
    }
    png_set_IHDR(png_ptr, info_ptr, output_width, output_height,
         8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
         PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png_ptr, info_ptr);

    // AERA presents a mapped DRM scanout buffer. Converting that buffer
    // directly avoids Pixelflinger, whose GGL context is not valid while the
    // LVGL/Adreno UI owns direct scanout. The old GGL path either returned an
    // empty image or dereferenced a null legacy draw surface.
    for (y = 0; y < output_height; ++y) {
        const uint32_t source_y = (uint32_t)((uint64_t)y * capture_source.height /
                                              output_height);
        if (!gr_rgb_row(&capture_source, source_y, output_width,
                        (uint8_t *)png_row))
            goto exit;
        png_write_row(png_ptr, (png_bytep)png_row);
    }

    png_write_end(png_ptr, NULL);

    res = 0;
exit:
    if(info_ptr)
        png_free_data(png_ptr, info_ptr, PNG_FREE_ALL, -1);
    if(png_ptr)
        png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
    if(png_row)
        free((void *)png_row);
    if(fp)
        fclose(fp);
    if (res != 0)
        remove(dest);
    return res;
}

int gr_save_screenshot(const char *dest)
{
    return gr_save_screenshot_internal(dest, 0, false);
}

int gr_save_screenshot_scaled_fast(const char *dest, unsigned int max_width)
{
    return gr_save_screenshot_internal(dest, max_width, true);
}

int gr_save_screenshot_scaled_jpeg(const char *dest, unsigned int max_width,
                                   int quality)
{
    GGLSurface source;
    if (!gr_capture_source(&source))
        return -1;
    const uint32_t width = max_width > 0 && source.width > max_width
        ? max_width : source.width;
    const uint32_t height = width == source.width ? source.height :
        (uint32_t)(((uint64_t)source.height * width + source.width / 2) /
                   source.width);
    FILE *file = fopen(dest, "wb");
    if (!file)
        return -1;
    uint8_t *row = (uint8_t *)malloc((size_t)width * 3);
    if (!row) {
        fclose(file);
        remove(dest);
        return -1;
    }

    jpeg_compress_struct encoder = {};
    aera_jpeg_error error = {};
    encoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = aera_jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_compress(&encoder);
        free(row);
        fclose(file);
        remove(dest);
        return -1;
    }
    jpeg_create_compress(&encoder);
    jpeg_stdio_dest(&encoder, file);
    encoder.image_width = width;
    encoder.image_height = height;
    encoder.input_components = 3;
    encoder.in_color_space = JCS_RGB;
    jpeg_set_defaults(&encoder);
    if (quality < 35) quality = 35;
    if (quality > 95) quality = 95;
    jpeg_set_quality(&encoder, quality, TRUE);
    encoder.dct_method = JDCT_IFAST;
    jpeg_start_compress(&encoder, TRUE);
    while (encoder.next_scanline < encoder.image_height) {
        const uint32_t source_y = (uint32_t)(
            (uint64_t)encoder.next_scanline * source.height / height);
        if (!gr_rgb_row(&source, source_y, width, row)) {
            jpeg_abort_compress(&encoder);
            jpeg_destroy_compress(&encoder);
            free(row);
            fclose(file);
            remove(dest);
            return -1;
        }
        JSAMPROW scanline = row;
        jpeg_write_scanlines(&encoder, &scanline, 1);
    }
    jpeg_finish_compress(&encoder);
    jpeg_destroy_compress(&encoder);
    free(row);
    fclose(file);
    return 0;
}

int ROTATION_X_DISP(int x, int y, int w) {
    return ((gr_rotation ==   0) ? (x) :
            (gr_rotation ==  90) ? (w - (y) - 1) :
            (gr_rotation == 180) ? (w - (x) - 1) :
            (gr_rotation == 270) ? (y) : -1);
}

int ROTATION_Y_DISP(int x, int y, int h) {
    return ((gr_rotation ==   0) ? (y) :
            (gr_rotation ==  90) ? (x) :
            (gr_rotation == 180) ? (h - (y) - 1) :
            (gr_rotation == 270) ? (h - (x) - 1) : -1);
}

#define MATRIX_ELEMENT(matrix, row, col, row_size, elem_size) \
    (((uint8_t*) (matrix)) + (((row) * (elem_size)) * (row_size)) + ((col) * (elem_size)))

#define DO_MATRIX_ROTATION(bits_per_pixel, bytes_per_pixel)                   \
{                                                                             \
    for (size_t y = 0; y < src->height; y++) {                                \
        for (size_t x = 0; x < src->width; x++) {                             \
            /* output pointer in dst->data */                                 \
            uint##bits_per_pixel##_t       *op;                               \
            /* input pointer from src->data */                                \
            const uint##bits_per_pixel##_t *ip;                               \
            /* Display coordinates (in dst) corresponding to (x, y) in src */ \
            size_t x_disp = ROTATION_X_DISP(x, y, dst->width);                \
            size_t y_disp = ROTATION_Y_DISP(x, y, dst->height);               \
                                                                              \
            ip = (const uint##bits_per_pixel##_t*)                            \
                 MATRIX_ELEMENT(src->data, y, x,                              \
                                src->stride, bytes_per_pixel);                \
            op = (uint##bits_per_pixel##_t*)                                  \
                 MATRIX_ELEMENT(dst->data, y_disp, x_disp,                    \
                                dst->stride, bytes_per_pixel);                \
            *op = *ip;                                                        \
        }                                                                     \
    }                                                                         \
}

void surface_ROTATION_transform(gr_surface dst_ptr, const gr_surface src_ptr,
                                  size_t num_bytes_per_pixel)
{
    GGLSurface *dst = (GGLSurface*) dst_ptr;
    const GGLSurface *src = (GGLSurface*) src_ptr;

    /* Handle duplicated code via a macro.
     * This is currently used for rotating surfaces of graphical resources
     * (32-bit pixel format) and of font glyphs (8-bit pixel format).
     * If you need to add handling of other pixel formats feel free to do so.
     */
    if (num_bytes_per_pixel == 4) {
        DO_MATRIX_ROTATION(32, 4);
    } else if (num_bytes_per_pixel == 1) {
        DO_MATRIX_ROTATION(8, 1);
    }
}

void gr_draw_rect(int x, int y, int w, int h, int thickness) {
    // Top border
    gr_line(x, y, x + w - 1, y, thickness);
    // Left border
    gr_line(x, y, x, y + h - 1, thickness);
    // Right border
    gr_line(x + w - 1, y, x + w - 1, y + h - 1, thickness);
    // Bottom border
    gr_line(x, y + h - 1, x + w - 1, y + h - 1, thickness);
}
