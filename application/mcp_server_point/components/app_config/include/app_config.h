/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "app_claw.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_STR_LEN        320
#define APP_WIFI_SSID             CONFIG_APP_WIFI_SSID
#define APP_WIFI_PASSWORD         CONFIG_APP_WIFI_PASSWORD

typedef struct {
    char wifi_ssid[APP_CONFIG_STR_LEN];
    char wifi_password[APP_CONFIG_STR_LEN];
} app_config_t;

esp_err_t app_config_init(void);
void app_config_load_defaults(app_config_t *config);
esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);
void app_config_to_claw(const app_config_t *config, app_claw_config_t *out);

#ifdef __cplusplus
}
#endif
