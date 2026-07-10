/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_board_device.h"
#include "esp_board_periph.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const st77916_lcd_init_cmd_t vendor_specific_init_default[] = {
    {0xF0, (uint8_t[]) {0x28}, 1, 0},
    {0xF2, (uint8_t[]) {0x28}, 1, 0},
    {0x73, (uint8_t[]) {0xF0}, 1, 0},
    {0x7C, (uint8_t[]) {0xD1}, 1, 0},
    {0x83, (uint8_t[]) {0xE0}, 1, 0},
    {0x84, (uint8_t[]) {0x61}, 1, 0},
    {0xF2, (uint8_t[]) {0x82}, 1, 0},
    {0xF0, (uint8_t[]) {0x00}, 1, 0},
    {0xF0, (uint8_t[]) {0x01}, 1, 0},
    {0xF1, (uint8_t[]) {0x01}, 1, 0},
    {0xB0, (uint8_t[]) {0x56}, 1, 0},
    {0xB1, (uint8_t[]) {0x4D}, 1, 0},
    {0xB2, (uint8_t[]) {0x24}, 1, 0},
    {0xB4, (uint8_t[]) {0x87}, 1, 0},
    {0xB5, (uint8_t[]) {0x44}, 1, 0},
    {0xB6, (uint8_t[]) {0x8B}, 1, 0},
    {0xB7, (uint8_t[]) {0x40}, 1, 0},
    {0xB8, (uint8_t[]) {0x86}, 1, 0},
    {0xBA, (uint8_t[]) {0x00}, 1, 0},
    {0xBB, (uint8_t[]) {0x08}, 1, 0},
    {0xBC, (uint8_t[]) {0x08}, 1, 0},
    {0xBD, (uint8_t[]) {0x00}, 1, 0},
    {0xC0, (uint8_t[]) {0x80}, 1, 0},
    {0xC1, (uint8_t[]) {0x10}, 1, 0},
    {0xC2, (uint8_t[]) {0x37}, 1, 0},
    {0xC3, (uint8_t[]) {0x80}, 1, 0},
    {0xC4, (uint8_t[]) {0x10}, 1, 0},
    {0xC5, (uint8_t[]) {0x37}, 1, 0},
    {0xC6, (uint8_t[]) {0xA9}, 1, 0},
    {0xC7, (uint8_t[]) {0x41}, 1, 0},
    {0xC8, (uint8_t[]) {0x01}, 1, 0},
    {0xC9, (uint8_t[]) {0xA9}, 1, 0},
    {0xCA, (uint8_t[]) {0x41}, 1, 0},
    {0xCB, (uint8_t[]) {0x01}, 1, 0},
    {0xD0, (uint8_t[]) {0x91}, 1, 0},
    {0xD1, (uint8_t[]) {0x68}, 1, 0},
    {0xD2, (uint8_t[]) {0x68}, 1, 0},
    {0xF5, (uint8_t[]) {0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]) {0x4F}, 1, 0},
    {0xDE, (uint8_t[]) {0x4F}, 1, 0},
    {0xF1, (uint8_t[]) {0x10}, 1, 0},
    {0xF0, (uint8_t[]) {0x00}, 1, 0},
    {0xF0, (uint8_t[]) {0x02}, 1, 0},
    {0xE0, (uint8_t[]) {0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]) {0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]) {0x10}, 1, 0},
    {0xF3, (uint8_t[]) {0x10}, 1, 0},
    {0xE0, (uint8_t[]) {0x07}, 1, 0},
    {0xE1, (uint8_t[]) {0x00}, 1, 0},
    {0xE2, (uint8_t[]) {0x00}, 1, 0},
    {0xE3, (uint8_t[]) {0x00}, 1, 0},
    {0xE4, (uint8_t[]) {0xE0}, 1, 0},
    {0xE5, (uint8_t[]) {0x06}, 1, 0},
    {0xE6, (uint8_t[]) {0x21}, 1, 0},
    {0xE7, (uint8_t[]) {0x01}, 1, 0},
    {0xE8, (uint8_t[]) {0x05}, 1, 0},
    {0xE9, (uint8_t[]) {0x02}, 1, 0},
    {0xEA, (uint8_t[]) {0xDA}, 1, 0},
    {0xEB, (uint8_t[]) {0x00}, 1, 0},
    {0xEC, (uint8_t[]) {0x00}, 1, 0},
    {0xED, (uint8_t[]) {0x0F}, 1, 0},
    {0xEE, (uint8_t[]) {0x00}, 1, 0},
    {0xEF, (uint8_t[]) {0x00}, 1, 0},
    {0xF8, (uint8_t[]) {0x00}, 1, 0},
    {0xF9, (uint8_t[]) {0x00}, 1, 0},
    {0xFA, (uint8_t[]) {0x00}, 1, 0},
    {0xFB, (uint8_t[]) {0x00}, 1, 0},
    {0xFC, (uint8_t[]) {0x00}, 1, 0},
    {0xFD, (uint8_t[]) {0x00}, 1, 0},
    {0xFE, (uint8_t[]) {0x00}, 1, 0},
    {0xFF, (uint8_t[]) {0x00}, 1, 0},
    {0x60, (uint8_t[]) {0x40}, 1, 0},
    {0x61, (uint8_t[]) {0x04}, 1, 0},
    {0x62, (uint8_t[]) {0x00}, 1, 0},
    {0x63, (uint8_t[]) {0x42}, 1, 0},
    {0x64, (uint8_t[]) {0xD9}, 1, 0},
    {0x65, (uint8_t[]) {0x00}, 1, 0},
    {0x66, (uint8_t[]) {0x00}, 1, 0},
    {0x67, (uint8_t[]) {0x00}, 1, 0},
    {0x68, (uint8_t[]) {0x00}, 1, 0},
    {0x69, (uint8_t[]) {0x00}, 1, 0},
    {0x6A, (uint8_t[]) {0x00}, 1, 0},
    {0x6B, (uint8_t[]) {0x00}, 1, 0},
    {0x70, (uint8_t[]) {0x40}, 1, 0},
    {0x71, (uint8_t[]) {0x03}, 1, 0},
    {0x72, (uint8_t[]) {0x00}, 1, 0},
    {0x73, (uint8_t[]) {0x42}, 1, 0},
    {0x74, (uint8_t[]) {0xD8}, 1, 0},
    {0x75, (uint8_t[]) {0x00}, 1, 0},
    {0x76, (uint8_t[]) {0x00}, 1, 0},
    {0x77, (uint8_t[]) {0x00}, 1, 0},
    {0x78, (uint8_t[]) {0x00}, 1, 0},
    {0x79, (uint8_t[]) {0x00}, 1, 0},
    {0x7A, (uint8_t[]) {0x00}, 1, 0},
    {0x7B, (uint8_t[]) {0x00}, 1, 0},
    {0x80, (uint8_t[]) {0x48}, 1, 0},
    {0x81, (uint8_t[]) {0x00}, 1, 0},
    {0x82, (uint8_t[]) {0x06}, 1, 0},
    {0x83, (uint8_t[]) {0x02}, 1, 0},
    {0x84, (uint8_t[]) {0xD6}, 1, 0},
    {0x85, (uint8_t[]) {0x04}, 1, 0},
    {0x86, (uint8_t[]) {0x00}, 1, 0},
    {0x87, (uint8_t[]) {0x00}, 1, 0},
    {0x88, (uint8_t[]) {0x48}, 1, 0},
    {0x89, (uint8_t[]) {0x00}, 1, 0},
    {0x8A, (uint8_t[]) {0x08}, 1, 0},
    {0x8B, (uint8_t[]) {0x02}, 1, 0},
    {0x8C, (uint8_t[]) {0xD8}, 1, 0},
    {0x8D, (uint8_t[]) {0x04}, 1, 0},
    {0x8E, (uint8_t[]) {0x00}, 1, 0},
    {0x8F, (uint8_t[]) {0x00}, 1, 0},
    {0x90, (uint8_t[]) {0x48}, 1, 0},
    {0x91, (uint8_t[]) {0x00}, 1, 0},
    {0x92, (uint8_t[]) {0x0A}, 1, 0},
    {0x93, (uint8_t[]) {0x02}, 1, 0},
    {0x94, (uint8_t[]) {0xDA}, 1, 0},
    {0x95, (uint8_t[]) {0x04}, 1, 0},
    {0x96, (uint8_t[]) {0x00}, 1, 0},
    {0x97, (uint8_t[]) {0x00}, 1, 0},
    {0x98, (uint8_t[]) {0x48}, 1, 0},
    {0x99, (uint8_t[]) {0x00}, 1, 0},
    {0x9A, (uint8_t[]) {0x0C}, 1, 0},
    {0x9B, (uint8_t[]) {0x02}, 1, 0},
    {0x9C, (uint8_t[]) {0xDC}, 1, 0},
    {0x9D, (uint8_t[]) {0x04}, 1, 0},
    {0x9E, (uint8_t[]) {0x00}, 1, 0},
    {0x9F, (uint8_t[]) {0x00}, 1, 0},
    {0xA0, (uint8_t[]) {0x48}, 1, 0},
    {0xA1, (uint8_t[]) {0x00}, 1, 0},
    {0xA2, (uint8_t[]) {0x05}, 1, 0},
    {0xA3, (uint8_t[]) {0x02}, 1, 0},
    {0xA4, (uint8_t[]) {0xD5}, 1, 0},
    {0xA5, (uint8_t[]) {0x04}, 1, 0},
    {0xA6, (uint8_t[]) {0x00}, 1, 0},
    {0xA7, (uint8_t[]) {0x00}, 1, 0},
    {0xA8, (uint8_t[]) {0x48}, 1, 0},
    {0xA9, (uint8_t[]) {0x00}, 1, 0},
    {0xAA, (uint8_t[]) {0x07}, 1, 0},
    {0xAB, (uint8_t[]) {0x02}, 1, 0},
    {0xAC, (uint8_t[]) {0xD7}, 1, 0},
    {0xAD, (uint8_t[]) {0x04}, 1, 0},
    {0xAE, (uint8_t[]) {0x00}, 1, 0},
    {0xAF, (uint8_t[]) {0x00}, 1, 0},
    {0xB0, (uint8_t[]) {0x48}, 1, 0},
    {0xB1, (uint8_t[]) {0x00}, 1, 0},
    {0xB2, (uint8_t[]) {0x09}, 1, 0},
    {0xB3, (uint8_t[]) {0x02}, 1, 0},
    {0xB4, (uint8_t[]) {0xD9}, 1, 0},
    {0xB5, (uint8_t[]) {0x04}, 1, 0},
    {0xB6, (uint8_t[]) {0x00}, 1, 0},
    {0xB7, (uint8_t[]) {0x00}, 1, 0},
    {0xB8, (uint8_t[]) {0x48}, 1, 0},
    {0xB9, (uint8_t[]) {0x00}, 1, 0},
    {0xBA, (uint8_t[]) {0x0B}, 1, 0},
    {0xBB, (uint8_t[]) {0x02}, 1, 0},
    {0xBC, (uint8_t[]) {0xDB}, 1, 0},
    {0xBD, (uint8_t[]) {0x04}, 1, 0},
    {0xBE, (uint8_t[]) {0x00}, 1, 0},
    {0xBF, (uint8_t[]) {0x00}, 1, 0},
    {0xC0, (uint8_t[]) {0x10}, 1, 0},
    {0xC1, (uint8_t[]) {0x47}, 1, 0},
    {0xC2, (uint8_t[]) {0x56}, 1, 0},
    {0xC3, (uint8_t[]) {0x65}, 1, 0},
    {0xC4, (uint8_t[]) {0x74}, 1, 0},
    {0xC5, (uint8_t[]) {0x88}, 1, 0},
    {0xC6, (uint8_t[]) {0x99}, 1, 0},
    {0xC7, (uint8_t[]) {0x01}, 1, 0},
    {0xC8, (uint8_t[]) {0xBB}, 1, 0},
    {0xC9, (uint8_t[]) {0xAA}, 1, 0},
    {0xD0, (uint8_t[]) {0x10}, 1, 0},
    {0xD1, (uint8_t[]) {0x47}, 1, 0},
    {0xD2, (uint8_t[]) {0x56}, 1, 0},
    {0xD3, (uint8_t[]) {0x65}, 1, 0},
    {0xD4, (uint8_t[]) {0x74}, 1, 0},
    {0xD5, (uint8_t[]) {0x88}, 1, 0},
    {0xD6, (uint8_t[]) {0x99}, 1, 0},
    {0xD7, (uint8_t[]) {0x01}, 1, 0},
    {0xD8, (uint8_t[]) {0xBB}, 1, 0},
    {0xD9, (uint8_t[]) {0xAA}, 1, 0},
    {0xF3, (uint8_t[]) {0x01}, 1, 0},
    {0xF0, (uint8_t[]) {0x00}, 1, 0},
    {0x21, (uint8_t[]) {}, 0, 0},
    {0x11, (uint8_t[]) {}, 0, 0},
    {0x00, (uint8_t[]) {}, 0, 120},
};

