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

/* Opens the "display" Lua module and pushes the table onto the stack. */
int luaopen_display(lua_State *L);

/* Registers the "display" module with cap_lua so Lua scripts can require it. */
esp_err_t lua_module_display_register(void);

#ifdef __cplusplus
}
#endif
