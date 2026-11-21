/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_ir_frame.h"

#include <string.h>

#include "lauxlib.h"

#define LUA_IR_FRAME_METATABLE "ir.frame"

static void lua_ir_frame_validate(lua_State *L, const ir_frame_t *frame)
{
    if (frame->carrier_hz && (frame->carrier_hz < LUA_IR_FRAME_MIN_CARRIER_HZ || frame->carrier_hz > LUA_IR_FRAME_MAX_CARRIER_HZ)) {
        luaL_error(L, "ir.frame: carrier_hz must be 0 or %d-%d", LUA_IR_FRAME_MIN_CARRIER_HZ, LUA_IR_FRAME_MAX_CARRIER_HZ);
    }
    if (frame->count == 0 || frame->count > frame->capacity || frame->count > LUA_IR_FRAME_MAX_PULSES) {
        luaL_error(L, "ir.frame: pulse count must be 1-%d", LUA_IR_FRAME_MAX_PULSES);
    }
    for (size_t i = 0; i < frame->count; i++) {
        if (frame->pulses_us[i] == 0 || frame->pulses_us[i] > LUA_IR_FRAME_MAX_PULSE_US) {
            luaL_error(L, "ir.frame: pulse[%d] must be 1-%d us", (int)i + 1, LUA_IR_FRAME_MAX_PULSE_US);
        }
    }
}

static void lua_ir_frame_create_metatable(lua_State *L);

ir_frame_t *lua_ir_frame_create(lua_State *L, size_t capacity)
{
    size_t allocation_size = ir_frame_size(capacity);
    if (!allocation_size) luaL_error(L, "ir.frame: capacity must be 1-%d", LUA_IR_FRAME_MAX_PULSES);
    lua_ir_frame_create_metatable(L);
    ir_frame_t *frame = lua_newuserdata(L, allocation_size);
    if (ir_frame_init(frame, capacity) != ESP_OK) luaL_error(L, "ir.frame: initialization failed");
    luaL_getmetatable(L, LUA_IR_FRAME_METATABLE);
    lua_setmetatable(L, -2);
    return frame;
}

ir_frame_t *lua_ir_frame_push(lua_State *L, const ir_frame_t *source)
{
    ir_frame_t *frame = lua_ir_frame_create(L, source->count);
    frame->carrier_hz = source->carrier_hz;
    frame->count = source->count;
    memcpy(frame->pulses_us, source->pulses_us, source->count * sizeof(uint32_t));
    return frame;
}

ir_frame_t *lua_ir_frame_check(lua_State *L, int index)
{
    ir_frame_t *frame = luaL_checkudata(L, index, LUA_IR_FRAME_METATABLE);
    size_t allocation_size = lua_rawlen(L, index);
    if (allocation_size < sizeof(*frame) || ir_frame_size(frame->capacity) != allocation_size) luaL_error(L, "ir.frame: invalid storage");
    lua_ir_frame_validate(L, frame);
    return frame;
}

static void lua_ir_frame_check_table(lua_State *L, int index)
{
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "ir.frame: field names must be strings");
        size_t length = 0;
        const char *key = lua_tolstring(L, -2, &length);
        if (strlen(key) != length) luaL_error(L, "ir.frame: field names must not contain NUL");
        if (strcmp(key, "carrier_hz") && strcmp(key, "pulses_us")) {
            luaL_error(L, "ir.frame: unknown field '%s'", key);
        }
        lua_pop(L, 1);
    }
}

