/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */


#pragma once

#include <stdbool.h>

#include "audio_capture.h"
#include "audio_mixer.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration passed to audio_hub_start().
 */
typedef struct {
    audio_mixer_config_t mixer;
    audio_capture_config_t capture;
} audio_hub_config_t;

/**
 * @brief Starts the shared mixer and capture hub.
 *
 * @param config Optional service configuration; NULL uses component defaults.
 * @return ESP_OK on success, or an error from the failed component.
 */
esp_err_t audio_hub_start(const audio_hub_config_t *config);

/**
 * @brief Returns whether both audio hubs are running.
 */
bool audio_hub_is_started(void);

/**
 * @brief Gets the shared mixer handle.
 *
 * @param out_mixer Output handle.
 * @return ESP_OK on success, or ESP_ERR_INVALID_STATE when not started.
 */
esp_err_t audio_hub_get_mixer(audio_mixer_handle_t *out_mixer);

/**
 * @brief Gets the shared capture handle.
 *
 * @param out_capture Output handle.
 * @return ESP_OK on success, or ESP_ERR_INVALID_STATE when not started.
 */
esp_err_t audio_hub_get_capture(audio_capture_handle_t *out_capture);

#ifdef __cplusplus
}
#endif
