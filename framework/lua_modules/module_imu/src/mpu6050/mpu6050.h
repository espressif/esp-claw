/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "mpu6050_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

int8_t mpu6050_init(mpu6050_dev_t *dev);
int8_t mpu6050_read_accel_gyro(mpu6050_raw_axes_t *accel,
                               mpu6050_raw_axes_t *gyro,
                               mpu6050_dev_t *dev);
int8_t mpu6050_read_temperature_raw(int16_t *temp_raw, mpu6050_dev_t *dev);
int8_t mpu6050_get_int_status(uint8_t *int_status, mpu6050_dev_t *dev);

#ifdef __cplusplus
}
#endif
