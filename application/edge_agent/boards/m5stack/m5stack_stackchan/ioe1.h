/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  StackChan IOE1 (PY32 IO expander) pin assignment
 */
#define STACKCHAN_IOE1_PIN_VM_EN  0   /*!< Servo power rail enable */
#define STACKCHAN_IOE1_PIN_RGB    13  /*!< Data line for the 12 Ring RGB LEDs */

/**
 * @brief  Opaque StackChan IOE1 handle
 */
typedef struct stackchan_ioe1_t *stackchan_ioe1_handle_t;

/**
 * @brief  Board manager custom device entry points
 */
int stackchan_ioe1_init(void *config, int cfg_size, void **device_handle);
int stackchan_ioe1_deinit(void *device_handle);

/**
 * @brief  Set a pin direction
 *
 * @param[in]  handle     IOE1 handle
 * @param[in]  pin        Pin number, 0-13
 * @param[in]  is_output  true for output, false for input
 *
 * @return
 *       - ESP_OK                On success
 *       - ESP_ERR_INVALID_ARG   Invalid handle or pin
 *       - Others                I2C transfer error
 */
esp_err_t stackchan_ioe1_set_dir(stackchan_ioe1_handle_t handle, uint8_t pin, bool is_output);

/**
 * @brief  Select a pin pull direction and enable it
 *
 * @param[in]  handle   IOE1 handle
 * @param[in]  pin      Pin number, 0-13
 * @param[in]  pull_up  true for pull-up, false for pull-down
 *
 * @return
 *       - ESP_OK                On success
 *       - ESP_ERR_INVALID_ARG   Invalid handle or pin
 *       - Others                I2C transfer error
 */
esp_err_t stackchan_ioe1_set_pull(stackchan_ioe1_handle_t handle, uint8_t pin, bool pull_up);

/**
 * @brief  Drive an output pin
 *
 * @param[in]  handle  IOE1 handle
 * @param[in]  pin     Pin number, 0-13
 * @param[in]  level   Output level
 *
 * @return
 *       - ESP_OK                On success
 *       - ESP_ERR_INVALID_ARG   Invalid handle or pin
 *       - Others                I2C transfer error
 */
esp_err_t stackchan_ioe1_set_level(stackchan_ioe1_handle_t handle, uint8_t pin, bool level);

/**
 * @brief  Read an input pin
 *
 * @param[in]   handle     IOE1 handle
 * @param[in]   pin        Pin number, 0-13
 * @param[out]  out_level  Pin level
 *
 * @return
 *       - ESP_OK                On success
 *       - ESP_ERR_INVALID_ARG   Invalid handle, pin or output pointer
 *       - Others                I2C transfer error
 */
esp_err_t stackchan_ioe1_get_level(stackchan_ioe1_handle_t handle, uint8_t pin, bool *out_level);

/**
 * @brief  Read the PY32 firmware version register
 *
 * @param[in]   handle       IOE1 handle
 * @param[out]  out_version  Version byte
 *
 * @return
 *       - ESP_OK                On success
 *       - ESP_ERR_INVALID_ARG   Invalid handle or output pointer
 *       - Others                I2C transfer error
 */
esp_err_t stackchan_ioe1_read_version(stackchan_ioe1_handle_t handle, uint8_t *out_version);

/**
 * @brief  Enable or disable the servo VM power rail
 *
 * @param[in]  handle   IOE1 handle
 * @param[in]  enabled  true to power the servos, false to cut the rail
 *
 * @return
 *       - ESP_OK                On success
 *       - ESP_ERR_INVALID_ARG   Invalid handle
 *       - Others                I2C transfer error
 */
esp_err_t stackchan_ioe1_set_servo_power(stackchan_ioe1_handle_t handle, bool enabled);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
