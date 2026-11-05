/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

typedef struct {
    uint32_t buffer_lines;
    uint32_t tick_ms;
    uint32_t task_period_ms;
} display_service_config_t;

typedef struct display_service_session_t *display_service_session_handle_t;

typedef enum {
    DISPLAY_SERVICE_PANEL_INTERFACE_SPI = 0,
    DISPLAY_SERVICE_PANEL_INTERFACE_I80,
    DISPLAY_SERVICE_PANEL_INTERFACE_RGB,
    DISPLAY_SERVICE_PANEL_INTERFACE_DSI,
    DISPLAY_SERVICE_PANEL_INTERFACE_PARLIO,
} display_service_panel_interface_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t bits_per_pixel;
    display_service_panel_interface_t panel_interface;
    bool touch_available;
} display_service_info_t;

typedef enum {
    DISPLAY_SERVICE_MODE_SHARED_LVGL = 0,
    DISPLAY_SERVICE_MODE_EXCLUSIVE_LVGL,
    DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
} display_service_mode_t;

typedef enum {
    DISPLAY_SERVICE_SESSION_FLAG_RESTORE_DEFAULT_ON_RELEASE = 1 << 0,
    DISPLAY_SERVICE_SESSION_FLAG_ALLOW_SYSTEM_OVERLAY       = 1 << 1,
} display_service_session_flags_t;

typedef void (*display_service_session_cleanup_cb_t)(display_service_session_handle_t session, void *user_ctx);
typedef void (*display_service_session_exit_request_cb_t)(display_service_session_handle_t session, void *user_ctx);

typedef struct {
    /** Stable, non-empty name used for diagnostics and ownership checks. */
    const char *owner_name;
    /** Shared LVGL or the exclusive LVGL/RAW presentation mode. */
    display_service_mode_t mode;
    /** Bitwise OR of display_service_session_flags_t values. */
    uint32_t flags;
    /** Service settings used if this session starts the display service. */
    display_service_config_t display_config;
    /** Optional cleanup invoked while the session is being closed. */
    display_service_session_cleanup_cb_t cleanup_cb;
    /** Optional non-blocking request to exit an exclusive session. */
    display_service_session_exit_request_cb_t exit_request_cb;
    /** Opaque context passed unchanged to both session callbacks. */
    void *user_ctx;
} display_service_session_config_t;

typedef struct {
    int x_start;
    int y_start;
    int x_end;
    int y_end;
    const void *frame_buffer;
    /** Source row stride in bytes; zero means tightly packed rows. */
    size_t stride_bytes;
    bool wait;
} display_service_raw_blit_t;

#define DISPLAY_SERVICE_OWNER_NAME_LEN 32

typedef struct {
    uint8_t id;
    int32_t x;
    int32_t y;
} display_service_touch_point_t;

typedef struct {
    uint8_t count;
    display_service_touch_point_t points[1];
} display_service_touch_snapshot_t;

esp_err_t display_service_session_get_info(display_service_session_handle_t session, display_service_info_t *info);
esp_err_t display_service_session_raw_blit(display_service_session_handle_t session, const display_service_raw_blit_t *blit);
esp_err_t display_service_session_get_touch_snapshot(display_service_session_handle_t session, display_service_touch_snapshot_t *snapshot);

esp_err_t display_service_start(const display_service_config_t *config);
void display_service_stop(void);
bool display_service_is_started(void);
esp_err_t display_service_open(const display_service_session_config_t *config, display_service_session_handle_t *ret_session);
esp_err_t display_service_close(display_service_session_handle_t session);
bool display_service_session_is_valid(display_service_session_handle_t session);
lv_display_t *display_service_session_display(display_service_session_handle_t session);
esp_err_t display_service_session_load_screen(display_service_session_handle_t session, lv_obj_t *screen);
esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t session, lv_obj_t *screen);
bool display_service_has_exclusive_session(void);
esp_err_t display_service_lock(void);
void display_service_unlock(void);
