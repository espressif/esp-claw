/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_CAPTURE_SUB_SYSTEM = 0,
    AUDIO_CAPTURE_SUB_APP,
} audio_capture_subscriber_role_t;

typedef struct audio_capture_t          *audio_capture_handle_t;
typedef struct audio_capture_subscriber *audio_capture_sub_handle_t;

#define AUDIO_CAPTURE_INPUT_GAIN_DB_MAX 42.0f

typedef struct {
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits;
    uint32_t frame_ms;
    uint32_t ring_frames;   /* per-subscriber ring depth */
} audio_capture_config_t;

typedef struct {
    /* Fields set to 0 inherit the hub's internal format. */
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits;
} audio_capture_sub_format_t;

esp_err_t audio_capture_start(const audio_capture_config_t *config, audio_capture_handle_t *out_capture);
esp_err_t audio_capture_stop (audio_capture_handle_t capture);

/* Codec ADC input gain in dB. Values must be in [0, AUDIO_CAPTURE_INPUT_GAIN_DB_MAX]. */
esp_err_t audio_capture_set_input_gain(audio_capture_handle_t capture, float gain_db);
esp_err_t audio_capture_get_input_gain(audio_capture_handle_t capture, float *out_gain_db);

/* One subscriber per role; a second open returns ESP_ERR_INVALID_STATE. */
esp_err_t audio_capture_open_subscriber(audio_capture_handle_t capture, audio_capture_subscriber_role_t role, const audio_capture_sub_format_t *fmt, const char *owner_tag, audio_capture_sub_handle_t *out_sub);

esp_err_t audio_capture_close_subscriber(audio_capture_sub_handle_t sub);

/**
 * @brief Discards all queued PCM for a subscriber.
 *
 * @param sub Subscriber handle.
 * @return ESP_OK on success, or ESP_ERR_INVALID_STATE when closed.
 */
esp_err_t audio_capture_sub_flush(audio_capture_sub_handle_t sub);

/* Read up to `bytes` from the subscriber in its target format.
 * timeout_ms == 0 is non-blocking; returns actual bytes read. */
size_t audio_capture_sub_read(audio_capture_sub_handle_t sub, void *pcm, size_t bytes, uint32_t timeout_ms);

esp_err_t audio_capture_sub_info(audio_capture_sub_handle_t sub, uint32_t *sample_rate, uint8_t *channels, uint8_t *bits);

/* Bytes discarded by this subscriber since it was opened. */
esp_err_t audio_capture_sub_get_dropped_bytes(audio_capture_sub_handle_t sub, uint64_t *dropped_bytes);

#ifdef __cplusplus
}
#endif
