/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_playback_t *audio_playback_handle_t;

typedef enum {
    AUDIO_PLAYER_IDLE = 0,
    AUDIO_PLAYER_PLAYING,
    AUDIO_PLAYER_PAUSED,
    AUDIO_PLAYER_STOPPED,
    AUDIO_PLAYER_FINISHED,
    AUDIO_PLAYER_ERROR,
} audio_playback_state_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
} audio_playback_format_t;

typedef struct {
    audio_playback_state_t state;
    audio_playback_format_t source_format;
    uint32_t bitrate;
    int64_t position_ms;
    int64_t duration_ms;
    esp_err_t last_error;
} audio_playback_status_t;

typedef esp_err_t (*audio_playback_write_cb_t)(
    const void *data, size_t bytes, void *user_ctx);
typedef void (*audio_playback_event_cb_t)(
    const audio_playback_status_t *status, void *user_ctx);

typedef struct {
    audio_playback_format_t output_format;
    audio_playback_write_cb_t write;
    void *write_ctx;
    audio_playback_event_cb_t event;
    void *event_ctx;
} audio_playback_config_t;

esp_err_t audio_playback_create(
    const audio_playback_config_t *config,
    audio_playback_handle_t *ret_player);
void audio_playback_delete(audio_playback_handle_t player);
esp_err_t audio_playback_play(audio_playback_handle_t player,
                                    const char *uri, uint64_t total_bytes,
                                    bool wait_until_finished);
esp_err_t audio_playback_stop(audio_playback_handle_t player);
esp_err_t audio_playback_pause(audio_playback_handle_t player);
esp_err_t audio_playback_resume(audio_playback_handle_t player);
esp_err_t audio_playback_get_status(
    audio_playback_handle_t player,
    audio_playback_status_t *out_status);
const char *audio_playback_state_name(audio_playback_state_t state);

#ifdef __cplusplus
}
#endif
