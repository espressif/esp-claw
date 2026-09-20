/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "display_dirty.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display_core";
#define DISPLAY_STATE_STACK_DEPTH 8

typedef struct {
    int tx, ty;
    int x0, y0, x1, y1;
} display_draw_state_t;

struct display_t {
    display_config_t config;
    TaskHandle_t owner_task;
    size_t framebuffer_bytes;
    uint8_t *framebuffers[2];
    uint8_t draw_index, visible_index, depth;
    bool frame_active, panel_initialized;
    display_dirty_region_t dirty;
    display_dirty_region_t sync_dirty;
    display_raster_t raster;
    display_draw_state_t stack[DISPLAY_STATE_STACK_DEPTH];
    display_stats_t stats;
};

static bool display_is_owner(display_handle_t handle)
{
    return handle != NULL && handle->owner_task != NULL && handle->owner_task == xTaskGetCurrentTaskHandle();
}

static size_t display_bytes_per_pixel(display_pixel_format_t format)
{
    return format == DISPLAY_PIXEL_FORMAT_RGB565 ? 2 : format == DISPLAY_PIXEL_FORMAT_RGB888 ? 3 : 0;
}

static void display_reset_view(display_handle_t handle)
{
    display_raster_t *r = &handle->raster;
    r->pixels = handle->framebuffers[handle->draw_index];
    r->width = handle->config.info.width;
    r->height = handle->config.info.height;
    r->bpp = (int)display_bytes_per_pixel(handle->config.pixel_format);
    r->stride = (size_t)r->width * (size_t)r->bpp;
    r->tx = r->ty = 0;
    r->x0 = r->y0 = 0;
    r->x1 = r->width;
    r->y1 = r->height;
    r->dirty = &handle->dirty;
    handle->depth = 0;
}

