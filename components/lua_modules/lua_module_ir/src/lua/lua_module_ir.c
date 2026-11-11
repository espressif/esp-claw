/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_ir.h"

#include "cap_lua.h"
#include "lua_ir_device.h"
#include "lua_ir_frame.h"
#include "lua_ir_protocol.h"

int luaopen_ir(lua_State *L)
{
    lua_newtable(L);
    lua_ir_device_register(L);
    lua_ir_frame_register(L);
    lua_ir_protocol_register(L);
    return 1;
}

esp_err_t lua_module_ir_register(void)
{
    return cap_lua_register_module("ir", luaopen_ir);
}