ir_frame_t *lua_ir_frame_from_table(lua_State *L, int index)
{
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    lua_ir_frame_check_table(L, index);
    lua_pushliteral(L, "pulses_us");
    lua_rawget(L, index);
    luaL_checktype(L, -1, LUA_TTABLE);
    int pulses_index = lua_absindex(L, -1);
    size_t pulse_count = lua_rawlen(L, pulses_index);
    if (pulse_count == 0 || pulse_count > LUA_IR_FRAME_MAX_PULSES) {
        luaL_error(L, "ir.frame: pulses_us count must be 1-%d", LUA_IR_FRAME_MAX_PULSES);
        return NULL;
    }
    size_t entries = 0;
    lua_pushnil(L);
    while (lua_next(L, pulses_index) != 0) {
        if (!lua_isinteger(L, -2)) luaL_error(L, "ir.frame: pulses_us keys must be integers");
        lua_Integer key = lua_tointeger(L, -2);
        if (key < 1 || (uint64_t)key > pulse_count) luaL_error(L, "ir.frame: pulses_us must be a dense array");
        entries++;
        lua_pop(L, 1);
    }
    if (entries != pulse_count) luaL_error(L, "ir.frame: pulses_us must be a dense array");

    ir_frame_t *frame = lua_ir_frame_create(L, pulse_count);
    lua_getfield(L, index, "carrier_hz");
    lua_Integer carrier_hz = luaL_optinteger(L, -1, 0);
    lua_pop(L, 1);
    if (carrier_hz && (carrier_hz < LUA_IR_FRAME_MIN_CARRIER_HZ || carrier_hz > LUA_IR_FRAME_MAX_CARRIER_HZ)) {
        luaL_error(L, "ir.frame: carrier_hz must be 0 or %d-%d", LUA_IR_FRAME_MIN_CARRIER_HZ, LUA_IR_FRAME_MAX_CARRIER_HZ);
        return NULL;
    }

    frame->carrier_hz = (uint32_t)carrier_hz;
    frame->count = pulse_count;
    for (size_t i = 0; i < pulse_count; i++) {
        lua_rawgeti(L, pulses_index, (lua_Integer)i + 1);
        lua_Integer pulse = luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        if (pulse <= 0 || pulse > LUA_IR_FRAME_MAX_PULSE_US) {
            luaL_error(L, "ir.frame: pulse[%d] must be 1-%d us", (int)i + 1, LUA_IR_FRAME_MAX_PULSE_US);
            return NULL;
        }
        frame->pulses_us[i] = (uint32_t)pulse;
    }
    lua_remove(L, pulses_index);
    return frame;
}

static int lua_ir_frame_new(lua_State *L)
{
    lua_ir_frame_from_table(L, 1);
    return 1;
}

static int lua_ir_frame_to_table(lua_State *L)
{
    const ir_frame_t *frame = lua_ir_frame_check(L, 1);
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, (lua_Integer)frame->carrier_hz);
    lua_setfield(L, -2, "carrier_hz");
    lua_createtable(L, (int)frame->count, 0);
    for (size_t i = 0; i < frame->count; i++) {
        lua_pushinteger(L, (lua_Integer)frame->pulses_us[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "pulses_us");
    return 1;
}

static int lua_ir_frame_info(lua_State *L)
{
    const ir_frame_t *frame = lua_ir_frame_check(L, 1);
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, (lua_Integer)frame->carrier_hz);
    lua_setfield(L, -2, "carrier_hz");
    lua_pushinteger(L, (lua_Integer)frame->count);
    lua_setfield(L, -2, "pulse_count");
    lua_pushinteger(L, (lua_Integer)ir_frame_duration_us(frame));
    lua_setfield(L, -2, "duration_us");
    return 1;
}

static int lua_ir_frame_len(lua_State *L)
{
    const ir_frame_t *frame = lua_ir_frame_check(L, 1);
    lua_pushinteger(L, (lua_Integer)frame->count);
    return 1;
}

static void lua_ir_frame_create_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_IR_FRAME_METATABLE)) {
        lua_pushcfunction(L, lua_ir_frame_len);
        lua_setfield(L, -2, "__len");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_ir_frame_to_table);
        lua_setfield(L, -2, "to_table");
        lua_pushcfunction(L, lua_ir_frame_info);
        lua_setfield(L, -2, "info");
    }
    lua_pop(L, 1);
}

void lua_ir_frame_register(lua_State *L)
{
    luaL_checktype(L, -1, LUA_TTABLE);
    lua_ir_frame_create_metatable(L);
    lua_pushcfunction(L, lua_ir_frame_new);
    lua_setfield(L, -2, "frame");
}
