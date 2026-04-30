/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

/* STT: Upload WAV audio buffer to Minimax API, return transcribed text */
esp_err_t cap_voice_stt_transcribe(const uint8_t *wav_data, size_t wav_len,
                                   const char *api_key, const char *base_url,
                                   char **out_text);

/* TTS: Send text to Minimax TTS API, return raw PCM audio (16kHz 16-bit mono) */
esp_err_t cap_voice_tts_synthesize(const char *text,
                                   const char *api_key, const char *base_url,
                                   uint8_t **out_pcm, size_t *out_pcm_len);
