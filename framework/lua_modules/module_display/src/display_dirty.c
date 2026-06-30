/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_dirty.h"

#include "esp_log.h"

static const char *TAG = "display_dirty";

void display_dirty_clear(display_dirty_rect_t *dirty)
{
    if (!dirty) {
        ESP_LOGE(TAG, "dirty rect output is NULL");
        return;
    }
    dirty->valid = false;
    dirty->x = 0;
    dirty->y = 0;
    dirty->width = 0;
    dirty->height = 0;
}

bool display_dirty_is_valid(const display_dirty_rect_t *dirty)
{
    return dirty != NULL && dirty->valid && dirty->width > 0 && dirty->height > 0;
}

void display_dirty_mark(display_dirty_rect_t *dirty, int x, int y, int width, int height)
{
    if (!dirty) {
        ESP_LOGE(TAG, "dirty rect output is NULL");
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    if (!display_dirty_is_valid(dirty)) {
        dirty->valid = true;
        dirty->x = x;
        dirty->y = y;
        dirty->width = width;
        dirty->height = height;
        return;
    }

    int left = dirty->x < x ? dirty->x : x;
    int top = dirty->y < y ? dirty->y : y;
    int right = dirty->x + dirty->width;
    int bottom = dirty->y + dirty->height;
    int new_right = x + width;
    int new_bottom = y + height;

    if (new_right > right) {
        right = new_right;
    }
    if (new_bottom > bottom) {
        bottom = new_bottom;
    }
    dirty->x = left;
    dirty->y = top;
    dirty->width = right - left;
    dirty->height = bottom - top;
}
