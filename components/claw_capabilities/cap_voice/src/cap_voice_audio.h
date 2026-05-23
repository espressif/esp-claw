/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/* I2S/ES8311 GPIO assignments for M5Stack StickS3 (from M5Unified board table) */
#define CAP_VOICE_I2S_PORT       I2S_NUM_0
#define CAP_VOICE_I2S_MCLK       18
#define CAP_VOICE_I2S_BCLK       17
#define CAP_VOICE_I2S_WS         15
#define CAP_VOICE_I2S_DIN        16   /* data in TO ESP32 (from mic) */
#define CAP_VOICE_I2S_DOUT       14   /* data out FROM ESP32 (to speaker) */
/* Internal I2C bus for ES8311 codec (SCL=48, SDA=47 per M5Unified StickS3 table) */
#define CAP_VOICE_I2C_PORT       I2C_NUM_0
#define CAP_VOICE_I2C_SDA        47
#define CAP_VOICE_I2C_SCL        48
#define CAP_VOICE_ES8311_ADDR    0x30  /* 8-bit write addr; esp_codec_dev does addr>>1 internally */

#define CAP_VOICE_SAMPLE_RATE_REC   16000
#define CAP_VOICE_SAMPLE_RATE_PLAY  24000
#define CAP_VOICE_BITS              16
#define CAP_VOICE_CHANNELS          1

esp_err_t cap_voice_audio_init(void);
esp_err_t cap_voice_audio_deinit(void);

esp_err_t cap_voice_audio_read(int16_t *buf, size_t samples, size_t *out_read);

esp_err_t cap_voice_audio_play(const int16_t *buf, size_t samples);

esp_err_t cap_voice_audio_play_tone(int freq_hz, int duration_ms);
