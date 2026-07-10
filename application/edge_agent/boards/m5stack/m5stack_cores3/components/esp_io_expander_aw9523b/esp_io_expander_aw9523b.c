/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_io_expander_aw9523b.h"

/* I2C communication related */
#define I2C_TIMEOUT_MS (1000)
#define I2C_CLK_SPEED  (400000)

/* Default register value on power-up */
#define DIR_REG_DEFAULT_VAL (0x0000)
#define OUT_REG_DEFAULT_VAL (0x0000)

static char *TAG = "AW9523B";

static esp_err_t read_input_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)__containerof(handle, esp_io_expander_aw9523b_t, base);
    uint16_t temp = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(aw9523b->i2c_handle, (uint8_t[]) {AW9523B_REG_INPUT0}, 1, (uint8_t *)&temp, sizeof(temp), I2C_TIMEOUT_MS), TAG, "Read input reg failed");
    *value = temp;
    return ESP_OK;
}

static esp_err_t write_output_reg(esp_io_expander_handle_t handle, uint32_t value)
{
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)__containerof(handle, esp_io_expander_aw9523b_t, base);
    value &= 0xffff;
    uint8_t data[] = {AW9523B_REG_OUTPUT0, value & 0xff, value >> 8};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(aw9523b->i2c_handle, data, sizeof(data), I2C_TIMEOUT_MS), TAG, "Write output reg failed");
    aw9523b->regs.output = value;
    return ESP_OK;
}

static esp_err_t read_output_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)__containerof(handle, esp_io_expander_aw9523b_t, base);
    *value = aw9523b->regs.output;
    return ESP_OK;
}

static esp_err_t write_direction_reg(esp_io_expander_handle_t handle, uint32_t value)
{
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)__containerof(handle, esp_io_expander_aw9523b_t, base);
    value &= 0xffff;
    uint8_t data[] = {AW9523B_REG_CONFIG0, value & 0xff, value >> 8};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(aw9523b->i2c_handle, data, sizeof(data), I2C_TIMEOUT_MS), TAG, "Write direction reg failed");
    aw9523b->regs.direction = value;
    return ESP_OK;
}

static esp_err_t read_direction_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)__containerof(handle, esp_io_expander_aw9523b_t, base);
    *value = aw9523b->regs.direction;
    return ESP_OK;
}

static esp_err_t reset(esp_io_expander_t *handle)
{
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)__containerof(handle, esp_io_expander_aw9523b_t, base);
    uint8_t data[] = {AW9523B_REG_SOFTRESET, 0};
    ESP_RETURN_ON_ERROR(i2c_master_transmit(aw9523b->i2c_handle, data, sizeof(data), I2C_TIMEOUT_MS), TAG, "Write direction reg failed");
    ESP_RETURN_ON_ERROR(write_direction_reg(handle, DIR_REG_DEFAULT_VAL), TAG, "Write dir reg failed");
    ESP_RETURN_ON_ERROR(write_output_reg(handle, OUT_REG_DEFAULT_VAL), TAG, "Write output reg failed");
    return ESP_OK;
}

static esp_err_t del(esp_io_expander_t *handle)
{
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)__containerof(handle, esp_io_expander_aw9523b_t, base);
    ESP_RETURN_ON_ERROR(i2c_master_bus_rm_device(aw9523b->i2c_handle), TAG, "Remove I2C device failed");
    free(aw9523b);
    return ESP_OK;
}

esp_err_t esp_io_expander_new_aw9523b(i2c_master_bus_handle_t i2c_bus, uint32_t dev_addr, esp_io_expander_handle_t *handle_ret)
{
    ESP_RETURN_ON_FALSE(handle_ret != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid handle_ret");

    // Allocate memory for driver object
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)calloc(1, sizeof(esp_io_expander_aw9523b_t));
    ESP_RETURN_ON_FALSE(aw9523b != NULL, ESP_ERR_NO_MEM, TAG, "Malloc failed");

    // Add new I2C device
    esp_err_t ret = ESP_OK;
    const i2c_device_config_t i2c_dev_cfg = {
        .device_address = dev_addr,
        .scl_speed_hz = I2C_CLK_SPEED,
    };

    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(i2c_bus, &i2c_dev_cfg, &aw9523b->i2c_handle), err, TAG, "Add new I2C device failed");

    aw9523b->base.config.io_count = AW9523B_IO_COUNT;
    aw9523b->base.config.flags.dir_out_bit_zero = 1;
    aw9523b->base.read_input_reg = read_input_reg;
    aw9523b->base.write_output_reg = write_output_reg;
    aw9523b->base.read_output_reg = read_output_reg;
    aw9523b->base.write_direction_reg = write_direction_reg;
    aw9523b->base.read_direction_reg = read_direction_reg;
    aw9523b->base.del = del;
    aw9523b->base.reset = reset;

    /* Reset configuration and register status */
    ESP_GOTO_ON_ERROR(reset(&aw9523b->base), err1, TAG, "Reset failed");

    *handle_ret = &aw9523b->base;
    return ESP_OK;
err1:
    i2c_master_bus_rm_device(aw9523b->i2c_handle);
err:
    free(aw9523b);
    return ret;
}

esp_err_t esp_io_expander_aw9523b_write_reg(esp_io_expander_handle_t handle, uint8_t reg_addr, uint8_t *data, size_t data_len)
{
    esp_io_expander_aw9523b_t *aw9523b = (esp_io_expander_aw9523b_t *)__containerof(handle, esp_io_expander_aw9523b_t, base);
    uint8_t buf[data_len + 1];
    buf[0] = reg_addr;
    memcpy(&buf[1], data, data_len);
    esp_err_t ret = i2c_master_transmit(aw9523b->i2c_handle, buf, data_len + 1, I2C_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Write reg failed");
    }
    return ret;
}
