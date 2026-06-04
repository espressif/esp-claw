/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#define ROVER_DEMO_STR_LEN       320
#define ROVER_DEMO_TIMEZONE_LEN  32

typedef struct {
    char wifi_ssid[ROVER_DEMO_STR_LEN];
    char wifi_password[ROVER_DEMO_STR_LEN];
    char llm_api_key[ROVER_DEMO_STR_LEN];
    char llm_backend_type[32];
    char llm_profile[32];
    char llm_model[64];
    char llm_base_url[ROVER_DEMO_STR_LEN];
    char llm_auth_type[32];
    char llm_timeout_ms[16];
    char tg_bot_token[ROVER_DEMO_STR_LEN];
    char time_timezone[ROVER_DEMO_TIMEZONE_LEN];
} rover_demo_settings_t;

extern const char *rover_demo_fatfs_base_path;

esp_err_t rover_demo_settings_init(void);
esp_err_t rover_demo_settings_load(rover_demo_settings_t *settings);
esp_err_t rover_demo_settings_save(const rover_demo_settings_t *settings);
esp_err_t rover_demo_settings_clear(void);
