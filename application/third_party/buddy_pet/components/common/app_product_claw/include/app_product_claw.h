/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_product_claw_start(const app_config_t *config,
                                 const char *data_root,
                                 bool suppress_startup_event);
esp_err_t app_product_claw_update_config(const app_config_t *config,
                                         bool suppress_startup_event);

#ifdef __cplusplus
}
#endif
