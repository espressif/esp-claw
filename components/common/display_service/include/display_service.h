/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t buffer_lines;
    uint32_t tick_ms;
    uint32_t task_period_ms;
} display_service_config_t;

typedef struct display_service_session_t *display_service_session_handle_t;

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

typedef struct {
    const char *owner_name;
    display_service_mode_t mode;
    uint32_t flags;
    display_service_config_t display_config;
    display_service_session_cleanup_cb_t cleanup_cb;
    void *user_ctx;
} display_service_session_config_t;

typedef struct {
    int x_start;
    int y_start;
    int x_end;
    int y_end;
    const void *frame_buffer;
    bool wait;
} display_service_raw_blit_t;

#define DISPLAY_SERVICE_OWNER_NAME_LEN 32

typedef struct {
    bool pressed;
    int32_t x;
    int32_t y;
} display_service_touch_sample_t;

typedef void (*display_service_touch_observer_cb_t)(const display_service_touch_sample_t *sample,
                                                    void *user_ctx);
typedef enum {
    DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_ENTERED = 0,
    DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_LVGL_EXITED,
    DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_ENTERED,
    DISPLAY_SERVICE_STATE_EVENT_EXCLUSIVE_RAW_EXITED,
} display_service_state_event_t;

typedef void (*display_service_state_observer_cb_t)(display_service_state_event_t event,
                                                    void *user_ctx);

esp_err_t display_service_start(const display_service_config_t *config);
void display_service_stop(void);
bool display_service_is_started(void);
esp_err_t display_service_open(const display_service_session_config_t *config,
                               display_service_session_handle_t *ret_session);
esp_err_t display_service_close(display_service_session_handle_t session);
bool display_service_session_is_valid(display_service_session_handle_t session);
bool display_service_session_is_active(display_service_session_handle_t session);
display_service_mode_t display_service_session_mode(display_service_session_handle_t session);
const char *display_service_session_owner_name(display_service_session_handle_t session);
esp_err_t display_service_session_load_screen(display_service_session_handle_t session, lv_obj_t *screen);
esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t session, lv_obj_t *screen);
lv_display_t *display_service_session_display(display_service_session_handle_t session);
esp_err_t display_service_session_raw_blit(display_service_session_handle_t session,
                                           const display_service_raw_blit_t *blit);
bool display_service_has_exclusive_session(void);
bool display_service_exclusive_allows_system_overlay(void);

esp_err_t display_service_lock(void);
void display_service_unlock(void);

esp_err_t display_service_set_touch_observer(display_service_touch_observer_cb_t cb,
                                             void *user_ctx);
esp_err_t display_service_set_state_observer(display_service_state_observer_cb_t cb,
                                             void *user_ctx);
esp_err_t display_service_set_default_screen(lv_obj_t *screen);
void display_service_set_default_screen_locked(lv_obj_t *screen);
lv_obj_t *display_service_default_screen(void);

void display_service_exclusive_raw_suspend_locked(void);
void display_service_exclusive_raw_resume_locked(void);

#ifdef __cplusplus
}
#endif
