/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file match_watch_app.h
 * @brief Internal Match Watch app loop and event requests.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "match_watch_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start Match Watch UI main loop.
 *
 * Opens platform assets, runs the UI loop until close is requested, then tears down.
 *
 * @return ESP_OK on normal exit, otherwise an error code.
 */
esp_err_t match_watch_app_main(void);

/**
 * @brief Request graceful close of the running Match Watch UI.
 *
 * @return ESP_OK if posted or app is not running, otherwise an error code.
 */
esp_err_t match_watch_app_request_close(void);

/**
 * @brief Request UI refresh after live schedule data changed.
 *
 * @return ESP_OK if posted or app is not running, otherwise an error code.
 */
esp_err_t match_watch_app_request_data_changed(void);

/**
 * @brief Request pet asset reload on the running Match Watch UI.
 *
 * @return ESP_OK if posted or app is not running, otherwise an error code.
 */
esp_err_t match_watch_app_request_pet_reload(void);

/**
 * @brief Set schedule data source for the running app.
 *
 * @param source LIVE for provider-pushed data, EXTERNAL for embedded/static data.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t match_watch_app_set_data_source(match_watch_data_source_t source);

/**
 * @brief Check whether Match Watch UI main loop is running.
 *
 * @return true if the app main loop is active.
 */
bool match_watch_app_is_running(void);

#ifdef __cplusplus
}
#endif
