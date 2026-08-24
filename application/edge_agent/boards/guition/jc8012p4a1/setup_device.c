/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "esp_log.h"
#include "dev_display_lcd.h"
#include "esp_lcd_jd9365.h"
// #include "esp_lcd_touch_gt911.h"

static const char *TAG = "P4_FUNCTION_EV_SETUP_DEVICE";

static const jd9365_lcd_init_cmd_t lcd_cmd[] = {
//  {cmd, { data }, data_size, delay_ms}
    {0xE0, (uint8_t[]){0x00}, 1, 0},
    {0xE1, (uint8_t[]){0x93}, 1, 0},
    {0xE2, (uint8_t[]){0x65}, 1, 0},
    {0xE3, (uint8_t[]){0xF8}, 1, 0},
    {0x80, (uint8_t[]){0x01}, 1, 0},

    {0xE0, (uint8_t[]){0x01}, 1, 0},
    {0x00, (uint8_t[]){0x00}, 1, 0},
    {0x01, (uint8_t[]){0x39}, 1, 0},
    {0x03, (uint8_t[]){0x10}, 1, 0},
    {0x04, (uint8_t[]){0x41}, 1, 0},

    {0x0C, (uint8_t[]){0x74}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0},
    {0x18, (uint8_t[]){0xD7}, 1, 0},
    {0x19, (uint8_t[]){0x00}, 1, 0},
    {0x1A, (uint8_t[]){0x00}, 1, 0},

    {0x1B, (uint8_t[]){0xD7}, 1, 0},
    {0x1C, (uint8_t[]){0x00}, 1, 0},
    {0x24, (uint8_t[]){0xFE}, 1, 0},
    {0x35, (uint8_t[]){0x26}, 1, 0},
    {0x37, (uint8_t[]){0x69}, 1, 0},

    {0x38, (uint8_t[]){0x05}, 1, 0},
    {0x39, (uint8_t[]){0x06}, 1, 0},
    {0x3A, (uint8_t[]){0x08}, 1, 0},
    {0x3C, (uint8_t[]){0x78}, 1, 0},
    {0x3D, (uint8_t[]){0xFF}, 1, 0},

    {0x3E, (uint8_t[]){0xFF}, 1, 0},
    {0x3F, (uint8_t[]){0xFF}, 1, 0},
    {0x40, (uint8_t[]){0x06}, 1, 0},
    {0x41, (uint8_t[]){0xA0}, 1, 0},
    {0x43, (uint8_t[]){0x14}, 1, 0},

    {0x44, (uint8_t[]){0x0B}, 1, 0},
    {0x45, (uint8_t[]){0x30}, 1, 0},
    //{0x4A, (uint8_t[]){0x35}, 1, 0},//bist
    {0x4B, (uint8_t[]){0x04}, 1, 0},
    {0x55, (uint8_t[]){0x02}, 1, 0},
    {0x57, (uint8_t[]){0x89}, 1, 0},

    {0x59, (uint8_t[]){0x0A}, 1, 0},
    {0x5A, (uint8_t[]){0x28}, 1, 0},

    {0x5B, (uint8_t[]){0x15}, 1, 0},
    {0x5D, (uint8_t[]){0x50}, 1, 0},
    {0x5E, (uint8_t[]){0x37}, 1, 0},
    {0x5F, (uint8_t[]){0x29}, 1, 0},
    {0x60, (uint8_t[]){0x1E}, 1, 0},

    {0x61, (uint8_t[]){0x1D}, 1, 0},
    {0x62, (uint8_t[]){0x12}, 1, 0},
    {0x63, (uint8_t[]){0x1A}, 1, 0},
    {0x64, (uint8_t[]){0x08}, 1, 0},
    {0x65, (uint8_t[]){0x25}, 1, 0},

    {0x66, (uint8_t[]){0x26}, 1, 0},
    {0x67, (uint8_t[]){0x28}, 1, 0},
    {0x68, (uint8_t[]){0x49}, 1, 0},
    {0x69, (uint8_t[]){0x3A}, 1, 0},
    {0x6A, (uint8_t[]){0x43}, 1, 0},

    {0x6B, (uint8_t[]){0x3A}, 1, 0},
    {0x6C, (uint8_t[]){0x3B}, 1, 0},
    {0x6D, (uint8_t[]){0x32}, 1, 0},
    {0x6E, (uint8_t[]){0x1F}, 1, 0},
    {0x6F, (uint8_t[]){0x0E}, 1, 0},

    {0x70, (uint8_t[]){0x50}, 1, 0},
    {0x71, (uint8_t[]){0x37}, 1, 0},
    {0x72, (uint8_t[]){0x29}, 1, 0},
    {0x73, (uint8_t[]){0x1E}, 1, 0},
    {0x74, (uint8_t[]){0x1D}, 1, 0},

    {0x75, (uint8_t[]){0x12}, 1, 0},
    {0x76, (uint8_t[]){0x1A}, 1, 0},
    {0x77, (uint8_t[]){0x08}, 1, 0},
    {0x78, (uint8_t[]){0x25}, 1, 0},
    {0x79, (uint8_t[]){0x26}, 1, 0},

    {0x7A, (uint8_t[]){0x28}, 1, 0},
    {0x7B, (uint8_t[]){0x49}, 1, 0},
    {0x7C, (uint8_t[]){0x3A}, 1, 0},
    {0x7D, (uint8_t[]){0x43}, 1, 0},
    {0x7E, (uint8_t[]){0x3A}, 1, 0},

    {0x7F, (uint8_t[]){0x3B}, 1, 0},
    {0x80, (uint8_t[]){0x32}, 1, 0},
    {0x81, (uint8_t[]){0x1F}, 1, 0},
    {0x82, (uint8_t[]){0x0E}, 1, 0},
    {0xE0,(uint8_t []){0x02},1,0},

    {0x00,(uint8_t []){0x1F},1,0},
    {0x01,(uint8_t []){0x1F},1,0},
    {0x02,(uint8_t []){0x52},1,0},
    {0x03,(uint8_t []){0x51},1,0},
    {0x04,(uint8_t []){0x50},1,0},

    {0x05,(uint8_t []){0x4B},1,0},
    {0x06,(uint8_t []){0x4A},1,0},
    {0x07,(uint8_t []){0x49},1,0},
    {0x08,(uint8_t []){0x48},1,0},
    {0x09,(uint8_t []){0x47},1,0},

    {0x0A,(uint8_t []){0x46},1,0},
    {0x0B,(uint8_t []){0x45},1,0},
    {0x0C,(uint8_t []){0x44},1,0},
    {0x0D,(uint8_t []){0x40},1,0},
    {0x0E,(uint8_t []){0x41},1,0},

    {0x0F,(uint8_t []){0x1F},1,0},
    {0x10,(uint8_t []){0x1F},1,0},
    {0x11,(uint8_t []){0x1F},1,0},
    {0x12,(uint8_t []){0x1F},1,0},
    {0x13,(uint8_t []){0x1F},1,0},

    {0x14,(uint8_t []){0x1F},1,0},
    {0x15,(uint8_t []){0x1F},1,0},
    {0x16,(uint8_t []){0x1F},1,0},
    {0x17,(uint8_t []){0x1F},1,0},
    {0x18,(uint8_t []){0x52},1,0},

    {0x19,(uint8_t []){0x51},1,0},
    {0x1A,(uint8_t []){0x50},1,0},
    {0x1B,(uint8_t []){0x4B},1,0},
    {0x1C,(uint8_t []){0x4A},1,0},
    {0x1D,(uint8_t []){0x49},1,0},

    {0x1E,(uint8_t []){0x48},1,0},
    {0x1F,(uint8_t []){0x47},1,0},
    {0x20,(uint8_t []){0x46},1,0},
    {0x21,(uint8_t []){0x45},1,0},
    {0x22,(uint8_t []){0x44},1,0},

    {0x23,(uint8_t []){0x40},1,0},
    {0x24,(uint8_t []){0x41},1,0},
    {0x25,(uint8_t []){0x1F},1,0},
    {0x26,(uint8_t []){0x1F},1,0},
    {0x27,(uint8_t []){0x1F},1,0},

    {0x28,(uint8_t []){0x1F},1,0},
    {0x29,(uint8_t []){0x1F},1,0},
    {0x2A,(uint8_t []){0x1F},1,0},
    {0x2B,(uint8_t []){0x1F},1,0},
    {0x2C,(uint8_t []){0x1F},1,0},

    {0x2D,(uint8_t []){0x1F},1,0},
    {0x2E,(uint8_t []){0x52},1,0},
    {0x2F,(uint8_t []){0x40},1,0},
    {0x30,(uint8_t []){0x41},1,0},
    {0x31,(uint8_t []){0x48},1,0},

    {0x32,(uint8_t []){0x49},1,0},
    {0x33,(uint8_t []){0x4A},1,0},
    {0x34,(uint8_t []){0x4B},1,0},
    {0x35,(uint8_t []){0x44},1,0},
    {0x36,(uint8_t []){0x45},1,0},

    {0x37,(uint8_t []){0x46},1,0},
    {0x38,(uint8_t []){0x47},1,0},
    {0x39,(uint8_t []){0x51},1,0},
    {0x3A,(uint8_t []){0x50},1,0},
    {0x3B,(uint8_t []){0x1F},1,0},

    {0x3C,(uint8_t []){0x1F},1,0},
    {0x3D,(uint8_t []){0x1F},1,0},
    {0x3E,(uint8_t []){0x1F},1,0},
    {0x3F,(uint8_t []){0x1F},1,0},
    {0x40,(uint8_t []){0x1F},1,0},

    {0x41,(uint8_t []){0x1F},1,0},
    {0x42,(uint8_t []){0x1F},1,0},
    {0x43,(uint8_t []){0x1F},1,0},
    {0x44,(uint8_t []){0x52},1,0},
    {0x45,(uint8_t []){0x40},1,0},

    {0x46,(uint8_t []){0x41},1,0},
    {0x47,(uint8_t []){0x48},1,0},
    {0x48,(uint8_t []){0x49},1,0},
    {0x49,(uint8_t []){0x4A},1,0},
    {0x4A,(uint8_t []){0x4B},1,0},

    {0x4B,(uint8_t []){0x44},1,0},
    {0x4C,(uint8_t []){0x45},1,0},
    {0x4D,(uint8_t []){0x46},1,0},
    {0x4E,(uint8_t []){0x47},1,0},
    {0x4F,(uint8_t []){0x51},1,0},

    {0x50,(uint8_t []){0x50},1,0},
    {0x51,(uint8_t []){0x1F},1,0},
    {0x52,(uint8_t []){0x1F},1,0},
    {0x53,(uint8_t []){0x1F},1,0},
    {0x54,(uint8_t []){0x1F},1,0},

    {0x55,(uint8_t []){0x1F},1,0},
    {0x56,(uint8_t []){0x1F},1,0},
    {0x57,(uint8_t []){0x1F},1,0},
    {0x58,(uint8_t []){0x40},1,0},
    {0x59,(uint8_t []){0x00},1,0},

    {0x5A,(uint8_t []){0x00},1,0},
    {0x5B,(uint8_t []){0x10},1,0},
    {0x5C,(uint8_t []){0x05},1,0},
    {0x5D,(uint8_t []){0x50},1,0},
    {0x5E,(uint8_t []){0x01},1,0},

    {0x5F,(uint8_t []){0x02},1,0},
    {0x60,(uint8_t []){0x50},1,0},
    {0x61,(uint8_t []){0x06},1,0},
    {0x62,(uint8_t []){0x04},1,0},
    {0x63,(uint8_t []){0x03},1,0},

    {0x64,(uint8_t []){0x64},1,0},
    {0x65,(uint8_t []){0x65},1,0},
    {0x66,(uint8_t []){0x0B},1,0},
    {0x67,(uint8_t []){0x73},1,0},
    {0x68,(uint8_t []){0x07},1,0},

    {0x69,(uint8_t []){0x06},1,0},
    {0x6A,(uint8_t []){0x64},1,0},
    {0x6B,(uint8_t []){0x08},1,0},
    {0x6C,(uint8_t []){0x00},1,0},
    {0x6D,(uint8_t []){0x32},1,0},

    {0x6E,(uint8_t []){0x08},1,0},
    {0xE0,(uint8_t []){0x04},1,0},
    {0x2C,(uint8_t []){0x6B},1,0},
    {0x35,(uint8_t []){0x08},1,0},
    {0x37,(uint8_t []){0x00},1,0},

    {0xE0,(uint8_t []){0x00},1,0},
    {0x11,(uint8_t []){0x00},1,0},
    {0x29, (uint8_t[]){0x00}, 1, 5},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x35, (uint8_t[]){0x00}, 1, 0},
};
esp_err_t lcd_dsi_panel_factory_entry_t(esp_lcd_dsi_bus_handle_t dsi_handle, dev_display_lcd_config_t *lcd_cfg, dev_display_lcd_handles_t *lcd_handles)
{
    jd9365_vendor_config_t vendor_config = {
        .init_cmds = lcd_cmd,
        .init_cmds_size = sizeof(lcd_cmd) / sizeof(jd9365_lcd_init_cmd_t),
        .mipi_config = {
            .dsi_bus = dsi_handle,
            .dpi_config = &lcd_cfg->sub_cfg.dsi.dpi_config,
        },
    };

    esp_lcd_panel_dev_config_t lcd_dev_config = {
        .reset_gpio_num = lcd_cfg->sub_cfg.dsi.reset_gpio_num,
        .rgb_ele_order = lcd_cfg->rgb_ele_order,
        .bits_per_pixel = lcd_cfg->bits_per_pixel,
        .data_endian = lcd_cfg->data_endian,
        .flags = {
            .reset_active_high = lcd_cfg->sub_cfg.dsi.reset_active_high,
        },
        .vendor_config = &vendor_config,
    };

    esp_err_t ret = esp_lcd_new_panel_jd9365(lcd_handles->io_handle, &lcd_dev_config, &lcd_handles->panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create jd9365 panel: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    return ESP_OK;
}

// esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
// {
    // esp_err_t ret = esp_lcd_touch_new_i2c_gt911(io, touch_dev_config, ret_touch);
    // if (ret != ESP_OK) {
    //     ESP_LOGE("lcd_touch_factory_entry_t", "Failed to create gt911 touch driver: %s", esp_err_to_name(ret));
    //     return ret;
    // }

//     return ESP_OK;
// }
