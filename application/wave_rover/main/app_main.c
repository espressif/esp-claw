/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "mdns.h"
#include "wave_rover_config.h"
#include "wave_rover_hal.h"
#include "wave_rover_mcp.h"
#include "wr_wifi.h"
#include "wr_syslog.h"

static const char *TAG = "wave_rover";
static wave_rover_config_t s_cfg;

void app_main(void)
{
    ESP_LOGI(TAG, "Wave Rover MCP firmware v0.1.0 starting");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wave_rover_config_load(&s_cfg));

    wr_syslog_init();  /* hook esp_log before any WiFi/HW init */

    /* The MCP SDK logs raw inbound/outbound JSON-RPC bodies at INFO level
     * ("Received message: %s" / "Sending response: %s" in esp_mcp_mgr.c) —
     * before any tool handler runs, so e.g. rover.set_wifi's plaintext
     * password would otherwise be captured here and forwarded over syslog
     * regardless of what individual tool handlers choose to log. Raise this
     * tag's threshold so those lines are neither emitted nor forwarded. */
    esp_log_level_set("esp_mcp_mgr", ESP_LOG_WARN);

    ESP_ERROR_CHECK(wr_hal_init());
    wr_motor_stop(); /* ensure motors off at boot */

    ESP_ERROR_CHECK(wr_wifi_init(&s_cfg));

    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_hostname_set(s_cfg.hostname);
        mdns_instance_name_set("Wave Rover MCP");
        mdns_service_add(NULL, "_http", "_tcp", s_cfg.mcp_port, NULL, 0);
        ESP_LOGI(TAG, "mDNS: %s.local -> :%u", s_cfg.hostname, s_cfg.mcp_port);
    } else {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
    }

    wr_syslog_start(s_cfg.syslog_enabled, s_cfg.syslog_host,
                    s_cfg.syslog_port, s_cfg.syslog_facility);
    ESP_ERROR_CHECK(wave_rover_mcp_start(&s_cfg));

    const char *ip = wr_wifi_get_ip();
    wr_display_status("0.1.0", ip[0] ? ip : "AP mode",
                      0.0f, true, false);

    ESP_LOGI(TAG, "boot complete. MCP at http://%s:%u/mcp",
             ip[0] ? ip : "<AP>", s_cfg.mcp_port);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGD(TAG, "heap free=%lu", (unsigned long)esp_get_free_heap_size());
    }
}
