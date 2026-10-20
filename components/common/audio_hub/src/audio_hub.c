/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Shared lifecycle for the mixer and capture hub. */

#include "audio_hub.h"

#include "esp_log.h"

static const char *TAG = "audio_hub";

#if CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
static audio_mixer_handle_t s_mixer;
static audio_capture_handle_t s_capture;
#endif

esp_err_t audio_hub_start(const audio_hub_config_t *config)
{
#if !CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    (void)config;
    ESP_LOGI(TAG, "audio codec is not supported by this board");
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_mixer && s_capture) return ESP_OK;
    if (s_mixer || s_capture) {
        ESP_LOGE(TAG, "service is partially started");
        return ESP_ERR_INVALID_STATE;
    }

    audio_mixer_config_t mixer_cfg = {0};
    audio_capture_config_t capture_cfg = {0};
    if (config) {
        mixer_cfg = config->mixer;
        capture_cfg = config->capture;
    }

    audio_mixer_handle_t mixer = NULL;
    esp_err_t err = audio_mixer_start(&mixer_cfg, &mixer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start mixer failed: %s", esp_err_to_name(err));
        return err;
    }

    audio_capture_handle_t capture = NULL;
    err = audio_capture_start(&capture_cfg, &capture);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start capture failed: %s", esp_err_to_name(err));
        esp_err_t stop_err = audio_mixer_stop(mixer);
        if (stop_err != ESP_OK) ESP_LOGW(TAG, "rollback mixer failed: %s", esp_err_to_name(stop_err));
        return err;
    }

    s_mixer = mixer;
    s_capture = capture;
    ESP_LOGI(TAG, "started");
    return ESP_OK;
#endif
}

bool audio_hub_is_started(void)
{
#if !CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    return false;
#else
    return s_mixer != NULL && s_capture != NULL;
#endif
}

esp_err_t audio_hub_get_mixer(audio_mixer_handle_t *out_mixer)
{
    if (out_mixer == NULL) return ESP_ERR_INVALID_ARG;
#if !CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    *out_mixer = NULL;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_mixer == NULL) {
        ESP_LOGE(TAG, "mixer is not started");
        return ESP_ERR_INVALID_STATE;
    }
    *out_mixer = s_mixer;
    return ESP_OK;
#endif
}

esp_err_t audio_hub_get_capture(audio_capture_handle_t *out_capture)
{
    if (out_capture == NULL) return ESP_ERR_INVALID_ARG;
#if !CONFIG_ESP_BOARD_DEV_AUDIO_CODEC_SUPPORT
    *out_capture = NULL;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_capture == NULL) {
        ESP_LOGE(TAG, "capture is not started");
        return ESP_ERR_INVALID_STATE;
    }
    *out_capture = s_capture;
    return ESP_OK;
#endif
}
