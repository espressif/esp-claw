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

#ifdef __cplusplus
extern "C" {
#endif

#define SYSTEM_UI_TASK_TITLE_LEN 48
#define SYSTEM_UI_TASK_STATUS_LEN 20
#define SYSTEM_UI_TASK_DETAIL_LEN 192
#define SYSTEM_UI_TASK_ID_LEN 16

typedef struct {
    uint32_t buffer_lines;
    uint32_t tick_ms;
    uint32_t task_period_ms;
    const char *font_path;
    uint32_t font_size;
} system_ui_config_t;

typedef struct {
    bool sta_connected;
    const char *ap_ssid;
} system_ui_network_state_t;

typedef struct {
    const char *id;
    const char *title;
    const char *action;
    const char *args_json;
} system_ui_launcher_item_t;

typedef struct {
    char id[SYSTEM_UI_TASK_ID_LEN];
    char title[SYSTEM_UI_TASK_TITLE_LEN];
    char status[SYSTEM_UI_TASK_STATUS_LEN];
    char detail[SYSTEM_UI_TASK_DETAIL_LEN];
} system_ui_task_item_t;

typedef void (*system_ui_launcher_select_cb_t)(const system_ui_launcher_item_t *item, void *user_ctx);
typedef size_t (*system_ui_tasks_provider_cb_t)(system_ui_task_item_t *out, size_t max, void *user_ctx);
typedef esp_err_t (*system_ui_task_action_cb_t)(const char *task_id, void *user_ctx);
typedef esp_err_t (*system_ui_tasks_stop_all_cb_t)(void *user_ctx);
typedef esp_err_t (*system_ui_app_exit_swipe_cb_t)(void *user_ctx);

typedef enum {
    SYSTEM_UI_PAGE_SWIPE_PREVIOUS = -1,
    SYSTEM_UI_PAGE_SWIPE_NEXT = 1,
} system_ui_page_swipe_direction_t;

typedef esp_err_t (*system_ui_page_swipe_cb_t)(system_ui_page_swipe_direction_t direction,
                                               void *user_ctx);

typedef struct {
    system_ui_launcher_select_cb_t on_launcher_select;
    system_ui_tasks_provider_cb_t get_tasks;
    system_ui_task_action_cb_t on_stop_task;
    system_ui_tasks_stop_all_cb_t on_stop_all_tasks;
    system_ui_app_exit_swipe_cb_t on_app_exit_swipe;
} system_ui_callbacks_t;

#ifdef __cplusplus
}
#endif
