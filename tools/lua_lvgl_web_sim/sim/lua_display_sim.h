/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "lua.h"

int luaopen_display(lua_State *L);
void sim_display_cleanup(lua_State *L);
