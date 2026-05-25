/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_io_expander_tca9554.h"

static const char *TAG = "nm_display_28";

#define AXP2101_I2C_ADDR        0x34
#define AXP2101_SCL_SPEED_HZ    400000

#define AXP2101_REG_DCDC_EN     0x80
#define AXP2101_REG_LDO_EN      0x90
#define AXP2101_REG_DLDO2_EN    0x91

#define AXP2101_REG_DC2_VOL     0x83
#define AXP2101_REG_DC3_VOL     0x84
#define AXP2101_REG_DC4_VOL     0x85
#define AXP2101_REG_DC5_VOL     0x86
#define AXP2101_REG_ALDO1_VOL   0x92
#define AXP2101_REG_ALDO2_VOL   0x93
#define AXP2101_REG_ALDO3_VOL   0x94
#define AXP2101_REG_ALDO4_VOL   0x95
#define AXP2101_REG_BLDO1_VOL   0x96
#define AXP2101_REG_BLDO2_VOL   0x97
#define AXP2101_REG_CPUSLDO_VOL 0x98
#define AXP2101_REG_DLDO1_VOL   0x99
#define AXP2101_REG_DLDO2_VOL   0x9A

#define AXP2101_DC2_1000MV      0x32
#define AXP2101_DC3_3300MV      0x69
#define AXP2101_DC4_1000MV      0x32
#define AXP2101_DC5_3300MV      0x13
#define AXP2101_LDO_3300MV      0x1C
#define AXP2101_BLDO1_1500MV    0x0A
#define AXP2101_BLDO2_2800MV    0x17
#define AXP2101_CPUSLDO_1000MV  0x0A

typedef struct {
    uint8_t reg;
    uint8_t mask;
    uint8_t value;
    const char *name;
} axp2101_update_t;

static esp_err_t axp2101_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, -1);
}

static esp_err_t axp2101_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, sizeof(buf), -1);
}

static esp_err_t axp2101_update_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    esp_err_t ret = axp2101_read_reg(dev, reg, &current);
    if (ret != ESP_OK) {
        return ret;
    }
    current = (current & ~mask) | (value & mask);
    return axp2101_write_reg(dev, reg, current);
}

static esp_err_t axp2101_apply_updates(i2c_master_dev_handle_t dev,
                                       const axp2101_update_t *updates,
                                       size_t update_count)
{
    for (size_t i = 0; i < update_count; i++) {
        esp_err_t ret = axp2101_update_reg(dev, updates[i].reg, updates[i].mask, updates[i].value);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "AXP2101 %s failed: %s", updates[i].name, esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t axp2101_init(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t dev = NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = AXP2101_SCL_SPEED_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add AXP2101 I2C device: %s", esp_err_to_name(ret));
        return ret;
    }

    const axp2101_update_t voltage_updates[] = {
        {AXP2101_REG_DC2_VOL,     0x7F, AXP2101_DC2_1000MV,     "DC2 1000 mV"},
        {AXP2101_REG_DC3_VOL,     0x7F, AXP2101_DC3_3300MV,     "DC3 3300 mV"},
        {AXP2101_REG_DC4_VOL,     0x7F, AXP2101_DC4_1000MV,     "DC4 1000 mV"},
        {AXP2101_REG_DC5_VOL,     0x1F, AXP2101_DC5_3300MV,     "DC5 3300 mV"},
        {AXP2101_REG_ALDO1_VOL,   0x1F, AXP2101_LDO_3300MV,     "ALDO1 3300 mV"},
        {AXP2101_REG_ALDO2_VOL,   0x1F, AXP2101_LDO_3300MV,     "ALDO2 3300 mV"},
        {AXP2101_REG_ALDO3_VOL,   0x1F, AXP2101_LDO_3300MV,     "ALDO3 3300 mV"},
        {AXP2101_REG_ALDO4_VOL,   0x1F, AXP2101_LDO_3300MV,     "ALDO4 3300 mV"},
        {AXP2101_REG_BLDO1_VOL,   0x1F, AXP2101_BLDO1_1500MV,   "BLDO1 1500 mV"},
        {AXP2101_REG_BLDO2_VOL,   0x1F, AXP2101_BLDO2_2800MV,   "BLDO2 2800 mV"},
        {AXP2101_REG_CPUSLDO_VOL, 0x1F, AXP2101_CPUSLDO_1000MV, "CPUSLDO 1000 mV"},
        {AXP2101_REG_DLDO1_VOL,   0x1F, AXP2101_LDO_3300MV,     "DLDO1 3300 mV"},
        {AXP2101_REG_DLDO2_VOL,   0x1F, AXP2101_LDO_3300MV,     "DLDO2 3300 mV"},
    };
    const axp2101_update_t enable_updates[] = {
        {AXP2101_REG_DCDC_EN,  0x1E, 0x1E, "enable DC2-DC5"},
        {AXP2101_REG_LDO_EN,   0xFF, 0xFF, "enable ALDO/BLDO/CPUSLDO/DLDO1"},
        {AXP2101_REG_DLDO2_EN, 0x01, 0x01, "enable DLDO2"},
    };

    ret = axp2101_apply_updates(dev, voltage_updates, sizeof(voltage_updates) / sizeof(voltage_updates[0]));
    if (ret == ESP_OK) {
        ret = axp2101_apply_updates(dev, enable_updates, sizeof(enable_updates) / sizeof(enable_updates[0]));
    }

    esp_err_t rm_ret = i2c_master_bus_rm_device(dev);
    if (ret == ESP_OK && rm_ret != ESP_OK) {
        ret = rm_ret;
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AXP2101 power rails enabled");
    }
    return ret;
}

/* ---------------------------------------------------------------------------
 * io_expander_factory_entry_t
 *
 * Called by esp_board_manager when initializing the 'gpio_expander' device.
 * Responsibilities:
 *   1. Initialize AXP2101 so all power rails come up before any other device.
 *   2. Create the TCA9554 io-expander handle.
 *   3. Reset the LCD/Touch module via TCA9554 PIN_1.
 * --------------------------------------------------------------------------- */
esp_err_t io_expander_factory_entry_t(i2c_master_bus_handle_t i2c_handle,
                                      const uint16_t dev_addr,
                                      esp_io_expander_handle_t *handle_ret)
{
    esp_err_t ret = axp2101_init(i2c_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(20));

    ret = esp_io_expander_new_i2c_tca9554(i2c_handle, dev_addr, handle_ret);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create TCA9554 expander: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_io_expander_set_dir(*handle_ret, IO_EXPANDER_PIN_NUM_1, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(*handle_ret, IO_EXPANDER_PIN_NUM_1, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_io_expander_set_level(*handle_ret, IO_EXPANDER_PIN_NUM_1, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "TCA9554 gpio-expander ready, LCD/Touch reset done");
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * lcd_panel_factory_entry_t
 *
 * Called by esp_board_manager when initializing the 'display_lcd' device.
 * Creates the ST7789 panel driver.
 * --------------------------------------------------------------------------- */
esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(esp_lcd_panel_dev_config_t));
    esp_err_t ret = esp_lcd_new_panel_st7789(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