esp_err_t display_create(const display_config_t *config, display_handle_t *ret_handle)
{
    if (config == NULL || ret_handle == NULL || config->session == NULL) return ESP_ERR_INVALID_ARG;
    *ret_handle = NULL;
    const size_t bpp = display_bytes_per_pixel(config->pixel_format);
    const size_t width = config->info.width, height = config->info.height;
    if (bpp == 0 || width == 0 || height == 0 || config->framebuffer_count < 1 || config->framebuffer_count > 2 ||
        config->info.bits_per_pixel != bpp * 8 || width > SIZE_MAX / height || width * height > SIZE_MAX / bpp ||
        width * height * bpp > SIZE_MAX / config->framebuffer_count) return ESP_ERR_INVALID_ARG;
    if (!display_service_session_is_valid(config->session)) return ESP_ERR_INVALID_STATE;
    display_handle_t handle = calloc(1, sizeof(*handle));
    if (handle == NULL) return ESP_ERR_NO_MEM;
    handle->config = *config;
    handle->owner_task = xTaskGetCurrentTaskHandle();
    handle->framebuffer_bytes = width * height * bpp;
    handle->stats.framebuffer_bytes = handle->framebuffer_bytes * config->framebuffer_count;
    for (uint8_t i = 0; i < config->framebuffer_count; ++i) {
        handle->framebuffers[i] = heap_caps_aligned_alloc(16, handle->framebuffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (handle->framebuffers[i] == NULL) {
            ESP_LOGE(TAG, "framebuffer allocation failed: %u bytes", (unsigned)handle->framebuffer_bytes);
            for (uint8_t j = 0; j < i; ++j) heap_caps_free(handle->framebuffers[j]);
            free(handle);
            return ESP_ERR_NO_MEM;
        }
        memset(handle->framebuffers[i], 0, handle->framebuffer_bytes);
    }
    handle->draw_index = config->framebuffer_count == 2 ? 1 : 0;
    display_dirty_clear(&handle->dirty);
    display_reset_view(handle);
    *ret_handle = handle;
    return ESP_OK;
}

esp_err_t display_delete(display_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (handle->owner_task != NULL && !display_is_owner(handle)) return ESP_ERR_INVALID_STATE;
    if (display_service_session_is_valid(handle->config.session)) {
        esp_err_t err = display_service_close(handle->config.session);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "display session close failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    for (uint8_t i = 0; i < 2; ++i) heap_caps_free(handle->framebuffers[i]);
    free(handle);
    return ESP_OK;
}

esp_err_t display_detach_for_cleanup(display_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (!display_is_owner(handle)) return ESP_ERR_INVALID_STATE;
    handle->frame_active = false;
    handle->owner_task = NULL;
    return ESP_OK;
}

esp_err_t display_begin(display_handle_t handle, bool clear, display_color_t color)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (!display_is_owner(handle)) return ESP_ERR_INVALID_STATE;
    if (handle->frame_active) return ESP_ERR_INVALID_STATE;
    if (handle->config.framebuffer_count == 2 && (!clear || color.a != 255) && display_dirty_is_valid(&handle->sync_dirty)) {
        /* After a swap, only the last frame's writes differ between buffers. */
        size_t bpp = display_bytes_per_pixel(handle->config.pixel_format);
        size_t stride = (size_t)handle->config.info.width * bpp;
        for (size_t i = 0; i < handle->sync_dirty.count; ++i) {
            const display_dirty_rect_t *dirty = &handle->sync_dirty.rects[i];
            size_t offset = (size_t)dirty->y * stride + (size_t)dirty->x * bpp;
            uint8_t *dst = handle->framebuffers[handle->draw_index] + offset;
            const uint8_t *src = handle->framebuffers[handle->visible_index] + offset;
            size_t row_bytes = (size_t)dirty->width * bpp;
            if (row_bytes == stride) {
                memcpy(dst, src, row_bytes * (size_t)dirty->height);
            } else {
                for (int row = 0; row < dirty->height; ++row) memcpy(dst + (size_t)row * stride, src + (size_t)row * stride, row_bytes);
            }
        }
    }
    display_dirty_clear(&handle->sync_dirty);
    handle->frame_active = true;
    display_dirty_clear(&handle->dirty);
    display_reset_view(handle);
    if (clear) display_raster_fill_rect(&handle->raster, 0, 0, handle->raster.width, handle->raster.height, color);
    return ESP_OK;
}

static esp_err_t display_submit(display_handle_t handle, int x, int y, int w, int h)
{
    const size_t bpp = display_bytes_per_pixel(handle->config.pixel_format);
    const size_t stride = (size_t)handle->config.info.width * bpp;
    const uint8_t *framebuffer = handle->framebuffers[handle->draw_index];
    return display_service_session_raw_blit(handle->config.session, &(display_service_raw_blit_t) {
        .x_start = x, .y_start = y, .x_end = x + w, .y_end = y + h,
        .frame_buffer = framebuffer + (size_t)y * stride + (size_t)x * bpp,
        .stride_bytes = stride, .wait = true,
    });
}

esp_err_t display_present(display_handle_t handle, bool full, bool *updated)
{
    if (handle == NULL || updated == NULL) return ESP_ERR_INVALID_ARG;
    if (!display_is_owner(handle)) return ESP_ERR_INVALID_STATE;
    if (!handle->frame_active) return ESP_ERR_INVALID_STATE;
    *updated = false;
    if (!full && !display_dirty_is_valid(&handle->dirty)) {
        handle->frame_active = false;
        handle->stats.present_us = 0;
        handle->stats.dirty_pixels = 0;
        return ESP_OK;
    }
    bool submit_full = full || !handle->panel_initialized;
    int64_t start = esp_timer_get_time();
    if (submit_full) {
        esp_err_t err = display_submit(handle, 0, 0, handle->config.info.width, handle->config.info.height);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "present failed: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        for (size_t i = 0; i < handle->dirty.count; ++i) {
            const display_dirty_rect_t *rect = &handle->dirty.rects[i];
            esp_err_t err = display_submit(handle, rect->x, rect->y, rect->width, rect->height);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "present failed: %s", esp_err_to_name(err));
                return err;
            }
        }
    }
    handle->stats.present_us = (uint32_t)(esp_timer_get_time() - start);
    handle->stats.dirty_pixels = submit_full ? (size_t)handle->config.info.width * handle->config.info.height : display_dirty_total_pixels(&handle->dirty);
    handle->panel_initialized = true;
    handle->sync_dirty = handle->dirty;
    display_dirty_clear(&handle->dirty);
    handle->frame_active = false;
    if (handle->config.framebuffer_count == 2) {
        handle->visible_index = handle->draw_index;
        handle->draw_index ^= 1;
    }
    *updated = true;
    return ESP_OK;
}

