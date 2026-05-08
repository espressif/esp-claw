/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "claw_cap.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int i2c_port;
    int sda_gpio;
    int scl_gpio;
    uint32_t i2c_freq_hz;
    uint8_t rover_addr;
    uint8_t gripper_servo_idx;
    uint8_t gripper_open_angle;
    uint8_t gripper_close_angle;
    uint32_t hw_task_stack_size;
    UBaseType_t hw_task_priority;
    BaseType_t hw_task_core;
} cap_rover_config_t;

typedef esp_err_t (*cap_rover_imu_read_fn)(float *ax, float *ay, float *az,
                                           float *gx, float *gy, float *gz);

esp_err_t cap_rover_init(const cap_rover_config_t *config);
esp_err_t cap_rover_register_group(void);
void cap_rover_set_imu_read(cap_rover_imu_read_fn fn);
void cap_rover_emergency_stop_set(void);
void cap_rover_emergency_stop_clear(void);
void cap_rover_register_cli(void);

#ifdef __cplusplus
}
#endif
