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
#include <png.h>
#include <pixelflinger/pixelflinger.h>
#include <linux/fb.h>
#include <string.h>

#include "minuitwrp/minui.h"

struct fb_var_screeninfo vi;
extern GGLSurface gr_mem_surface;
extern GRSurface* gr_draw;
extern unsigned int gr_rotation;

int gr_save_screenshot(const char *dest)
{
    uint32_t y;
    volatile int res = -1;
    uint8_t * volatile png_row = NULL;
    FILE * volatile fp = NULL;
    png_structp png_ptr = NULL;
    png_infop info_ptr = NULL;
    GGLSurface capture_source = gr_mem_surface;
    const gr_surface presented = gr_drm_get_presented_surface();
    if (presented != NULL) {
        const GRSurface *drm_surface = (const GRSurface *)presented;
        capture_source.version = sizeof(capture_source);
        capture_source.width = drm_surface->width;
        capture_source.height = drm_surface->height;
        capture_source.stride = drm_surface->row_bytes / drm_surface->pixel_bytes;
        capture_source.data = drm_surface->data;
        capture_source.format = drm_surface->format;
    }

    // DRM direct scanout does not use minui's legacy gr_draw surface. Use
    // the dimensions and mapped pixels of the actually presented buffer;
    // dereferencing gr_draw here caused a null-pointer crash in AERA's GPU
    // UI and left a zero-byte PNG behind.
    if (capture_source.data == NULL || capture_source.width == 0 ||
        capture_source.height == 0 || capture_source.stride == 0)
        goto exit;

    fp = fopen(dest, "wb");
    if(!fp)
        goto exit;

    png_row = (uint8_t *)malloc((size_t)capture_source.width * 3);
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
    png_set_IHDR(png_ptr, info_ptr, capture_source.width, capture_source.height,
         8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
         PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
    png_write_info(png_ptr, info_ptr);

    // AERA presents a mapped DRM scanout buffer. Converting that buffer
    // directly avoids Pixelflinger, whose GGL context is not valid while the
    // LVGL/Adreno UI owns direct scanout. The old GGL path either returned an
    // empty image or dereferenced a null legacy draw surface.
    for (y = 0; y < capture_source.height; ++y) {
        const uint8_t *src = capture_source.data +
            (size_t)y * capture_source.stride *
            ((capture_source.format == GGL_PIXEL_FORMAT_RGB_565) ? 2 : 4);
        uint8_t *dst = (uint8_t *)png_row;

        for (uint32_t x = 0; x < capture_source.width; ++x) {
            if (capture_source.format == GGL_PIXEL_FORMAT_BGRA_8888) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                src += 4;
            } else if (capture_source.format == GGL_PIXEL_FORMAT_RGBA_8888 ||
                       capture_source.format == GGL_PIXEL_FORMAT_RGBX_8888) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
                src += 4;
            } else if (capture_source.format == GGL_PIXEL_FORMAT_RGB_565) {
                const uint16_t pixel = src[0] | ((uint16_t)src[1] << 8);
                dst[0] = (uint8_t)(((pixel >> 11) & 0x1f) * 255 / 31);
                dst[1] = (uint8_t)(((pixel >> 5) & 0x3f) * 255 / 63);
                dst[2] = (uint8_t)((pixel & 0x1f) * 255 / 31);
                src += 2;
            } else {
                printf("gr_save_screenshot unsupported pixel format %d\n",
                       capture_source.format);
                goto exit;
            }
            dst += 3;
        }
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
