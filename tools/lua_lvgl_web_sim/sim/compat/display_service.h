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

esp_err_t display_service_start(const display_service_config_t *config);
void display_service_stop(void);
bool display_service_is_started(void);
esp_err_t display_service_open(const display_service_session_config_t *config, display_service_session_handle_t *ret_session);
esp_err_t display_service_close(display_service_session_handle_t session);
bool display_service_session_is_valid(display_service_session_handle_t session);
lv_display_t *display_service_session_display(display_service_session_handle_t session);
esp_err_t display_service_session_load_screen(display_service_session_handle_t session, lv_obj_t *screen);
esp_err_t display_service_session_load_screen_locked(display_service_session_handle_t session, lv_obj_t *screen);
esp_err_t display_service_lock(void);
void display_service_unlock(void);
