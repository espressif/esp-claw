/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_cst816s.h"

static const char *TAG = "lilygo_t_cameraplus_s3";

/* Create ST7789 panel from generated dev config */
esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    if (panel_dev_config == NULL || ret_panel == NULL) {
        ESP_LOGE(TAG, "lcd_panel_factory_entry_t: invalid argument");
        return ESP_ERR_INVALID_ARG;
    }

    esp_lcd_panel_dev_config_t cfg = {0};
    memcpy(&cfg, panel_dev_config, sizeof(cfg));

    esp_err_t ret = esp_lcd_new_panel_st7789(io, &cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "ST7789 panel created");
    return ESP_OK;
}

/* Create CST816S touch driver from generated dev config */
esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    if (io == NULL || touch_dev_config == NULL || ret_touch == NULL) {
        ESP_LOGE(TAG, "lcd_touch_factory_entry_t: invalid argument");
        return ESP_ERR_INVALID_ARG;
    }

    /* Copy the incoming touch configuration so we can adjust if needed */
    esp_lcd_touch_config_t cfg = {0};
    memcpy(&cfg, touch_dev_config, sizeof(cfg));

    /* If board expects interrupt-based touch and user didn't provide a callback,
       we keep default behavior (driver will poll if interrupt_callback is NULL). */
    esp_err_t ret = esp_lcd_touch_new_i2c_cst816s(io, &cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_cst816s failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "CST816S touch driver created");
    return ESP_OK;
}
