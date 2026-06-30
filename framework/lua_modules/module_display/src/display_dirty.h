/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    int x;
    int y;
    int width;
    int height;
} display_dirty_rect_t;

void display_dirty_clear(display_dirty_rect_t *dirty);
bool display_dirty_is_valid(const display_dirty_rect_t *dirty);
void display_dirty_mark(display_dirty_rect_t *dirty, int x, int y, int width, int height);

#ifdef __cplusplus
}
#endif
