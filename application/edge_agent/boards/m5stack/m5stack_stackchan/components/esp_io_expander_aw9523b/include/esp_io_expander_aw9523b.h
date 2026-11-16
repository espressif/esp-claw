/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_io_expander.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

#define AW9523B_IO_COUNT (16)

/* Register address */
#define AW9523B_I2C_ADDR      0x58  // Default I2C address
#define AW9523B_REG_INPUT0    0x00
#define AW9523B_REG_INPUT1    0x01
#define AW9523B_REG_OUTPUT0   0x02
#define AW9523B_REG_OUTPUT1   0x03
#define AW9523B_REG_CONFIG0   0x04
#define AW9523B_REG_CONFIG1   0x05
#define AW9523B_REG_LEDMODE0  0x12
#define AW9523B_REG_LEDMODE1  0x13
#define AW9523B_REG_INTR0     0x06
#define AW9523B_REG_INTR1     0x07
#define AW9523B_REG_ID        0x10
#define AW9523B_REG_SOFTRESET 0x7F
#define AW9523B_REG_GCR       0x11

/**
 * @brief  Device Structure Type
 *
 */
typedef struct {
    esp_io_expander_t        base;
    i2c_master_dev_handle_t  i2c_handle;
    struct {
        uint16_t  direction;
        uint16_t  output;
    } regs;
} esp_io_expander_aw9523b_t;

/**
 * @brief  Create a AW9523B IO expander object
 *
 * @note   In cores3, the AW9523B IO expander needs to be initialized before the AXP2101 PMU,
 *         because it is used when enabling the power
 *
 * @param[in]   i2c_bus     I2C bus handle. Obtained from `i2c_new_master_bus()`
 * @param[in]   dev_addr    I2C device address of chip.
 * @param[out]  handle_ret  Handle to created IO expander object
 *
 * @return
 *       - ESP_OK  Success, otherwise returns ESP_ERR_xxx
 */
esp_err_t esp_io_expander_new_aw9523b(i2c_master_bus_handle_t i2c_bus, uint32_t dev_addr, esp_io_expander_handle_t *handle_ret);

/**
 * @brief  Write data to the specified AW9523B register
 *
 * @param[in]  handle    IO expander handle
 * @param[in]  reg_addr  Register address to write
 * @param[in]  data      Data buffer to write
 * @param[in]  data_len  Length of data to write
 *
 * @return
 *       - ESP_OK          Success
 *       - ESP_ERR_NO_MEM  Memory allocation failed
 *       - ESP_FAIL        I2C transmission failed
 */
esp_err_t esp_io_expander_aw9523b_write_reg(esp_io_expander_handle_t handle, uint8_t reg_addr, uint8_t *data, size_t data_len);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
