/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "system_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "display_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_UI_TAG "system_ui"

#define SYSTEM_UI_DEFAULT_BUFFER_LINES 40
#define SYSTEM_UI_DEFAULT_TICK_MS 5
#define SYSTEM_UI_DEFAULT_TASK_PERIOD_MS 10
#define SYSTEM_UI_TASK_STACK 8192
#define SYSTEM_UI_TASK_PRIO 5
#define SYSTEM_UI_STOP_TIMEOUT_MS 5000
#define SYSTEM_UI_DEFAULT_FONT_PATH "fonts/NotoSansSC-Regular-sub.ttf"
#define SYSTEM_UI_DEFAULT_FONT_SIZE 24
#define SYSTEM_UI_CLOCK_FONT_SIZE 72
#define SYSTEM_UI_PATH_MAX 256
#define SYSTEM_UI_LAUNCHER_MAX_APPS 32
#define SYSTEM_UI_LAUNCHER_ID_LEN 32
#define SYSTEM_UI_LAUNCHER_TITLE_LEN 40
#define SYSTEM_UI_LAUNCHER_ARGS_LEN 384
#define SYSTEM_UI_LAUNCHER_MAX_TILES 16
#define SYSTEM_UI_JOBS_MAX_ITEMS 8
#define SYSTEM_UI_EVENT_QUEUE_LEN 12

#define SYSTEM_UI_JOB_TITLE_LEN SYSTEM_UI_TASK_TITLE_LEN
#define SYSTEM_UI_JOB_STATUS_LEN SYSTEM_UI_TASK_STATUS_LEN
#define SYSTEM_UI_JOB_DETAIL_LEN SYSTEM_UI_TASK_DETAIL_LEN
#define SYSTEM_UI_JOB_ID_LEN SYSTEM_UI_TASK_ID_LEN

typedef system_ui_launcher_item_t system_ui_launcher_selection_t;
typedef system_ui_task_item_t system_ui_job_item_t;
typedef system_ui_tasks_provider_cb_t system_ui_jobs_provider_cb_t;
typedef system_ui_task_action_cb_t system_ui_job_action_cb_t;
typedef system_ui_tasks_stop_all_cb_t system_ui_jobs_stop_all_cb_t;

#ifndef SYSTEM_UI_THEME_PIXEL_MONO_INVERTED
#define SYSTEM_UI_THEME_PIXEL_MONO_INVERTED 0
#endif

#if SYSTEM_UI_THEME_PIXEL_MONO_INVERTED
#define SYSTEM_UI_COLOR_BG 0xf2f4f8
#define SYSTEM_UI_COLOR_SURFACE 0xe0e4ea
#define SYSTEM_UI_COLOR_PANEL 0xd0d6df
#define SYSTEM_UI_COLOR_LINE 0x687080
#define SYSTEM_UI_COLOR_FAINT 0x9aa2b0
#define SYSTEM_UI_COLOR_TEXT 0x0d0f14
#define SYSTEM_UI_COLOR_MUTED 0x3c4350
#define SYSTEM_UI_COLOR_BUTTON_BG 0x0d0f14
#define SYSTEM_UI_COLOR_BUTTON_TEXT 0xf2f4f8
#else
#define SYSTEM_UI_COLOR_BG 0x0d0f14
#define SYSTEM_UI_COLOR_SURFACE 0x161a24
#define SYSTEM_UI_COLOR_PANEL 0x1f2531
#define SYSTEM_UI_COLOR_LINE 0x3a4250
#define SYSTEM_UI_COLOR_FAINT 0x707888
#define SYSTEM_UI_COLOR_TEXT 0xf2f4f8
#define SYSTEM_UI_COLOR_MUTED 0x9aa2b0
#define SYSTEM_UI_COLOR_BUTTON_BG 0xf2f4f8
#define SYSTEM_UI_COLOR_BUTTON_TEXT 0x0d0f14
#endif

static inline lv_color_t system_ui_color(uint32_t hex)
{
    return lv_color_hex(hex);
}

static inline int32_t system_ui_min_i32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static inline int32_t system_ui_max_i32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

