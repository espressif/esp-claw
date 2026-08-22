/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "system_ui_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t system_ui_start(const system_ui_config_t *config);
void system_ui_stop(void);
bool system_ui_is_started(void);

esp_err_t system_ui_show_home(void);
esp_err_t system_ui_reload_home(void);
esp_err_t system_ui_set_callbacks(const system_ui_callbacks_t *callbacks, void *user_ctx);
esp_err_t system_ui_update_network(const system_ui_network_state_t *state);
esp_err_t system_ui_set_activity(bool active);
esp_err_t system_ui_show_task_panel(bool visible);
esp_err_t system_ui_refresh_tasks(void);
void system_ui_click_feedback(void);

#ifdef __cplusplus
}
#endif