// define st77916_vendor_config_t
static const st77916_vendor_config_t vendor_config = {
    .init_cmds = vendor_specific_init_default,
    .init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(vendor_specific_init_default[0]),
    .flags = {
        .use_qspi_interface = 1,  // QSPI
    }
};

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    panel_dev_cfg.vendor_config = (void *)&vendor_config;
    int ret = esp_lcd_new_panel_st77916(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE("lcd_panel_factory_entry_t", "New ST77916 panel failed");
        return ret;
    }
    return ESP_OK;
}

/* CST816T 无触摸时进入低功耗、I2C 不响应,需中断驱动读取。
 * 契约: interrupt_callback 非空且 user_data 是二值信号量 -> 上层走中断模式。 */
static IRAM_ATTR void cst816t_interrupt_cb(esp_lcd_touch_handle_t tp)
{
    SemaphoreHandle_t sem = (tp != NULL) ? (SemaphoreHandle_t)tp->config.user_data : NULL;
    if (sem == NULL) {
        return;
    }
    BaseType_t higher_prio_woken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &higher_prio_woken);
    if (higher_prio_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *touch_dev_config, esp_lcd_touch_handle_t *ret_touch)
{
    static SemaphoreHandle_t s_cst816t_int_sem = NULL;
    esp_lcd_touch_config_t touch_cfg = {0};
    memcpy(&touch_cfg, touch_dev_config, sizeof(esp_lcd_touch_config_t));

    if (s_cst816t_int_sem == NULL) {
        s_cst816t_int_sem = xSemaphoreCreateBinary();
    }
    touch_cfg.interrupt_callback = cst816t_interrupt_cb;
    touch_cfg.user_data = (void *)s_cst816t_int_sem;

    esp_err_t ret = esp_lcd_touch_new_i2c_cst816s(io, &touch_cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE("lcd_touch_factory_entry_t", "Failed to create CST816S touch driver: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
