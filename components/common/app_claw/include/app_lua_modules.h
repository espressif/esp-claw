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

typedef esp_err_t (*app_lua_module_register_fn)(const char *fatfs_base_path);

typedef struct {
    const char *module_id;
    const char *display_name;
    app_lua_module_register_fn reg;
} app_lua_module_external_t;

/**
 * @brief Register an application-provided Lua module for app_claw startup.
 *
 *        Call this before app_claw_start(). The module participates in
 *        enabled_lua_modules using module_id, alongside built-in app_claw Lua
 *        modules.
 */
esp_err_t app_lua_modules_register_external(const app_lua_module_external_t *module);
esp_err_t app_lua_modules_register(const app_claw_config_t *config, const char *fatfs_base_path);
esp_err_t app_lua_modules_get_compiled_modules(const app_lua_module_info_t **modules,
                                               size_t *count);

#ifdef __cplusplus
}
#endif
