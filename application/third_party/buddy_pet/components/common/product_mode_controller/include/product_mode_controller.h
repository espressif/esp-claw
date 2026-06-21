/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool (*is_ready)(void);
} product_mode_controller_config_t;

esp_err_t product_mode_controller_init(const product_mode_controller_config_t *config);
esp_err_t product_mode_controller_open_default(void);

#ifdef __cplusplus
}
#endif
