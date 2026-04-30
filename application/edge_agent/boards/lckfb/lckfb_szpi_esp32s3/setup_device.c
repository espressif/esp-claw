/*
 * SPDX-FileCopyrightText: 2026 LCKFB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board setup for LCKFB SZPI ESP32-S3
 * Handles PCA9557 IO expander for LCD_CS, PA_EN, DVP_PWDN
 */

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_board_manager_includes.h"
#include "gen_board_device_custom.h"

static const char *TAG = "szpi_setup";

/* PCA9557 IO Expander */
#define PCA9557_I2C_ADDR            0x19
#define PCA9557_REG_INPUT           0x00
#define PCA9557_REG_OUTPUT          0x01
#define PCA9557_REG_POLARITY        0x02
#define PCA9557_REG_CONFIG          0x03

#define PCA9557_PIN_LCD_CS          BIT(0)
#define PCA9557_PIN_PA_EN           BIT(1)
#define PCA9557_PIN_DVP_PWDN        BIT(2)

static uint8_t s_pca9557_output = 0;

static esp_err_t pca9557_write_reg(uint8_t reg, uint8_t val)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (PCA9557_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_write_byte(cmd, val, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(0, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

static esp_err_t pca9557_init(void)
{
    /* Configure pins 0-2 as output (0 = output in config register) */
    esp_err_t ret = pca9557_write_reg(PCA9557_REG_CONFIG, 0xF8);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Set initial state: LCD_CS=low (active), PA_EN=high (on), DVP_PWDN=high (off) */
    s_pca9557_output = PCA9557_PIN_PA_EN | PCA9557_PIN_DVP_PWDN;
    return pca9557_write_reg(PCA9557_REG_OUTPUT, s_pca9557_output);
}

static int pca9557_expander_init(void *config, int cfg_size, void **device_handle)
{
    (void)config;
    (void)cfg_size;
    (void)device_handle;

    esp_err_t ret = pca9557_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PCA9557 expander init failed");
        return ret;
    }
    ESP_LOGI(TAG, "PCA9557 IO expander initialized (PA enabled, LCD_CS active)");
    return ESP_OK;
}

static int pca9557_expander_deinit(void *device_handle)
{
    (void)device_handle;
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
