/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "display_raster.h"

typedef enum {
    DISPLAY_BITMAP_RGB565 = 0, /* Little-endian RGB565. */
    DISPLAY_BITMAP_BGR888,
    DISPLAY_BITMAP_RGB888,
} display_bitmap_format_t;

typedef enum {
    DISPLAY_BITMAP_RAW = 0,
    DISPLAY_BITMAP_CONTAIN,
    DISPLAY_BITMAP_COVER,
    DISPLAY_BITMAP_STRETCH,
    DISPLAY_BITMAP_CROP,
} display_bitmap_mode_t;

typedef struct {
    const uint8_t *pixels;
    size_t length, stride;
    int width, height;
    display_bitmap_format_t format;
} display_bitmap_view_t;

typedef struct {
    display_bitmap_mode_t mode;
    int width, height;
    int source_x, source_y, source_width, source_height;
    uint8_t opacity;
} display_bitmap_options_t;

/* Zero dimensions select the source extent; NULL options mean opaque RAW. */
esp_err_t display_bitmap_draw(display_raster_t *r, int x, int y, const display_bitmap_view_t *view,
                              const display_bitmap_options_t *options);
