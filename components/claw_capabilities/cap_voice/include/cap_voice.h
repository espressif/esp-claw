/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"

typedef enum {
    CAP_VOICE_UI_IDLE,
    CAP_VOICE_UI_LISTENING,
    CAP_VOICE_UI_THINKING,
    CAP_VOICE_UI_SPEAKING,
} cap_voice_ui_state_t;

typedef void (*cap_voice_ui_cb_t)(cap_voice_ui_state_t state, void *ctx);

typedef struct {
    const char *whisper_api_key;  /* NULL -> use LLM key from claw_core config */
    const char *whisper_base_url; /* NULL -> use llm_base_url */
    const char *tts_api_key;
    const char *tts_base_url;
    const char *tts_voice;        /* "alloy", "nova", etc. */
    float wake_sensitivity;       /* 0.0-1.0, default 0.7 */
    float multinet_threshold;     /* 0.0-1.0, default 0.85 */
    cap_voice_ui_cb_t on_ui_state; /* optional: called on voice state change */
    void *ui_ctx;
} cap_voice_config_t;

esp_err_t cap_voice_register_group(void);
esp_err_t cap_voice_set_config(const cap_voice_config_t *config);
esp_err_t cap_voice_start(void);
esp_err_t cap_voice_stop(void);

/* Called by TTS response path to speak text immediately */
esp_err_t cap_voice_speak(const char *text);
