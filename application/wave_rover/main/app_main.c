/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wave_rover_config.h"
#include "wr_wifi.h"

static const char *TAG = "wave_rover";
static wave_rover_config_t s_cfg;

void app_main(void)
{
    ESP_LOGI(TAG, "Wave Rover MCP firmware v0.1.0 starting");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wave_rover_config_load(&s_cfg));
    ESP_ERROR_CHECK(wr_wifi_init(&s_cfg));
    ESP_LOGI(TAG, "init complete. dry_run=%d", s_cfg.dry_run);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
