/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_runtime_state.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_runtime_state";

typedef struct {
    SemaphoreHandle_t config_lock;
    app_config_t config;
} app_runtime_state_t;

static app_runtime_state_t s_runtime;

bool app_runtime_state_business_ready_from_config(const app_config_t *config)
{
    if (config == NULL || config->wifi_ssid[0] == '\0') {
        ESP_LOGD(TAG, "business not ready: Wi-Fi SSID missing");
        return false;
    }
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    if (config->wechat_token[0] == '\0') {
        ESP_LOGI(TAG, "business not ready: WeChat token missing");
        return false;
    }
#endif
    return true;
}

esp_err_t app_runtime_state_init(void)
{
    memset(&s_runtime, 0, sizeof(s_runtime));
    s_runtime.config_lock = xSemaphoreCreateMutex();
    return s_runtime.config_lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

app_config_t *app_runtime_state_config(void)
{
    return &s_runtime.config;
}

esp_err_t app_runtime_state_load_config(void)
{
    return app_config_load(&s_runtime.config);
}

esp_err_t app_runtime_state_update_config(const app_config_t *config)
{
    if (config == NULL || s_runtime.config_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_runtime.config_lock, portMAX_DELAY);
    s_runtime.config = *config;
    xSemaphoreGive(s_runtime.config_lock);
    return ESP_OK;
}

esp_err_t app_runtime_state_save_config(const app_config_t *config)
{
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = app_config_validate_wifi(config, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = app_config_save(config);
    if (err != ESP_OK) {
        return err;
    }
    return app_runtime_state_update_config(config);
}

esp_err_t app_runtime_state_wifi_fields_snapshot(app_runtime_wifi_fields_t *out_fields)
{
    if (out_fields == NULL || s_runtime.config_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_fields, 0, sizeof(*out_fields));
    xSemaphoreTake(s_runtime.config_lock, portMAX_DELAY);
    strlcpy(out_fields->sta_ssid, s_runtime.config.wifi_ssid, sizeof(out_fields->sta_ssid));
    strlcpy(out_fields->sta_password, s_runtime.config.wifi_password, sizeof(out_fields->sta_password));
    strlcpy(out_fields->ap_ssid, s_runtime.config.ap_ssid, sizeof(out_fields->ap_ssid));
    strlcpy(out_fields->ap_password, s_runtime.config.ap_password, sizeof(out_fields->ap_password));
    strlcpy(out_fields->ap_behavior, s_runtime.config.ap_behavior, sizeof(out_fields->ap_behavior));
    xSemaphoreGive(s_runtime.config_lock);
    return ESP_OK;
}

bool app_runtime_state_business_ready(void)
{
    bool ready;

    if (s_runtime.config_lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_runtime.config_lock, portMAX_DELAY);
    ready = app_runtime_state_business_ready_from_config(&s_runtime.config);
    xSemaphoreGive(s_runtime.config_lock);
    return ready;
}
