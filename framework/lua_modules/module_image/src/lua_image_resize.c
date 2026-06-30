/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_image_resize.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#define LUA_IMAGE_RESIZE_MAX_PIXELS (1920U * 1080U)

static const char *TAG = "image_resize";

static inline int clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static esp_err_t lua_image_resize_alloc(size_t bytes, uint8_t **out)
{
    *out = (uint8_t *)heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (*out == NULL) {
        ESP_LOGE(TAG, "resize output alloc failed: %u bytes", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------------- */
/* GRAY8                                                                     */
/* ------------------------------------------------------------------------- */

static void resize_gray8_nearest(const uint8_t *src, int sw, int sh,
                                 uint8_t *dst, int dw, int dh)
{
    /* Top-left mapping with Q16 fixed-point step. */
    uint32_t x_step = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)dw);
    uint32_t y_step = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)dh);

    for (int y = 0; y < dh; y++) {
        uint32_t sy_q16 = (uint32_t)y * y_step;
        int sy = clamp_int((int)(sy_q16 >> 16), 0, sh - 1);
        const uint8_t *src_row = src + (size_t)sy * (size_t)sw;
        uint8_t *dst_row = dst + (size_t)y * (size_t)dw;
        uint32_t sx_q16 = 0;

        for (int x = 0; x < dw; x++) {
            int sx = clamp_int((int)(sx_q16 >> 16), 0, sw - 1);
            dst_row[x] = src_row[sx];
            sx_q16 += x_step;
        }
    }
}

static void resize_gray8_bilinear(const uint8_t *src, int sw, int sh,
                                  uint8_t *dst, int dw, int dh)
{
    uint32_t x_step = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)dw);
    uint32_t y_step = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)dh);

    for (int y = 0; y < dh; y++) {
        uint32_t sy_q16 = (uint32_t)y * y_step;
        int sy0 = clamp_int((int)(sy_q16 >> 16), 0, sh - 1);
        int sy1 = clamp_int(sy0 + 1, 0, sh - 1);
        uint32_t fy = sy_q16 & 0xFFFF;
        uint32_t inv_fy = 0x10000 - fy;
        const uint8_t *row0 = src + (size_t)sy0 * (size_t)sw;
        const uint8_t *row1 = src + (size_t)sy1 * (size_t)sw;
        uint8_t *dst_row = dst + (size_t)y * (size_t)dw;
        uint32_t sx_q16 = 0;

        for (int x = 0; x < dw; x++) {
            int sx0 = clamp_int((int)(sx_q16 >> 16), 0, sw - 1);
            int sx1 = clamp_int(sx0 + 1, 0, sw - 1);
            uint32_t fx = sx_q16 & 0xFFFF;
            uint32_t inv_fx = 0x10000 - fx;
            /* Two-step blend: horizontal first, then vertical, all in Q16. */
            uint32_t top = (row0[sx0] * inv_fx + row0[sx1] * fx) >> 16;
            uint32_t bot = (row1[sx0] * inv_fx + row1[sx1] * fx) >> 16;
            dst_row[x] = (uint8_t)((top * inv_fy + bot * fy) >> 16);
            sx_q16 += x_step;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* RGB565LE                                                                  */
/* ------------------------------------------------------------------------- */

static inline void rgb565_unpack(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* Expand to 8-bit by replicating high bits into the gap; matches the
     * inverse used in lua_image_convert.c. */
    uint16_t rr = (px >> 11) & 0x1f;
    uint16_t gg = (px >> 5) & 0x3f;
    uint16_t bb = px & 0x1f;

    *r = (uint8_t)((rr << 3) | (rr >> 2));
    *g = (uint8_t)((gg << 2) | (gg >> 4));
    *b = (uint8_t)((bb << 3) | (bb >> 2));
}

static inline uint16_t rgb888_pack_565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3));
}

static void resize_rgb565_nearest(const uint8_t *src, int sw, int sh,
                                  uint8_t *dst, int dw, int dh)
{
    const uint16_t *src16 = (const uint16_t *)src;
    uint16_t *dst16 = (uint16_t *)dst;
    uint32_t x_step = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)dw);
    uint32_t y_step = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)dh);

    for (int y = 0; y < dh; y++) {
        uint32_t sy_q16 = (uint32_t)y * y_step;
        int sy = clamp_int((int)(sy_q16 >> 16), 0, sh - 1);
        const uint16_t *src_row = src16 + (size_t)sy * (size_t)sw;
        uint16_t *dst_row = dst16 + (size_t)y * (size_t)dw;
        uint32_t sx_q16 = 0;

        for (int x = 0; x < dw; x++) {
            int sx = clamp_int((int)(sx_q16 >> 16), 0, sw - 1);
            dst_row[x] = src_row[sx];
            sx_q16 += x_step;
        }
    }
}

