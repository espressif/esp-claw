/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * @file pet_runtime.h
 * @brief Default empty Pet Buddy scene runtime API.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the default empty pet scene.
 *
 * If another pet host is already active, this API is a no-op.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_runtime_start(void);

/**
 * @brief Stop the default empty pet scene.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_runtime_stop(void);

/**
 * @brief Check whether the default empty pet scene is running.
 *
 * @return true when running, false otherwise.
 */
bool pet_runtime_is_running(void);

#ifdef __cplusplus
}
#endif
