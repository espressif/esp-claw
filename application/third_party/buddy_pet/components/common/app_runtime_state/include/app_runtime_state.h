/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char sta_ssid[APP_CONFIG_STR_LEN];
    char sta_password[APP_CONFIG_STR_LEN];
    char ap_ssid[APP_CONFIG_STR_LEN];
    char ap_password[APP_CONFIG_STR_LEN];
    char ap_behavior[16];
} app_runtime_wifi_fields_t;

esp_err_t app_runtime_state_init(void);
app_config_t *app_runtime_state_config(void);
esp_err_t app_runtime_state_load_config(void);
esp_err_t app_runtime_state_update_config(const app_config_t *config);
esp_err_t app_runtime_state_save_config(const app_config_t *config);
esp_err_t app_runtime_state_wifi_fields_snapshot(app_runtime_wifi_fields_t *out_fields);
bool app_runtime_state_business_ready(void);
bool app_runtime_state_business_ready_from_config(const app_config_t *config);

#ifdef __cplusplus
}
#endif
