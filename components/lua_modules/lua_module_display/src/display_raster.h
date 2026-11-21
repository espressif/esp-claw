/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "display_color.h"
#include "display_dirty.h"

/* Internal borrowed view; clip bounds are exclusive and in screen coordinates. */
typedef struct {
    uint8_t *pixels;
    int width, height, bpp;
    size_t stride;
    int tx, ty;
    int x0, y0, x1, y1;
    display_dirty_region_t *dirty;
} display_raster_t;

typedef struct {
    display_color_t color;
    uint16_t rgb565;
    uint32_t rgb888;
} display_raster_pen_t;

/* All drawing coordinates are local; each pixel is blended at most once per shape. */
display_raster_pen_t display_raster_make_pen(display_color_t color);
/* Screen coordinates must already be inside the current clip. */
void display_raster_run_unchecked(display_raster_t *r, int x, int y, size_t count, const display_raster_pen_t *pen);
void display_raster_pixel(display_raster_t *r, int64_t x, int64_t y, display_color_t color);
void display_raster_span(display_raster_t *r, int64_t x0, int64_t x1, int64_t y, display_color_t color);
void display_raster_fill_rect(display_raster_t *r, int x, int y, int w, int h, display_color_t color);
void display_raster_stroke_rect(display_raster_t *r, int x, int y, int w, int h, display_color_t color);
void display_raster_line(display_raster_t *r, int x0, int y0, int x1, int y1, display_color_t color);
void display_raster_fill_circle(display_raster_t *r, int cx, int cy, int radius, display_color_t color);
void display_raster_stroke_circle(display_raster_t *r, int cx, int cy, int radius, display_color_t color);
void display_raster_arc(display_raster_t *r, int cx, int cy, int radius, double start, double end, display_color_t color);
void display_raster_fill_round_rect(display_raster_t *r, int x, int y, int w, int h, int radius, display_color_t color);
void display_raster_stroke_round_rect(display_raster_t *r, int x, int y, int w, int h, int radius, display_color_t color);
void display_raster_fill_triangle(display_raster_t *r, int x0, int y0, int x1, int y1, int x2, int y2, display_color_t color);