static void resize_rgb565_bilinear(const uint8_t *src, int sw, int sh,
                                   uint8_t *dst, int dw, int dh)
{
    const uint16_t *src16 = (const uint16_t *)src;
    uint16_t *dst16 = (uint16_t *)dst;
    uint32_t x_step = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)dw);
    uint32_t y_step = (uint32_t)(((uint64_t)sh << 16) / (uint32_t)dh);

    for (int y = 0; y < dh; y++) {
        uint32_t sy_q16 = (uint32_t)y * y_step;
        int sy0 = clamp_int((int)(sy_q16 >> 16), 0, sh - 1);
        int sy1 = clamp_int(sy0 + 1, 0, sh - 1);
        uint32_t fy = sy_q16 & 0xFFFF;
        uint32_t inv_fy = 0x10000 - fy;
        const uint16_t *row0 = src16 + (size_t)sy0 * (size_t)sw;
        const uint16_t *row1 = src16 + (size_t)sy1 * (size_t)sw;
        uint16_t *dst_row = dst16 + (size_t)y * (size_t)dw;
        uint32_t sx_q16 = 0;

        for (int x = 0; x < dw; x++) {
            int sx0 = clamp_int((int)(sx_q16 >> 16), 0, sw - 1);
            int sx1 = clamp_int(sx0 + 1, 0, sw - 1);
            uint32_t fx = sx_q16 & 0xFFFF;
            uint32_t inv_fx = 0x10000 - fx;
            uint8_t r00, g00, b00, r01, g01, b01, r10, g10, b10, r11, g11, b11;

            rgb565_unpack(row0[sx0], &r00, &g00, &b00);
            rgb565_unpack(row0[sx1], &r01, &g01, &b01);
            rgb565_unpack(row1[sx0], &r10, &g10, &b10);
            rgb565_unpack(row1[sx1], &r11, &g11, &b11);

            uint32_t top_r = ((uint32_t)r00 * inv_fx + (uint32_t)r01 * fx) >> 16;
            uint32_t top_g = ((uint32_t)g00 * inv_fx + (uint32_t)g01 * fx) >> 16;
            uint32_t top_b = ((uint32_t)b00 * inv_fx + (uint32_t)b01 * fx) >> 16;
            uint32_t bot_r = ((uint32_t)r10 * inv_fx + (uint32_t)r11 * fx) >> 16;
            uint32_t bot_g = ((uint32_t)g10 * inv_fx + (uint32_t)g11 * fx) >> 16;
            uint32_t bot_b = ((uint32_t)b10 * inv_fx + (uint32_t)b11 * fx) >> 16;
            uint32_t r = (top_r * inv_fy + bot_r * fy) >> 16;
            uint32_t g = (top_g * inv_fy + bot_g * fy) >> 16;
            uint32_t b = (top_b * inv_fy + bot_b * fy) >> 16;

            dst_row[x] = rgb888_pack_565(r, g, b);
            sx_q16 += x_step;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Public entry                                                              */
/* ------------------------------------------------------------------------- */

esp_err_t lua_image_resize_view(const lua_image_view_t *src,
                                int dst_width,
                                int dst_height,
                                lua_image_resize_filter_t filter,
                                lua_image_view_t *out)
{
    size_t bpp;
    size_t pixel_count;
    size_t output_bytes;
    uint8_t *buffer = NULL;
    esp_err_t err;

    if (src == NULL || out == NULL || src->data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (src->width <= 0 || src->height <= 0 || dst_width <= 0 || dst_height <= 0) {
        ESP_LOGE(TAG, "invalid resize dims: %dx%d -> %dx%d",
                 src->width, src->height, dst_width, dst_height);
        return ESP_ERR_INVALID_SIZE;
    }
    if ((size_t)dst_width > LUA_IMAGE_RESIZE_MAX_PIXELS / (size_t)dst_height) {
        ESP_LOGE(TAG, "resize destination exceeds pixel limit: %dx%d", dst_width, dst_height);
        return ESP_ERR_INVALID_SIZE;
    }

    switch (src->format) {
    case LUA_IMAGE_FORMAT_GRAY8:
        bpp = 1;
        break;
    case LUA_IMAGE_FORMAT_RGB565LE:
        bpp = 2;
        break;
    default:
        ESP_LOGE(TAG, "resize only supports GRAY8 / RGB565LE, got %s", lua_image_format_name(src->format));
        return ESP_ERR_NOT_SUPPORTED;
    }

    pixel_count = (size_t)src->width * (size_t)src->height;
    if (src->bytes < pixel_count * bpp) {
        ESP_LOGE(TAG, "resize source buffer too small: %u < %u", (unsigned)src->bytes, (unsigned)(pixel_count * bpp));
        return ESP_ERR_INVALID_SIZE;
    }

    output_bytes = (size_t)dst_width * (size_t)dst_height * bpp;
    err = lua_image_resize_alloc(output_bytes, &buffer);
    if (err != ESP_OK) {
        return err;
    }

    if (src->format == LUA_IMAGE_FORMAT_GRAY8) {
        if (filter == LUA_IMAGE_RESIZE_FILTER_BILINEAR) {
            resize_gray8_bilinear(src->data, src->width, src->height, buffer, dst_width, dst_height);
        } else {
            resize_gray8_nearest(src->data, src->width, src->height, buffer, dst_width, dst_height);
        }
    } else {
        if (filter == LUA_IMAGE_RESIZE_FILTER_BILINEAR) {
            resize_rgb565_bilinear(src->data, src->width, src->height, buffer, dst_width, dst_height);
        } else {
            resize_rgb565_nearest(src->data, src->width, src->height, buffer, dst_width, dst_height);
        }
    }

    out->data = buffer;
    out->bytes = output_bytes;
    out->width = dst_width;
    out->height = dst_height;
    out->format = src->format;
    out->owned = true;
    strlcpy(out->source_format, src->source_format, sizeof(out->source_format));
    return ESP_OK;
}
