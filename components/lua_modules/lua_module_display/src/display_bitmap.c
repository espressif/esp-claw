/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_bitmap.h"

#include <string.h>

static int64_t max64(int64_t a, int64_t b) { return a > b ? a : b; }
static int64_t min64(int64_t a, int64_t b) { return a < b ? a : b; }

static display_color_t read_color(const uint8_t *p, display_bitmap_format_t format, uint8_t opacity)
{
    display_color_t color = { .a = opacity };
    if (format == DISPLAY_BITMAP_RGB565) {
        uint16_t value = p[0] | ((uint16_t)p[1] << 8);
        unsigned r = value >> 11, g = (value >> 5) & 63, b = value & 31;
        color.r = (r << 3) | (r >> 2);
        color.g = (g << 2) | (g >> 4);
        color.b = (b << 3) | (b >> 2);
    } else {
        color.r = p[format == DISPLAY_BITMAP_RGB888 ? 0 : 2];
        color.g = p[1];
        color.b = p[format == DISPLAY_BITMAP_RGB888 ? 2 : 0];
    }
    return color;
}

static void write_color(uint8_t *dst, int bpp, display_color_t color)
{
    if (bpp == 2) {
        uint16_t value = display_color_blend_rgb565(dst[0] | ((uint16_t)dst[1] << 8), color);
        dst[0] = value;
        dst[1] = value >> 8;
    } else {
        unsigned a = color.a, inv = 255 - a;
        dst[0] = (color.b * a + dst[0] * inv + 127) / 255;
        dst[1] = (color.g * a + dst[1] * inv + 127) / 255;
        dst[2] = (color.r * a + dst[2] * inv + 127) / 255;
    }
}

esp_err_t display_bitmap_draw(display_raster_t *r, int x, int y, const display_bitmap_view_t *view,
                              const display_bitmap_options_t *options)
{
    if (!r || !r->pixels || r->width <= 0 || r->height <= 0 || (r->bpp != 2 && r->bpp != 3) ||
        !view || !view->pixels || view->width <= 0 || view->height <= 0 ||
        view->format < DISPLAY_BITMAP_RGB565 || view->format > DISPLAY_BITMAP_RGB888) {
        return ESP_ERR_INVALID_ARG;
    }
    const int bpp = view->format == DISPLAY_BITMAP_RGB565 ? 2 : 3;
    if ((size_t)view->width > SIZE_MAX / bpp || (size_t)r->width > SIZE_MAX / r->bpp) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t row_bytes = (size_t)view->width * bpp;
    size_t stride = view->stride ? view->stride : row_bytes;
    if (stride < row_bytes || (size_t)(view->height - 1) > (SIZE_MAX - row_bytes) / stride ||
        view->length < (size_t)(view->height - 1) * stride + row_bytes ||
        r->stride < (size_t)r->width * r->bpp || (size_t)r->height > SIZE_MAX / r->stride) {
        return ESP_ERR_INVALID_SIZE;
    }
    display_bitmap_options_t defaults = { .opacity = 255 };
    const display_bitmap_options_t *o = options ? options : &defaults;
    if (o->mode < DISPLAY_BITMAP_RAW || o->mode > DISPLAY_BITMAP_CROP || o->width < 0 || o->height < 0 ||
        o->source_x < 0 || o->source_y < 0 || o->source_x >= view->width || o->source_y >= view->height ||
        o->source_width < 0 || o->source_height < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int sx = o->source_x, sy = o->source_y;
    int sw = o->source_width ? o->source_width : view->width - sx;
    int sh = o->source_height ? o->source_height : view->height - sy;
    if (sw > view->width - sx || sh > view->height - sy) {
        return ESP_ERR_INVALID_SIZE;
    }
    int dw = o->width ? o->width : sw, dh = o->height ? o->height : sh;
    int64_t dx = (int64_t)x + r->tx, dy = (int64_t)y + r->ty;
    if (o->mode == DISPLAY_BITMAP_RAW) {
        dw = sw;
        dh = sh;
    } else if (o->mode == DISPLAY_BITMAP_CROP) {
        dw = sw = (int)min64(dw, sw);
        dh = sh = (int)min64(dh, sh);
    } else if (o->mode == DISPLAY_BITMAP_CONTAIN) {
        int w = dw, h = dh;
        if ((int64_t)dw * sh <= (int64_t)dh * sw) {
            dh = (int)max64(1, (int64_t)dw * sh / sw);
        } else {
            dw = (int)max64(1, (int64_t)dh * sw / sh);
        }
        dx += (w - dw) / 2;
        dy += (h - dh) / 2;
    } else if (o->mode == DISPLAY_BITMAP_COVER) {
        if ((int64_t)dw * sh > (int64_t)dh * sw) {
            int h = (int)max64(1, (int64_t)sw * dh / dw);
            sy += (sh - h) / 2;
            sh = h;
        } else {
            int w = (int)max64(1, (int64_t)sh * dw / dh);
            sx += (sw - w) / 2;
            sw = w;
        }
    }
    int64_t left = max64(dx, max64(0, r->x0)), top = max64(dy, max64(0, r->y0));
    int64_t right = min64(dx + dw, min64(r->width, r->x1));
    int64_t bottom = min64(dy + dh, min64(r->height, r->y1));
    if (left >= right || top >= bottom || o->opacity == 0) {
        return ESP_OK;
    }
    bool copy = o->opacity == 255 && sw == dw && sh == dh &&
                ((r->bpp == 2 && view->format == DISPLAY_BITMAP_RGB565) ||
                 (r->bpp == 3 && view->format == DISPLAY_BITMAP_BGR888));
    /* Exact integer stepping avoids division in the inner pixel loop. */
    int step = sw / dw, remainder = sw % dw;
    int source_left = sx + (int)((left - dx) * sw / dw);
    int64_t initial_error = (left - dx) * sw % dw;
    for (int yy = (int)top; yy < bottom; yy++) {
        int source_y = sy + (int)(((int64_t)yy - dy) * sh / dh);
        const uint8_t *row = view->pixels + (size_t)source_y * stride;
        uint8_t *dst = r->pixels + (size_t)yy * r->stride + (size_t)left * r->bpp;
        if (copy) {
            memcpy(dst, row + (size_t)source_left * bpp, (size_t)(right - left) * bpp);
            continue;
        }
        int source_x = source_left;
        int64_t error = initial_error;
        for (int xx = (int)left; xx < right; xx++, dst += r->bpp) {
            write_color(dst, r->bpp, read_color(row + (size_t)source_x * bpp, view->format, o->opacity));
            source_x += step;
            error += remainder;
            if (error >= dw) {
                source_x++;
                error -= dw;
            }
        }
    }
    display_dirty_mark(r->dirty, (int)left, (int)top, (int)(right - left), (int)(bottom - top));
    return ESP_OK;
}
