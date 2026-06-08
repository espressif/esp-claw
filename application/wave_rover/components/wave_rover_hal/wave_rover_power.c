/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "wr_power";
static i2c_master_dev_handle_t s_ina219_dev = NULL;

#define INA219_REG_CONFIG   0x00
#define INA219_REG_SHUNT    0x01
#define INA219_REG_BUS      0x02
#define INA219_REG_POWER    0x03
#define INA219_REG_CURRENT  0x04
#define INA219_REG_CALIB    0x05

/* BRNG=0(16V), PGA=2(+/-320mV), BADC=9bit, SADC=9bit, mode=7(cont) */
#define INA219_CONFIG_VAL   0x219F
#define INA219_CALIB_VAL    0x8000  /* calibrated for 0.01 ohm shunt */
#define INA219_CURRENT_LSB  0.001f  /* 1mA per bit */

static esp_err_t ina219_read_reg(uint8_t reg, uint16_t *val)
{
    uint8_t buf[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_ina219_dev, &reg, 1, 50), TAG, "tx");
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_ina219_dev, buf, 2, 50), TAG, "rx");
    *val = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

static esp_err_t ina219_write_reg(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_master_transmit(s_ina219_dev, buf, 3, 50);
}

static bool s_ina219_failed = false;

static esp_err_t ina219_init(void)
{
    if (!g_wr_i2c_bus || s_ina219_failed) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = WR_INA219_ADDR,
        .scl_speed_hz    = WR_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(g_wr_i2c_bus, &dev_cfg, &s_ina219_dev);
    if (err != ESP_OK) { s_ina219_failed = true; return err; }
    if (ina219_write_reg(INA219_REG_CONFIG, INA219_CONFIG_VAL) != ESP_OK ||
        ina219_write_reg(INA219_REG_CALIB,  INA219_CALIB_VAL)  != ESP_OK) {
        i2c_master_bus_rm_device(s_ina219_dev);
        s_ina219_dev = NULL;
        s_ina219_failed = true;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "INA219 initialized at 0x%02X", WR_INA219_ADDR);
    return ESP_OK;
}

esp_err_t wr_power_get_status(wr_power_status_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));

    if (!g_wr_i2c_bus) {
        s->present        = false;
        s->bus_voltage_v  = 11.8f;
        s->load_voltage_v = 11.8f;
        return ESP_OK;
    }

    if (!s_ina219_dev) {
        esp_err_t e = ina219_init();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "INA219 init failed: %s", esp_err_to_name(e));
            return ESP_OK;
        }
    }

    uint16_t shunt_raw, bus_raw, power_raw, current_raw;
    if (ina219_read_reg(INA219_REG_SHUNT,   &shunt_raw)   != ESP_OK ||
        ina219_read_reg(INA219_REG_BUS,     &bus_raw)     != ESP_OK ||
        ina219_read_reg(INA219_REG_POWER,   &power_raw)   != ESP_OK ||
        ina219_read_reg(INA219_REG_CURRENT, &current_raw) != ESP_OK) {
        return ESP_OK;
    }

    s->present          = true;
    s->shunt_voltage_mv = (int16_t)shunt_raw * 0.01f;
    s->bus_voltage_v    = (float)(bus_raw >> 3) * 0.004f;
    s->load_voltage_v   = s->bus_voltage_v + s->shunt_voltage_mv / 1000.0f;
    s->current_ma       = (int16_t)current_raw * INA219_CURRENT_LSB * 1000.0f;
    s->power_mw         = power_raw * INA219_CURRENT_LSB * 20.0f * 1000.0f;
    s->charging         = (s->current_ma < -50.0f);
    s->low_battery      = (s->load_voltage_v < WR_LOW_BATT_V && s->load_voltage_v > 1.0f);
    return ESP_OK;
}
