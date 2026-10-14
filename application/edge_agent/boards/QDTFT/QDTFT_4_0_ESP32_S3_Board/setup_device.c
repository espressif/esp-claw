/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"

static const char *TAG = "QDTFT_SETUP_DEVICE";

/* ST7796 SPI LCD vendor specific initialization.
 * MADCTL(0x36)/COLMOD(0x3A) are handled by the driver via rgb_ele_order/mirror_x/bits_per_pixel,
 * SLPOUT(0x11)/DISPON(0x29) are handled by the driver internally. */
static const st7796_lcd_init_cmd_t vendor_specific_init_default[] = {
    {0xF0, (uint8_t []){0xC3}, 1, 0},
    {0xF0, (uint8_t []){0x96}, 1, 0},
    {0xB0, (uint8_t []){0x80}, 1, 0},
    {0xB6, (uint8_t []){0x00, 0x02}, 2, 0},
    {0xB5, (uint8_t []){0x02, 0x03, 0x00, 0x04}, 4, 0},
    {0xB1, (uint8_t []){0x80, 0x10}, 2, 0},
    {0xB4, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0xC6}, 1, 0},
    {0xC5, (uint8_t []){0x1C}, 1, 0},
    {0xE4, (uint8_t []){0x31}, 1, 0},
    {0xE8, (uint8_t []){0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33}, 8, 0},
    {0xC2, NULL, 0, 0},
    {0xA7, NULL, 0, 0},
    {0xE0, (uint8_t []){0xF0, 0x09, 0x13, 0x12, 0x12, 0x2B, 0x3C, 0x44, 0x4B, 0x1B, 0x18, 0x17, 0x1D, 0x21}, 14, 0},
    {0xE1, (uint8_t []){0xF0, 0x09, 0x13, 0x0C, 0x0D, 0x27, 0x3B, 0x44, 0x4D, 0x0B, 0x17, 0x17, 0x1D, 0x21}, 14, 0},
    {0x21, NULL, 0, 0},
    {0xF0, (uint8_t []){0x3C}, 1, 0},
    {0xF0, (uint8_t []){0x69}, 1, 0},
    {0x13, NULL, 0, 0},
};

static const st7796_vendor_config_t vendor_config = {
    .init_cmds      = vendor_specific_init_default,
    .init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(vendor_specific_init_default[0]),
    .flags          = {
        .use_mipi_interface = 0,  /* SPI interface */
    },
};

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    panel_dev_cfg.vendor_config = (void *)&vendor_config;
    esp_err_t ret = esp_lcd_new_panel_st7796(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New ST7796 panel failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = esp_lcd_touch_new_i2c_ft5x06(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ft5x06 (FT6336U compatible) touch driver: %s", esp_err_to_name(ret));
    }
    return ret;
}
