/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdatomic.h>
#include <stdint.h>

#include "cap_rover.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ROVER_ACTION_MOVE = 1,
    ROVER_ACTION_TURN = 2,
    ROVER_ACTION_STOP = 3,
    ROVER_ACTION_GRIPPER_OPEN = 4,
    ROVER_ACTION_GRIPPER_CLOSE = 5,
} rover_action_kind_t;

typedef struct {
    uint32_t req_id;
    rover_action_kind_t kind;
    int8_t x;
    int8_t y;
    int8_t z;
    uint16_t duration_ms;
    uint16_t turn_target_deg;
    uint16_t turn_timeout_ms;
} rover_action_req_t;

typedef struct {
    uint32_t req_id;
    esp_err_t err;
    float turn_measured_deg;
} rover_action_result_t;

typedef struct {
    cap_rover_config_t cfg;
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_dev;
    QueueHandle_t req_queue;
    QueueHandle_t result_queue;
    SemaphoreHandle_t queue_lock;
    TaskHandle_t hw_task;
    cap_rover_imu_read_fn imu_read;
    cap_rover_power_read_fn power_read;
    volatile bool emergency_requested;
    atomic_uint_fast32_t req_seq;
    bool initialized;
} cap_rover_state_t;

extern cap_rover_state_t g_cap_rover;

esp_err_t cap_rover_submit_and_wait(rover_action_req_t *req,
                                    TickType_t timeout,
                                    rover_action_result_t *out_result);
esp_err_t cap_rover_hw_set_speed(int8_t x, int8_t y, int8_t z);
esp_err_t cap_rover_hw_set_servo_angle(uint8_t servo_idx, uint8_t angle);
void cap_rover_hw_zero_motors(void);
esp_err_t cap_rover_hw_start(void);

#ifdef __cplusplus
}
#endif
