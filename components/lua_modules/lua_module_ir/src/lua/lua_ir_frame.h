/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "ir_frame.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LUA_IR_FRAME_MAX_PULSES IR_FRAME_MAX_PULSES
#define LUA_IR_FRAME_MAX_PULSE_US IR_FRAME_MAX_PULSE_US
#define LUA_IR_FRAME_MIN_CARRIER_HZ IR_FRAME_MIN_CARRIER_HZ
#define LUA_IR_FRAME_MAX_CARRIER_HZ IR_FRAME_MAX_CARRIER_HZ

/* Registers ir.frame() in the module table at the top of the stack. */
void lua_ir_frame_register(lua_State *L);

/* Allocates and pushes an empty frame with inline pulse storage. */
ir_frame_t *lua_ir_frame_create(lua_State *L, size_t capacity);

/* Pushes a Lua-owned copy of source. */
ir_frame_t *lua_ir_frame_push(lua_State *L, const ir_frame_t *source);

ir_frame_t *lua_ir_frame_from_table(lua_State *L, int index);

ir_frame_t *lua_ir_frame_check(lua_State *L, int index);
#ifdef __cplusplus
}
#endif
