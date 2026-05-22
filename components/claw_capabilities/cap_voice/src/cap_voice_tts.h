/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *voice;  /* "alloy", "nova", "shimmer", "echo", "fable", "onyx" */
} cap_voice_tts_config_t;

/* Synthesize text to speech and play it through ES8311.
   Blocks until playback is complete. */
esp_err_t cap_voice_tts_speak(const cap_voice_tts_config_t *cfg,
                               const char *text);
