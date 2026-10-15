/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_lcd_touch_new_i2c_cst226se(esp_lcd_panel_io_handle_t io,
                                         const esp_lcd_touch_config_t *config,
                                         esp_lcd_touch_handle_t *ret_touch);

#ifdef __cplusplus
}
#endif
