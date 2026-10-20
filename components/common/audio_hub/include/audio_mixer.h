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
    AUDIO_MIXER_TRACK_SYSTEM = 0,
    AUDIO_MIXER_TRACK_APP,
} audio_mixer_track_role_t;

typedef struct audio_mixer_t     *audio_mixer_handle_t;
typedef struct audio_mixer_track *audio_mixer_track_handle_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits;
    uint32_t frame_ms;
    float    system_full_gain;
    float    app_full_gain;
    float    app_ducked_gain;
    uint32_t duck_release_ms;
    int      output_volume;     /* codec DAC volume [0, 100] */
} audio_mixer_config_t;

esp_err_t audio_mixer_start(const audio_mixer_config_t *config, audio_mixer_handle_t *out_mixer);
esp_err_t audio_mixer_stop (audio_mixer_handle_t mixer);

esp_err_t audio_mixer_set_output_volume(audio_mixer_handle_t mixer, int percent);
esp_err_t audio_mixer_get_output_volume(audio_mixer_handle_t mixer, int *out_percent);

esp_err_t audio_mixer_open_track(audio_mixer_handle_t mixer, audio_mixer_track_role_t role, const char *owner_tag, audio_mixer_track_handle_t *out_track);
esp_err_t audio_mixer_close_track(audio_mixer_track_handle_t track);
size_t audio_mixer_track_write(audio_mixer_track_handle_t track, const void *pcm, size_t bytes);

esp_err_t audio_mixer_track_flush(audio_mixer_track_handle_t track);
esp_err_t audio_mixer_track_stop (audio_mixer_track_handle_t track);
esp_err_t audio_mixer_track_info(audio_mixer_track_handle_t track, uint32_t *sample_rate, uint8_t *channels, uint8_t *bits);

#ifdef __cplusplus
}
#endif
