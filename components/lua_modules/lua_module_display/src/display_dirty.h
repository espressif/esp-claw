/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x;
    int y;
    int width;
    int height;
} display_dirty_rect_t;

#define DISPLAY_DIRTY_RECT_CAPACITY 4

typedef struct {
    display_dirty_rect_t rects[DISPLAY_DIRTY_RECT_CAPACITY];
    size_t count;
} display_dirty_region_t;

void display_dirty_clear(display_dirty_region_t *dirty);
bool display_dirty_is_valid(const display_dirty_region_t *dirty);
size_t display_dirty_total_pixels(const display_dirty_region_t *dirty);
void display_dirty_mark(display_dirty_region_t *dirty, int x, int y, int width, int height);

#ifdef __cplusplus
}
#endif
