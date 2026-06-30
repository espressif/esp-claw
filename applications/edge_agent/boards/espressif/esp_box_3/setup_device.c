/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 *
 * See LICENSE file for details.
 */

#include <string.h>
#include "esp_board_device.h"
#include "esp_codec_dev.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},

    {0xB1, (uint8_t []){00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},

    {0x11, (uint8_t []){0}, 0x80, 0},
    {0x29, (uint8_t []){0}, 0x80, 0},

    {0, (uint8_t []){0}, 0xff, 0},
};

// define st77916_vendor_config_t
static const ili9341_vendor_config_t vendor_config = {
    .init_cmds = vendor_specific_init,
    .init_cmds_size = sizeof(vendor_specific_init) / sizeof(vendor_specific_init[0]),
};

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    panel_dev_cfg.vendor_config = (void *)&vendor_config;
    int ret = esp_lcd_new_panel_ili9341(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE("lcd_panel_factory_entry_t", "New ili9341 panel failed");
        return ret;
    }
    return ESP_OK;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
{
    esp_lcd_touch_config_t touch_cfg = {0};
    memcpy(&touch_cfg, touch_dev_config, sizeof(esp_lcd_touch_config_t));
    if (touch_cfg.int_gpio_num != GPIO_NUM_NC) {
        ESP_LOGW("lcd_touch_factory_entry_t", "Touch interrupt supported!");
        touch_cfg.interrupt_callback = NULL;
    }
    esp_err_t ret = esp_lcd_touch_new_i2c_gt911(io, &touch_cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE("lcd_touch_factory_entry_t", "Failed to create gt911 touch driver: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
