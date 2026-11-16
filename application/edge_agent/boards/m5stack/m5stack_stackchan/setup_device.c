/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_io_expander_aw9523b.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_ft5x06.h"

static const char *TAG = "M5STACK_CORES3_SETUP_DEVICE";

esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle, const uint16_t dev_addr, esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = esp_io_expander_new_aw9523b(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create IO expander handle\n");
        return ret;
    }
    /* P0 push-pull mode (GCR.bit4=1). Required so P0_x can drive high
       (e.g. BUS_EN for Grove 5V output). */
    uint8_t data = 0x10;
    ret = esp_io_expander_aw9523b_write_reg(*handle_ret, AW9523B_REG_GCR, &data, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AW9523 P0 to push-pull mode");
        return ret;
    }
    /* AW9523B power-on/soft-reset default is LED (constant-current) mode for
       every pin. Switch P0/P1 fully into GPIO mode, otherwise high-level writes
       only release the current sink (relying on external pull-ups) and cannot
       drive enable signals like BUS_EN (P0_1) or BOOST_EN (P1_7). */
    data = 0xFF;
    ret = esp_io_expander_aw9523b_write_reg(*handle_ret, AW9523B_REG_LEDMODE0, &data, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AW9523 P0 to GPIO mode");
        return ret;
    }
    data = 0xFF;
    ret = esp_io_expander_aw9523b_write_reg(*handle_ret, AW9523B_REG_LEDMODE1, &data, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AW9523 P1 to GPIO mode");
    }
    return ret;
}

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));
    int ret = esp_lcd_new_panel_ili9341(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New ili9341 panel failed");
    }
    return ret;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = esp_lcd_touch_new_i2c_ft5x06(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ft5x06 touch driver: %s", esp_err_to_name(ret));
    }
    return ret;
}
