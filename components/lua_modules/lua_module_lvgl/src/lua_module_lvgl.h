/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

int luaopen_lvgl(lua_State *L);
esp_err_t lua_module_lvgl_register(void);
esp_err_t lua_module_lvgl_register_with_data_root(const char *data_root);

#ifdef __cplusplus
}
#endif
