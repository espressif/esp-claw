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
    cfg->mcp_port                = 80;
    cfg->auth_enabled            = false;
    cfg->safe_mode               = false;
    cfg->max_speed               = 0.4f;
    cfg->max_command_duration_ms = 3000;
    cfg->syslog_enabled          = true;
    cfg->syslog_host[0]          = '\0';   /* broadcast */
    cfg->syslog_port             = 5514;
    cfg->syslog_facility         = 16;     /* local0 */

    cfg->power_mgr_enabled             = true;
    cfg->power_active_timeout_sec      = 60;
    cfg->power_idle_to_low_power_sec   = 300;
    cfg->power_wifi_power_save         = true;
    cfg->power_reduce_cpu_frequency    = true;
    cfg->power_disable_display_idle    = true;
    cfg->power_critical_battery_v      = 9.6f;
    cfg->power_telemetry_active_sec    = 5;
    cfg->power_telemetry_idle_sec      = 30;
    cfg->power_telemetry_low_power_sec = 120;
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

    /* Sanity-check the loaded combination — STA mode with an empty SSID
     * makes esp_wifi_connect() return ESP_ERR_WIFI_SSID, which wr_wifi_init
     * cannot recover from at boot; that previously caused an abort()/reboot
     * crash-loop with the bad config persisting in NVS (unreachable brick).
     * Fall back to AP mode so the device stays reachable for repair. */
    if (cfg->wifi_mode == 1 && cfg->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "saved config: STA mode with empty SSID — falling back to AP");
        cfg->wifi_mode = 0;
    }

    /* Never log password fields */
    ESP_LOGI(TAG, "config loaded: wifi_mode=%u mcp_port=%u",
             cfg->wifi_mode, cfg->mcp_port);
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
