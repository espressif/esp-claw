/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#define MPU6050_OK                         INT8_C(0)
#define MPU6050_E_NULL_PTR                INT8_C(-1)
#define MPU6050_E_COM_FAIL                INT8_C(-2)
#define MPU6050_E_DEV_NOT_FOUND           INT8_C(-3)
#define MPU6050_E_INVALID_ARG             INT8_C(-4)

#define MPU6050_I2C_ADDRESS_LOW           UINT8_C(0x68)
#define MPU6050_I2C_ADDRESS_HIGH          UINT8_C(0x69)
#define MPU6050_CHIP_ID                   UINT8_C(0x68)

#define MPU6050_REG_SMPLRT_DIV            UINT8_C(0x19)
#define MPU6050_REG_CONFIG                UINT8_C(0x1A)
#define MPU6050_REG_GYRO_CONFIG           UINT8_C(0x1B)
#define MPU6050_REG_ACCEL_CONFIG          UINT8_C(0x1C)
#define MPU6050_REG_INT_ENABLE            UINT8_C(0x38)
#define MPU6050_REG_INT_STATUS            UINT8_C(0x3A)
#define MPU6050_REG_ACCEL_XOUT_H          UINT8_C(0x3B)
#define MPU6050_REG_TEMP_OUT_H            UINT8_C(0x41)
#define MPU6050_REG_PWR_MGMT_1            UINT8_C(0x6B)
#define MPU6050_REG_WHO_AM_I              UINT8_C(0x75)

#define MPU6050_PWR_MGMT_1_SLEEP_DISABLE  UINT8_C(0x00)
#define MPU6050_PWR_MGMT_1_CLKSEL_PLL_XGYRO UINT8_C(0x01)
#define MPU6050_DLPF_CFG_44HZ             UINT8_C(0x03)
#define MPU6050_GYRO_FS_2000DPS           UINT8_C(0x18)
#define MPU6050_ACCEL_FS_16G              UINT8_C(0x18)
#define MPU6050_INT_DATA_RDY_EN           UINT8_C(0x01)

typedef int8_t (*mpu6050_read_f)(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr);
typedef int8_t (*mpu6050_write_f)(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr);
typedef void (*mpu6050_delay_ms_f)(uint32_t period_ms, void *intf_ptr);

typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} mpu6050_raw_axes_t;

typedef struct {
    uint8_t chip_id;
    void *intf_ptr;
    mpu6050_read_f read;
    mpu6050_write_f write;
    mpu6050_delay_ms_f delay_ms;
} mpu6050_dev_t;
