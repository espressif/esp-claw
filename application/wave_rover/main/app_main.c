/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "wave_rover_config.h"
#include "wave_rover_hal.h"
#include "wave_rover_mcp.h"
#include "wr_wifi.h"

static const char *TAG = "wave_rover";
static wave_rover_config_t s_cfg;

void app_main(void)
{
    ESP_LOGI(TAG, "Wave Rover MCP firmware v0.1.0 starting");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wave_rover_config_load(&s_cfg));

    ESP_ERROR_CHECK(wr_hal_init(s_cfg.dry_run));
    wr_motor_stop(); /* ensure motors off at boot */

    ESP_ERROR_CHECK(wr_wifi_init(&s_cfg));
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