esp_err_t display_save(display_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (!display_is_owner(handle)) return ESP_ERR_INVALID_STATE;
    if (!handle->frame_active || handle->depth >= DISPLAY_STATE_STACK_DEPTH) return ESP_ERR_INVALID_STATE;
    display_raster_t *r = &handle->raster;
    handle->stack[handle->depth++] = (display_draw_state_t) {r->tx, r->ty, r->x0, r->y0, r->x1, r->y1};
    return ESP_OK;
}

esp_err_t display_restore(display_handle_t handle)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (!display_is_owner(handle)) return ESP_ERR_INVALID_STATE;
    if (!handle->frame_active || handle->depth == 0) return ESP_ERR_INVALID_STATE;
    display_draw_state_t state = handle->stack[--handle->depth];
    display_raster_t *r = &handle->raster;
    r->tx = state.tx; r->ty = state.ty;
    r->x0 = state.x0; r->y0 = state.y0; r->x1 = state.x1; r->y1 = state.y1;
    return ESP_OK;
}

esp_err_t display_translate(display_handle_t handle, int dx, int dy)
{
    if (handle == NULL) return ESP_ERR_INVALID_ARG;
    if (!display_is_owner(handle)) return ESP_ERR_INVALID_STATE;
    if (!handle->frame_active || (int64_t)handle->raster.tx + dx > INT_MAX || (int64_t)handle->raster.tx + dx < INT_MIN ||
        (int64_t)handle->raster.ty + dy > INT_MAX || (int64_t)handle->raster.ty + dy < INT_MIN) return ESP_ERR_INVALID_STATE;
    handle->raster.tx += dx;
    handle->raster.ty += dy;
    return ESP_OK;
}

esp_err_t display_clip(display_handle_t handle, int x, int y, int w, int h)
{
    if (handle == NULL || w < 0 || h < 0) return ESP_ERR_INVALID_ARG;
    if (!display_is_owner(handle)) return ESP_ERR_INVALID_STATE;
    if (!handle->frame_active) return ESP_ERR_INVALID_STATE;
    display_raster_t *r = &handle->raster;
    int64_t x0 = (int64_t)x + r->tx, y0 = (int64_t)y + r->ty;
    int64_t x1 = x0 + w, y1 = y0 + h;
    if (x0 > r->x0) r->x0 = x0 > r->width ? r->width : (int)x0;
    if (y0 > r->y0) r->y0 = y0 > r->height ? r->height : (int)y0;
    if (x1 < r->x1) r->x1 = x1 < 0 ? 0 : (int)x1;
    if (y1 < r->y1) r->y1 = y1 < 0 ? 0 : (int)y1;
    if (r->x1 < r->x0) r->x1 = r->x0;
    if (r->y1 < r->y0) r->y1 = r->y0;
    return ESP_OK;
}

bool display_frame_active(display_handle_t handle) { return display_is_owner(handle) && handle->frame_active; }
display_raster_t *display_draw_view(display_handle_t handle) { return display_frame_active(handle) ? &handle->raster : NULL; }
const display_config_t *display_get_config(display_handle_t handle) { return display_is_owner(handle) ? &handle->config : NULL; }
const display_stats_t *display_get_stats(display_handle_t handle) { return display_is_owner(handle) ? &handle->stats : NULL; }
