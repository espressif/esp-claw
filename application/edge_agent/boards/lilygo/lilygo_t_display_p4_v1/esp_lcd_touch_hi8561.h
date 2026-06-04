/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_lcd_touch_new_i2c_hi8561(i2c_master_dev_handle_t i2c_dev,
                                       const esp_lcd_touch_config_t *config,
                                       esp_lcd_touch_handle_t *ret_touch);

#ifdef __cplusplus
}
#endif
