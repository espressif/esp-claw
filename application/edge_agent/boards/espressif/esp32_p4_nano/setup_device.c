/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "driver/gpio.h"

static const char *TAG = "P4_NANO_SETUP_DEVICE";

#define BSP_SD_EN_PIN           (GPIO_NUM_46)

/*
 * Board early init: SD card power control
 * Called via __attribute__((constructor)) so it runs before app_main / board_manager.
 */
static void __attribute__((constructor)) p4_nano_board_early_init(void)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "P4-NANO board early init");

    /* SD card enable pin (active low) */
    const gpio_config_t sdcard_io_config = {
        .pin_bit_mask = BIT64(BSP_SD_EN_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ret = gpio_config(&sdcard_io_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card power GPIO%u config failed: %s — skipping",
                 (unsigned)BSP_SD_EN_PIN, esp_err_to_name(ret));
        return;
    }

    /* Enable SD card (active low, so set to 0) */
    ret = gpio_set_level(BSP_SD_EN_PIN, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD card power GPIO%u set failed: %s",
                 (unsigned)BSP_SD_EN_PIN, esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "SD card power enabled (GPIO%u)", (unsigned)BSP_SD_EN_PIN);
}
