/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_board_manager_includes.h"
#include "gen_board_device_custom.h"
#include "periph_rmt.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"
#include "esp_lcd_ili9341.h"

static const char *TAG = "ADAFRUIT_METRO_ESP32S3_SETUP_DEVICE";

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));
    esp_err_t ret = esp_lcd_new_panel_ili9341(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New ili9341 panel failed");
    }
    return ret;
}
