/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mpu6050.h"

#include <stddef.h>

static int8_t mpu6050_null_ptr_check(const mpu6050_dev_t *dev)
{
    if (dev == NULL || dev->read == NULL || dev->write == NULL || dev->delay_ms == NULL) {
        return MPU6050_E_NULL_PTR;
    }

    return MPU6050_OK;
}

static int8_t mpu6050_read_regs(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, mpu6050_dev_t *dev)
{
    int8_t rslt = mpu6050_null_ptr_check(dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }
    if (reg_data == NULL || len == 0) {
        return MPU6050_E_INVALID_ARG;
    }

    return dev->read(reg_addr, reg_data, len, dev->intf_ptr);
}

static int8_t mpu6050_write_regs(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, mpu6050_dev_t *dev)
{
    int8_t rslt = mpu6050_null_ptr_check(dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }
    if (reg_data == NULL || len == 0) {
        return MPU6050_E_INVALID_ARG;
    }

    return dev->write(reg_addr, reg_data, len, dev->intf_ptr);
}

static int16_t mpu6050_read_be16(const uint8_t *buf)
{
    return (int16_t)(((uint16_t)buf[0] << 8) | buf[1]);
}

static int8_t mpu6050_write_byte(uint8_t reg_addr, uint8_t value, mpu6050_dev_t *dev)
{
    return mpu6050_write_regs(reg_addr, &value, 1, dev);
}

int8_t mpu6050_init(mpu6050_dev_t *dev)
{
    int8_t rslt = mpu6050_null_ptr_check(dev);
    uint8_t chip_id = 0;

    if (rslt != MPU6050_OK) {
        return rslt;
    }

    rslt = mpu6050_read_regs(MPU6050_REG_WHO_AM_I, &chip_id, 1, dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }

    dev->chip_id = chip_id;
    if (chip_id != MPU6050_CHIP_ID) {
        return MPU6050_E_DEV_NOT_FOUND;
    }

    rslt = mpu6050_write_byte(MPU6050_REG_PWR_MGMT_1, MPU6050_PWR_MGMT_1_SLEEP_DISABLE, dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }
    dev->delay_ms(100, dev->intf_ptr);

    rslt = mpu6050_write_byte(MPU6050_REG_PWR_MGMT_1, MPU6050_PWR_MGMT_1_CLKSEL_PLL_XGYRO, dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }

    rslt = mpu6050_write_byte(MPU6050_REG_SMPLRT_DIV, 0x04, dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }

    rslt = mpu6050_write_byte(MPU6050_REG_CONFIG, MPU6050_DLPF_CFG_44HZ, dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }

    rslt = mpu6050_write_byte(MPU6050_REG_GYRO_CONFIG, MPU6050_GYRO_FS_2000DPS, dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }

    rslt = mpu6050_write_byte(MPU6050_REG_ACCEL_CONFIG, MPU6050_ACCEL_FS_16G, dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }

    return mpu6050_write_byte(MPU6050_REG_INT_ENABLE, MPU6050_INT_DATA_RDY_EN, dev);
}

int8_t mpu6050_read_accel_gyro(mpu6050_raw_axes_t *accel,
                               mpu6050_raw_axes_t *gyro,
                               mpu6050_dev_t *dev)
{
    uint8_t raw[14] = { 0 };
    int8_t rslt = mpu6050_null_ptr_check(dev);

    if (rslt != MPU6050_OK) {
        return rslt;
    }
    if (accel == NULL || gyro == NULL) {
        return MPU6050_E_NULL_PTR;
    }

    rslt = mpu6050_read_regs(MPU6050_REG_ACCEL_XOUT_H, raw, sizeof(raw), dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }

    accel->x = mpu6050_read_be16(&raw[0]);
    accel->y = mpu6050_read_be16(&raw[2]);
    accel->z = mpu6050_read_be16(&raw[4]);
    gyro->x = mpu6050_read_be16(&raw[8]);
    gyro->y = mpu6050_read_be16(&raw[10]);
    gyro->z = mpu6050_read_be16(&raw[12]);

    return MPU6050_OK;
}

int8_t mpu6050_read_temperature_raw(int16_t *temp_raw, mpu6050_dev_t *dev)
{
    uint8_t raw[2] = { 0 };
    int8_t rslt = mpu6050_null_ptr_check(dev);

    if (rslt != MPU6050_OK) {
        return rslt;
    }
    if (temp_raw == NULL) {
        return MPU6050_E_NULL_PTR;
    }

    rslt = mpu6050_read_regs(MPU6050_REG_TEMP_OUT_H, raw, sizeof(raw), dev);
    if (rslt != MPU6050_OK) {
        return rslt;
    }

    *temp_raw = mpu6050_read_be16(raw);
    return MPU6050_OK;
}

int8_t mpu6050_get_int_status(uint8_t *int_status, mpu6050_dev_t *dev)
{
    int8_t rslt = mpu6050_null_ptr_check(dev);

    if (rslt != MPU6050_OK) {
        return rslt;
    }
    if (int_status == NULL) {
        return MPU6050_E_NULL_PTR;
    }

    return mpu6050_read_regs(MPU6050_REG_INT_STATUS, int_status, 1, dev);
}
