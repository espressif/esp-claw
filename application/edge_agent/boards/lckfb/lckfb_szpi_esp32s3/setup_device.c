/*
 * SPDX-FileCopyrightText: 2026 LCKFB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board setup for LCKFB SZPI ESP32-S3
 * Handles PCA9557 IO expander for LCD_CS, PA_EN, DVP_PWDN
 */

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_board_periph.h"
#include "gen_board_device_custom.h"

static const char *TAG = "szpi_setup";

/* PCA9557 IO Expander */
#define PCA9557_I2C_ADDR            0x19
#define PCA9557_REG_OUTPUT          0x01
#define PCA9557_REG_CONFIG          0x03

#define PCA9557_PIN_LCD_CS          BIT(0)
#define PCA9557_PIN_PA_EN           BIT(1)
#define PCA9557_PIN_DVP_PWDN        BIT(2)

static i2c_master_dev_handle_t s_pca9557_dev = NULL;

static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t data[2] = {reg, val};
    return i2c_master_transmit(s_pca9557_dev, data, sizeof(data), 100);
}

static int pca9557_expander_init(void *config, int cfg_size, void **device_handle)
{
    dev_custom_pca9557_expander_config_t *cfg = (dev_custom_pca9557_expander_config_t *)config;

    /* Get the I2C bus handle from the board manager */
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t ret = esp_board_periph_get_handle(cfg->peripheral_name, (void **)&i2c_bus);
    if (ret != ESP_OK || i2c_bus == NULL) {
        ESP_LOGE(TAG, "Failed to get I2C bus '%s': %s", cfg->peripheral_name, esp_err_to_name(ret));
        return ret;
    }

    /* Add PCA9557 device to the I2C bus */
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCA9557_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &s_pca9557_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add PCA9557 to I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure pins 0-2 as output (0 = output in config register) */
    ret = pca9557_write_reg(PCA9557_REG_CONFIG, 0xF8);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 config register write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set initial state: LCD_CS=low (active), PA_EN=high (on), DVP_PWDN=high (off) */
    uint8_t output_val = PCA9557_PIN_PA_EN | PCA9557_PIN_DVP_PWDN;
    ret = pca9557_write_reg(PCA9557_REG_OUTPUT, output_val);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 output register write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "PCA9557 IO expander initialized (PA enabled, LCD_CS active)");
    *device_handle = (void *)s_pca9557_dev;
    return ESP_OK;
}

static int pca9557_expander_deinit(void *device_handle)
{
    if (s_pca9557_dev) {
        i2c_master_bus_rm_device(s_pca9557_dev);
        s_pca9557_dev = NULL;
    }
    return ESP_OK;
}

CUSTOM_DEVICE_IMPLEMENT(pca9557_expander, pca9557_expander_init, pca9557_expander_deinit);

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));

    int ret = esp_lcd_new_panel_st7789(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "New ST7789 panel failed");
        return ret;
    }
    return ESP_OK;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                     const esp_lcd_touch_config_t *touch_dev_config,
                                     esp_lcd_touch_handle_t *ret_touch)
{
    esp_err_t ret = esp_lcd_touch_new_i2c_ft5x06(io, touch_dev_config, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create FT6336 touch driver: %s", esp_err_to_name(ret));
    }
    return ret;
}
