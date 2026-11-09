/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_cst816s.h"

static const char *TAG = "NULLPOD_S3";

/* The ST7789 controller drives 240x320 of frame memory while the NullPod panel
 * only exposes 240x280, so the visible area starts 20 rows below the origin. */
#define NULLPOD_LCD_Y_GAP  20

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(io != NULL && panel_dev_config != NULL && ret_panel != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid lcd factory args");

    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    esp_err_t ret = esp_lcd_new_panel_st7789(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_panel_set_gap(*ret_panel, 0, NULLPOD_LCD_Y_GAP);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to set ST7789 panel gap: %s", esp_err_to_name(ret));
    }

    return ESP_OK;
}

/*
 * Touch: the CST816S driver is polled. lua_module_lcd_touch only switches to
 * its interrupt-driven path when interrupt_callback is non-NULL, and the board
 * manager always leaves it NULL, so leaving it untouched keeps the touch panel
 * in polling mode (esp_lcd_touch_read_data on each Lua read).
 */
esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t *touch_dev_config,
                                    esp_lcd_touch_handle_t *ret_touch)
{
    ESP_RETURN_ON_FALSE(io != NULL && touch_dev_config != NULL && ret_touch != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid lcd touch factory args");

    esp_lcd_touch_config_t touch_cfg = {0};
    memcpy(&touch_cfg, touch_dev_config, sizeof(esp_lcd_touch_config_t));

    esp_err_t ret = esp_lcd_touch_new_i2c_cst816s(io, &touch_cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create CST816S touch driver: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}
