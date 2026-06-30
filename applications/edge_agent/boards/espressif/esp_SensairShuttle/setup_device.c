/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "esp_board_manager_includes.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "SETUP_DEVICE";

/*
 * CST816S raw coordinate range at the physical screen edges.
 * Touch the four corners and observe "touch raw" log to calibrate.
 */
#define CST816S_RAW_Y_MIN   40
#define CST816S_RAW_Y_MAX   286

static uint16_t touch_linear_map(int32_t raw, int32_t raw_min, int32_t raw_max, int32_t out_max)
{
    if (raw <= raw_min) {
        return 0;
    }
    if (raw >= raw_max) {
        return (uint16_t)out_max;
    }
    return (uint16_t)((raw - raw_min) * out_max / (raw_max - raw_min));
}

static void touch_process_coordinates(esp_lcd_touch_handle_t tp,
                                      uint16_t *x, uint16_t *y,
                                      uint16_t *strength,
                                      uint8_t *point_num,
                                      uint8_t max_point_num)
{
    for (int i = 0; i < *point_num; i++) {
        y[i] = touch_linear_map(y[i], CST816S_RAW_Y_MIN, CST816S_RAW_Y_MAX, tp->config.y_max);
    }
}

static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0x11, NULL, 0, 120},                                                                         // Sleep Out
    {0x36, (uint8_t []){0x00}, 1, 0},                                                             // Memory Data Access Control
    {0x3A, (uint8_t []){0x05}, 1, 0},                                                             // Interface Pixel Format (16-bit)
    {0xB2, (uint8_t []){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5, 0},                                     // Porch Setting
    {0xB7, (uint8_t []){0x05}, 1, 0},                                                             // Gate Control
    {0xBB, (uint8_t []){0x21}, 1, 0},                                                             // VCOM Setting
    {0xC0, (uint8_t []){0x2C}, 1, 0},                                                             // LCM Control
    {0xC2, (uint8_t []){0x01}, 1, 0},                                                             // VDV and VRH Command Enable
    {0xC3, (uint8_t []){0x15}, 1, 0},                                                             // VRH Set
    {0xC6, (uint8_t []){0x0F}, 1, 0},                                                             // Frame Rate Control
    {0xD0, (uint8_t []){0xA7}, 1, 0},                                                             // Power Control 1
    {0xD0, (uint8_t []){0xA4, 0xA1}, 2, 0},                                                       // Power Control 1
    {0xD6, (uint8_t []){0xA1}, 1, 0},                                                             // Gate output GND in sleep mode
    {
        0xE0, (uint8_t [])
        {
            0xF0, 0x05, 0x0E, 0x08, 0x0A, 0x17, 0x39, 0x54,
            0x4E, 0x37, 0x12, 0x12, 0x31, 0x37
        }, 14, 0
    },                              // Positive Gamma Control
    {
        0xE1, (uint8_t [])
        {
            0xF0, 0x10, 0x14, 0x0D, 0x0B, 0x05, 0x39, 0x44,
            0x4D, 0x38, 0x14, 0x14, 0x2E, 0x35
        }, 14, 0
    },                              // Negative Gamma Control
    {0xE4, (uint8_t []){0x23, 0x00, 0x00}, 3, 0},                                                 // Gate position control
    {0x21, NULL, 0, 0},                                                                           // Display Inversion On
    {0x29, NULL, 0, 0},                                                                           // Display On
    {0x2C, NULL, 0, 0},                                                                           // Memory Write
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
        ESP_LOGE(TAG, "New ili9341 panel failed");
        return ret;
    }
    // esp_lcd_panel_set_gap(*ret_panel, 36, 0);
    return ESP_OK;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
{
    esp_lcd_touch_config_t touch_cfg = {0};
    memcpy(&touch_cfg, touch_dev_config, sizeof(esp_lcd_touch_config_t));
    touch_cfg.process_coordinates = touch_process_coordinates;
    if (touch_cfg.int_gpio_num != GPIO_NUM_NC) {
        ESP_LOGW(TAG, "Touch interrupt supported!");
        touch_cfg.interrupt_callback = NULL;
    }
    esp_err_t ret = esp_lcd_touch_new_i2c_cst816s(io, &touch_cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create gt911 touch driver: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
