/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#define ROVER_S3_STR_LEN  320
#define ROVER_S3_TZ_LEN   32

typedef struct {
    char wifi_ssid[ROVER_S3_STR_LEN];
    char wifi_password[ROVER_S3_STR_LEN];
    char llm_api_key[ROVER_S3_STR_LEN];
    char llm_backend_type[32];
    char llm_profile[32];
    char llm_model[64];
    char llm_base_url[ROVER_S3_STR_LEN];
    char llm_auth_type[32];
    char llm_timeout_ms[16];
    char tg_bot_token[ROVER_S3_STR_LEN];
    char time_timezone[ROVER_S3_TZ_LEN];
    char whisper_api_key[ROVER_S3_STR_LEN];  /* empty = use llm_api_key */
    char whisper_base_url[ROVER_S3_STR_LEN]; /* empty = use llm_base_url */
    char tts_api_key[ROVER_S3_STR_LEN];      /* empty = use llm_api_key */
    char tts_base_url[ROVER_S3_STR_LEN];     /* empty = use llm_base_url */
    char tts_voice[32];                      /* default alloy */
    char wake_sensitivity[8];                /* float string, default 0.7 */
    char multinet_threshold[8];              /* float string, default 0.85 */
    char voice_enabled[8];                   /* 1 or 0, default 1 */
} rover_s3_settings_t;

extern const char *rover_s3_fatfs_base_path;

esp_err_t rover_s3_settings_init(void);
esp_err_t rover_s3_settings_load(rover_s3_settings_t *settings);
esp_err_t rover_s3_settings_save(const rover_s3_settings_t *settings);
esp_err_t rover_s3_settings_clear(void);
