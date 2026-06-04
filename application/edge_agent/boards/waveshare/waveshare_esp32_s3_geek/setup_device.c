/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Waveshare ESP32-S3-GEEK — board-specific device factories.
 * Provides the ST7789 panel factory used by the generic display_lcd device.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

static const char *TAG = "WS_S3_GEEK_SETUP";

/* Visible portion of the 1.14" 135x240 ST7789 panel starts at (52, 40). */
#define GEEK_LCD_OFFSET_X  52
#define GEEK_LCD_OFFSET_Y  40

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t cfg = {0};
    memcpy(&cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    esp_err_t ret = esp_lcd_new_panel_st7789(io, &cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(*ret_panel, GEEK_LCD_OFFSET_X, GEEK_LCD_OFFSET_Y);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "esp_lcd_panel_set_gap failed: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}
