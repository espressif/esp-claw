/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "display_raster.h"

typedef struct display_font_t *display_font_handle_t;

typedef struct {
    display_font_handle_t font;
    /* ASCII height 8..64; width is aspect-preserving, rounded to nearest. DFN1 uses native metrics. */
    int font_size;
    display_color_t color;
} display_text_options_t;

/* DFN1 LE header: magic[4], ascent:i16, descent:i16, default_advance:i16, reserved:u16, count:u32, fallback:u32.
 * Each sorted record: codepoint:u32, bitmap_offset:u32, advance:i16, x_offset:i16, y_offset:i16, width:u16, height:u16.
 * Records are followed by tightly packed row-aligned MSB-first 1bpp bitmaps. */
esp_err_t display_font_create(const char *path, display_font_handle_t *ret_font);
void display_font_delete(display_font_handle_t font);
esp_err_t display_text_measure(const char *text, size_t length, const display_text_options_t *options, int *width, int *height);
esp_err_t display_text_draw(display_raster_t *r, int x, int y, const char *text, size_t length, const display_text_options_t *options);
