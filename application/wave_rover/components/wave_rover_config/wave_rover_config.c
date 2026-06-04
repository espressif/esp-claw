/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_config.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "wr_config";
#define NVS_NS "wr_cfg"

void wave_rover_config_defaults(wave_rover_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->wifi_ap_ssid,     "WR-ESP32",    sizeof(cfg->wifi_ap_ssid));
    strlcpy(cfg->wifi_ap_password, "12345678",    sizeof(cfg->wifi_ap_password));
    strlcpy(cfg->hostname,         "wave-rover",  sizeof(cfg->hostname));
    cfg->wifi_mode               = 0;     /* AP */
    cfg->mcp_port                = 8080;
    cfg->auth_enabled            = false;
    cfg->safe_mode               = false;
    cfg->dry_run                 = true;  /* safe default until HW confirmed */
    cfg->max_speed               = 0.4f;
    cfg->max_command_duration_ms = 3000;
}

esp_err_t wave_rover_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t wave_rover_config_load(wave_rover_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    wave_rover_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved config, using defaults");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");

    size_t sz = sizeof(*cfg);
    err = nvs_get_blob(h, "cfg", cfg, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        wave_rover_config_defaults(cfg);
        err = ESP_OK;
    }
    nvs_close(h);
    /* Never log password fields */
    ESP_LOGI(TAG, "config loaded: wifi_mode=%u mcp_port=%u dry_run=%d",
             cfg->wifi_mode, cfg->mcp_port, cfg->dry_run);
    return err;
}

esp_err_t wave_rover_config_save(const wave_rover_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_blob(h, "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
