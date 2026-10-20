/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_dirty.h"

#include <stdint.h>
#include "esp_log.h"

static const char *TAG = "display_dirty";
#define DISPLAY_DIRTY_MERGE_OVERHEAD 256

static size_t rect_area(const display_dirty_rect_t *rect)
{
    return (size_t)rect->width * (size_t)rect->height;
}

static display_dirty_rect_t rect_union(const display_dirty_rect_t *a, const display_dirty_rect_t *b)
{
    int left = a->x < b->x ? a->x : b->x;
    int top = a->y < b->y ? a->y : b->y;
    int right = a->x + a->width > b->x + b->width ? a->x + a->width : b->x + b->width;
    int bottom = a->y + a->height > b->y + b->height ? a->y + a->height : b->y + b->height;
    return (display_dirty_rect_t) {.x = left, .y = top, .width = right - left, .height = bottom - top};
}

static bool should_merge(const display_dirty_rect_t *a, const display_dirty_rect_t *b)
{
    display_dirty_rect_t merged = rect_union(a, b);
    return rect_area(&merged) <= rect_area(a) + rect_area(b) + DISPLAY_DIRTY_MERGE_OVERHEAD;
}

void display_dirty_clear(display_dirty_region_t *dirty)
{
    if (!dirty) {
        ESP_LOGE(TAG, "dirty region is NULL");
        return;
    }
    dirty->count = 0;
}

bool display_dirty_is_valid(const display_dirty_region_t *dirty)
{
    return dirty != NULL && dirty->count > 0;
}

size_t display_dirty_total_pixels(const display_dirty_region_t *dirty)
{
    size_t total = 0;
    if (dirty == NULL) return 0;
    for (size_t i = 0; i < dirty->count; ++i) total += rect_area(&dirty->rects[i]);
    return total;
}

void display_dirty_mark(display_dirty_region_t *dirty, int x, int y, int width, int height)
{
    if (!dirty) {
        ESP_LOGE(TAG, "dirty region is NULL");
        return;
    }
    if (width <= 0 || height <= 0) return;

    display_dirty_rect_t next = {.x = x, .y = y, .width = width, .height = height};
    for (size_t i = 0; i < dirty->count;) {
        if (!should_merge(&dirty->rects[i], &next)) {
            ++i;
            continue;
        }
        next = rect_union(&dirty->rects[i], &next);
        dirty->rects[i] = dirty->rects[--dirty->count];
    }
    if (dirty->count < DISPLAY_DIRTY_RECT_CAPACITY) {
        dirty->rects[dirty->count++] = next;
        return;
    }

    size_t best = 0;
    size_t best_growth = SIZE_MAX;
    for (size_t i = 0; i < dirty->count; ++i) {
        display_dirty_rect_t merged = rect_union(&dirty->rects[i], &next);
        size_t growth = rect_area(&merged) - rect_area(&dirty->rects[i]);
        if (growth < best_growth) {
            best = i;
            best_growth = growth;
        }
    }
    display_dirty_rect_t merged = rect_union(&dirty->rects[best], &next);
    dirty->rects[best] = dirty->rects[--dirty->count];
    display_dirty_mark(dirty, merged.x, merged.y, merged.width, merged.height);
}
