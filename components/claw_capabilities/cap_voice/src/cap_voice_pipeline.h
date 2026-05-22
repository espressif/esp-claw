/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"

typedef enum {
    CAP_VOICE_STATE_IDLE,
    CAP_VOICE_STATE_LISTENING,
    CAP_VOICE_STATE_RECOGNIZING,
    CAP_VOICE_STATE_SPEAKING,
} cap_voice_state_t;

typedef void (*cap_voice_wakeup_cb_t)(void *ctx);
typedef void (*cap_voice_command_cb_t)(const char *command_id,
                                        const char *text, void *ctx);

typedef struct {
    cap_voice_wakeup_cb_t  on_wakeup;
    cap_voice_command_cb_t on_command;
    cap_voice_command_cb_t on_transcript;
    void *cb_ctx;
    float wake_sensitivity;
    float multinet_threshold;
    const char *whisper_api_key;
    const char *whisper_base_url;
    const char *tts_api_key;
    const char *tts_base_url;
} cap_voice_pipeline_config_t;

esp_err_t cap_voice_pipeline_init(const cap_voice_pipeline_config_t *cfg);
esp_err_t cap_voice_pipeline_start(void);
esp_err_t cap_voice_pipeline_stop(void);
cap_voice_state_t cap_voice_pipeline_get_state(void);
