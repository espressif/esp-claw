/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_board_manager_includes.h"

static const char *TAG = "setup_device";

/*
 * Minimal setup_device.c for I2S audio breadboard configuration.
 * Audio DAC (MAX98357A) and ADC (INMP441) are handled automatically
 * by the board manager via chip: internal audio_codec devices.
 * No custom device initialization needed.
 */

void app_setup_device_info(void)
{
    ESP_LOGI(TAG, "I2S breadboard config: MAX98357A (out) + INMP441 (in)");
}
