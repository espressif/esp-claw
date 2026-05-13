/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;
    const char *backend_type;
    const char *api_key;
    const char *base_url;
    const char *model;
    const char *language;
    bool keep_audio_in_storage;
} audio_stt_config_t;

esp_err_t audio_stt_set_config(const audio_stt_config_t *config);
bool audio_stt_is_enabled(void);
bool audio_stt_keep_audio_in_storage(void);

esp_err_t audio_stt_transcribe_file(const char *file_path,
                                    const char *mime_type,
                                    char *out_text,
                                    size_t out_text_size);

esp_err_t audio_stt_transcribe_buffer(const void *audio_data,
                                      size_t audio_len,
                                      const char *filename,
                                      const char *mime_type,
                                      char *out_text,
                                      size_t out_text_size);

#ifdef __cplusplus
}
#endif
