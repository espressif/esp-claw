/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "app_claw.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *module_id;
    const char *display_name;
} app_lua_module_info_t;

esp_err_t app_lua_modules_register(const app_claw_config_t *config, const char *fatfs_base_path);
esp_err_t app_lua_modules_get_compiled_modules(const app_lua_module_info_t **modules,
                                               size_t *count);

#ifdef __cplusplus
}
#endif
