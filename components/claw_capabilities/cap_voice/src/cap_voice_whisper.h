/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>

typedef struct {
    const char *api_key;
    const char *base_url;
    const char *language;
} cap_voice_whisper_config_t;

esp_err_t cap_voice_whisper_transcribe(const cap_voice_whisper_config_t *cfg,
                                        const int16_t *pcm, size_t pcm_samples,
                                        char **out_text);
