/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "product_mode_controller.h"

#include "config_runtime.h"
#include "esp_log.h"
#include "match_watch_runtime.h"

static const char *TAG = "product_mode";
static product_mode_controller_config_t s_config;

esp_err_t product_mode_controller_init(const product_mode_controller_config_t *config)
{
    if (config != NULL) {
        s_config = *config;
    } else {
        s_config = (product_mode_controller_config_t) {0};
    }
    return ESP_OK;
}

esp_err_t product_mode_controller_open_default(void)
{
    esp_err_t err;

    if (s_config.is_ready != NULL && !s_config.is_ready()) {
        ESP_LOGI(TAG, "default mode blocked: setup not complete");
        return ESP_ERR_INVALID_STATE;
    }

    err = config_runtime_clear_qr();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "clear config QR failed: %s", esp_err_to_name(err));
    }

    err = match_watch_runtime_open(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open Match Watch failed: %s", esp_err_to_name(err));
    }
    return err;
}
