/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the voice assistant capability
 *
 * Sets up GPIO button, audio devices, and starts the voice task.
 * Requires board manager to have initialized I2S audio codec devices.
 *
 * @return ESP_OK on success
 */
esp_err_t cap_voice_init(void);

/**
 * @brief Start the voice assistant (enables button listening)
 */
esp_err_t cap_voice_start(void);

/**
 * @brief Stop the voice assistant
 */
esp_err_t cap_voice_stop(void);

#ifdef __cplusplus
}
#endif
