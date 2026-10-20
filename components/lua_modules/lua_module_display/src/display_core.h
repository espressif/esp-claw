/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "display_color.h"
#include "display_raster.h"
#include "display_service.h"
#include "esp_err.h"

typedef struct display_t *display_handle_t;

typedef enum {
    DISPLAY_PIXEL_FORMAT_RGB565 = 0,
    DISPLAY_PIXEL_FORMAT_RGB888,
} display_pixel_format_t;

typedef struct {
    display_service_session_handle_t session;
    display_service_info_t info;
    display_pixel_format_t pixel_format;
    uint8_t framebuffer_count;
} display_config_t;

typedef struct {
    uint32_t draw_us;
    uint32_t present_us;
    uint32_t sync_us;
    size_t dirty_pixels;
    size_t dirty_rects;
    size_t submitted_bytes;
    size_t framebuffer_bytes;
} display_stats_t;

esp_err_t display_create(const display_config_t *config, display_handle_t *ret_handle);
esp_err_t display_delete(display_handle_t handle);
/* Owner-only terminal handoff; the recipient may only delete the detached handle. */
esp_err_t display_detach_for_cleanup(display_handle_t handle);
esp_err_t display_begin(display_handle_t handle, bool clear, display_color_t color);
esp_err_t display_present(display_handle_t handle, bool full, bool *updated);
esp_err_t display_save(display_handle_t handle);
esp_err_t display_restore(display_handle_t handle);
esp_err_t display_translate(display_handle_t handle, int dx, int dy);
esp_err_t display_clip(display_handle_t handle, int x, int y, int w, int h);
bool display_frame_active(display_handle_t handle);
display_raster_t *display_draw_view(display_handle_t handle);
const display_config_t *display_get_config(display_handle_t handle);
const display_stats_t *display_get_stats(display_handle_t handle);
