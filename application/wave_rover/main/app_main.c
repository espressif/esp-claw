/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wave_rover";

void app_main(void)
{
    ESP_LOGI(TAG, "Wave Rover MCP firmware v0.1.0 starting");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "NVS initialized");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
