/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "lua.h"
#include "esp_err.h"

int luaopen_button(lua_State *L);
esp_err_t lua_module_button_register(void);
