/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ir_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ir_device_t *ir_device_handle_t;

typedef struct {
    int tx_gpio;
    int rx_gpio;
    int ctrl_gpio;
    int ctrl_active_level;
    bool tx_invert;
    bool rx_invert;
    uint32_t carrier_hz;
    float carrier_duty;
    uint32_t tx_resolution_hz;
    uint32_t rx_resolution_hz;
    int rx_max_symbols;
    uint32_t signal_range_min_ns;
    uint32_t signal_range_max_ns;
} ir_device_config_t;

typedef struct {
    uint32_t signal_range_min_ns;
    uint32_t signal_range_max_ns;
} ir_device_receive_config_t;

typedef enum {
    IR_DEVICE_RECEIVE_FRAME = 0,
    IR_DEVICE_RECEIVE_TIMEOUT,
    IR_DEVICE_RECEIVE_EMPTY_FRAME,
    IR_DEVICE_RECEIVE_FRAME_TOO_LONG,
} ir_device_receive_status_t;

typedef struct {
    ir_device_receive_status_t status;
    /* The caller owns frame and releases it with ir_device_receive_result_clear(). */
    ir_frame_t *frame;
} ir_device_receive_result_t;

esp_err_t ir_device_create(const ir_device_config_t *config, ir_device_handle_t *ret_handle);
esp_err_t ir_device_delete(ir_device_handle_t handle);
esp_err_t ir_device_send(ir_device_handle_t handle, const ir_frame_t *frame, int timeout_ms);
esp_err_t ir_device_start_receive(ir_device_handle_t handle, const ir_device_receive_config_t *config);
esp_err_t ir_device_read(ir_device_handle_t handle, int timeout_ms, bool cancel_on_timeout, ir_device_receive_result_t *result);
esp_err_t ir_device_stop_receive(ir_device_handle_t handle);
bool ir_device_is_receiving(ir_device_handle_t handle);
const ir_device_config_t *ir_device_get_config(ir_device_handle_t handle);
void ir_device_receive_result_clear(ir_device_receive_result_t *result);

#ifdef __cplusplus
}
#endif