static inline int32_t system_ui_clamp_i32(int32_t value, int32_t min_value, int32_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static inline int32_t system_ui_short_side_from(uint32_t width, uint32_t height)
{
    return system_ui_min_i32((int32_t)width, (int32_t)height);
}

typedef struct system_ui_launcher_app_t {
    char id[SYSTEM_UI_LAUNCHER_ID_LEN];
    char title[SYSTEM_UI_LAUNCHER_TITLE_LEN];
    char action[SYSTEM_UI_PATH_MAX];
    char icon_path[SYSTEM_UI_PATH_MAX];
    char args_json[SYSTEM_UI_LAUNCHER_ARGS_LEN];
    lv_image_dsc_t icon_dsc;
    uint8_t *icon_data;
    size_t icon_data_size;
    uint16_t icon_side;
    int order;
    struct system_ui_launcher_app_t *next;
} system_ui_launcher_app_t;

typedef struct {
    uint8_t rows;
    uint8_t cols;
} system_ui_launcher_layout_t;

typedef enum {
    SYSTEM_UI_WORK_EVENT_STOP = 0,
    SYSTEM_UI_WORK_EVENT_SHOW_JOBS,
    SYSTEM_UI_WORK_EVENT_LAUNCHER_SELECT,
    SYSTEM_UI_WORK_EVENT_JOBS_REFRESH,
    SYSTEM_UI_WORK_EVENT_JOBS_ACTION,
    SYSTEM_UI_WORK_EVENT_NETWORK_STATUS,
    SYSTEM_UI_WORK_EVENT_APP_EXIT_SWIPE,
} system_ui_work_event_type_t;

typedef enum {
    SYSTEM_UI_TOUCH_GESTURE_NONE = 0,
    SYSTEM_UI_TOUCH_GESTURE_SHOW_JOBS,
    SYSTEM_UI_TOUCH_GESTURE_EXIT_APP,
} system_ui_touch_gesture_t;

typedef struct {
    system_ui_work_event_type_t type;
    uint32_t generation;
    union {
        system_ui_launcher_selection_t launcher_selection;
        struct {
            bool stop_all;
            char job_id[SYSTEM_UI_JOB_ID_LEN];
            system_ui_job_action_cb_t action_cb;
            void *action_user_ctx;
            system_ui_jobs_stop_all_cb_t stop_all_cb;
            void *stop_all_user_ctx;
        } jobs_action;
        struct {
            bool sta_connected;
            char ap_ssid[64];
        } network_status;
    };
    char launcher_id[SYSTEM_UI_LAUNCHER_ID_LEN];
    char launcher_title[SYSTEM_UI_LAUNCHER_TITLE_LEN];
    char launcher_action[SYSTEM_UI_PATH_MAX];
    char launcher_args_json[SYSTEM_UI_LAUNCHER_ARGS_LEN];
} system_ui_work_event_t;

typedef struct {
    lv_obj_t *row;
    lv_obj_t *title;
    lv_obj_t *status;
    lv_obj_t *detail;
    lv_obj_t *stop_button;
    lv_obj_t *stop_label;
    char job_id[SYSTEM_UI_JOB_ID_LEN];
} system_ui_jobs_row_t;

typedef struct {
    lv_obj_t *home_screen;
    lv_obj_t *home_tile;
    lv_obj_t *tileview;
    lv_obj_t *launcher_first_tile;
    lv_obj_t *status_label;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
    lv_timer_t *home_clock_timer;
    lv_obj_t *overlay_root;
    lv_obj_t *overlay_dot;
    lv_timer_t *overlay_timer;
    lv_obj_t *jobs_root;
    lv_obj_t *jobs_panel;
    lv_obj_t *jobs_list;
    lv_obj_t *jobs_empty_label;
    lv_obj_t *jobs_stop_all_button;
    lv_obj_t *jobs_stop_all_label;
    system_ui_jobs_provider_cb_t jobs_provider_cb;
    void *jobs_provider_user_ctx;
    system_ui_job_action_cb_t jobs_action_cb;
    void *jobs_action_user_ctx;
    system_ui_jobs_stop_all_cb_t jobs_stop_all_cb;
    void *jobs_stop_all_user_ctx;
    system_ui_app_exit_swipe_cb_t app_exit_swipe_cb;
    void *app_exit_swipe_user_ctx;
    bool jobs_stop_task_running;
    bool jobs_refresh_running;
    bool jobs_refresh_pending;
    bool jobs_snapshot_valid;
    size_t jobs_item_count;
    system_ui_job_item_t jobs_items[SYSTEM_UI_JOBS_MAX_ITEMS];
    system_ui_jobs_row_t jobs_rows[SYSTEM_UI_JOBS_MAX_ITEMS];
    system_ui_launcher_select_cb_t launcher_select_cb;
    void *launcher_select_user_ctx;
    system_ui_launcher_layout_t launcher_layout;
    system_ui_launcher_app_t *launcher_apps;
    lv_image_dsc_t launcher_default_icon_dsc;
    uint8_t *launcher_default_icon_data;
    size_t launcher_default_icon_data_size;
    size_t launcher_app_count;
    size_t launcher_page_count;
    lv_font_t *font;
    lv_font_t *clock_font;
    uint8_t *font_data;
    size_t font_data_size;
    uint32_t width;
    uint32_t height;
    SemaphoreHandle_t callback_mutex;
    QueueHandle_t event_queue;
    TaskHandle_t event_task;
    display_service_session_handle_t display_session;
    uint32_t generation;
    bool started;
    bool event_task_stop;
    system_ui_touch_gesture_t touch_gesture;
    bool jobs_visible;
    bool sta_connected;
    char ap_ssid[64];
    char font_path[SYSTEM_UI_PATH_MAX];
} system_ui_state_t;

extern system_ui_state_t s_ui;

static inline void system_ui_apply_font(lv_obj_t *obj)
{
    if (obj && s_ui.font) {
        lv_obj_set_style_text_font(obj, s_ui.font, 0);
    }
}

static inline void system_ui_apply_clock_font(lv_obj_t *obj)
{
    if (obj && s_ui.clock_font) {
        lv_obj_set_style_text_font(obj, s_ui.clock_font, 0);
    } else {
        system_ui_apply_font(obj);
    }
}

esp_err_t system_ui_create_home_locked(void);
void system_ui_home_update_locked(void);
void system_ui_delete_home_locked(void);
void system_ui_load_screen_locked(lv_obj_t *screen);

esp_err_t system_ui_launcher_load_locked(void);
esp_err_t system_ui_launcher_create_pages_locked(void);
void system_ui_launcher_delete_locked(void);

esp_err_t system_ui_create_overlay_locked(void);
void system_ui_delete_overlay_locked(void);

esp_err_t system_ui_vibration_init(void);
void system_ui_vibration_deinit(void);
void system_ui_add_click_feedback(lv_obj_t *obj);

esp_err_t system_ui_create_jobs_locked(void);
void system_ui_delete_jobs_locked(void);
void system_ui_jobs_set_visible_locked(bool visible);
void system_ui_jobs_request_refresh_locked(void);

void system_ui_dummy_draw_suspend_for_overlay_locked(void);
void system_ui_dummy_draw_resume_if_no_overlay_locked(void);
bool system_ui_system_overlay_allowed(void);

void system_ui_create_font_locked(const char *font_path, uint32_t font_size);
void system_ui_destroy_font_locked(void);

esp_err_t system_ui_post_work_event(const system_ui_work_event_t *event, TickType_t wait_ticks);
void system_ui_jobs_refresh_snapshot_locked(const system_ui_job_item_t *items, size_t count);
esp_err_t system_ui_lock(void);
void system_ui_unlock(void);
esp_err_t system_ui_callback_lock(void);
void system_ui_callback_unlock(void);

esp_err_t system_ui_set_network_status(bool sta_connected, const char *ap_ssid);
esp_err_t system_ui_overlay_set_visible(bool visible);
esp_err_t system_ui_launcher_set_select_callback(system_ui_launcher_select_cb_t cb, void *user_ctx);
esp_err_t system_ui_jobs_set_provider(system_ui_jobs_provider_cb_t cb, void *user_ctx);
esp_err_t system_ui_jobs_set_action_callback(system_ui_job_action_cb_t cb, void *user_ctx);
esp_err_t system_ui_jobs_set_stop_all_callback(system_ui_jobs_stop_all_cb_t cb, void *user_ctx);
esp_err_t system_ui_jobs_request_refresh(void);
esp_err_t system_ui_jobs_set_visible(bool visible);
bool system_ui_jobs_is_visible(void);

#ifdef __cplusplus
}
#endif
