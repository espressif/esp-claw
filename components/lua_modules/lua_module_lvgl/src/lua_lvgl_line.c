/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

#define LINE_STACK_POINTS 16
#define LINE_MAX_POINTS 1024

static lv_point_precise_t *read_points(lua_State *L, int index, lv_point_precise_t *small, uint32_t *count)
{
    index = lua_absindex(L, index);
    luaL_checktype(L, index, LUA_TTABLE);
    size_t n = lua_rawlen(L, index);
    luaL_argcheck(L, n >= 2 && n <= LINE_MAX_POINTS, index, "line requires 2 to 1024 points");
    /* Larger temporary arrays belong to Lua, so argument errors cannot leak them. */
    lv_point_precise_t *points = n <= LINE_STACK_POINTS ? small : lua_newuserdatauv(L, n * sizeof(*points), 0);
    for (size_t i = 0; i < n; i++) {
        lua_rawgeti(L, index, (lua_Integer)i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_pushliteral(L, "x");
        lua_rawget(L, -2);
        lua_Integer x = luaL_optinteger(L, -1, 0);
        lua_pop(L, 1);
        lua_pushliteral(L, "y");
        lua_rawget(L, -2);
        lua_Integer y = luaL_optinteger(L, -1, 0);
        lua_pop(L, 2);
        luaL_argcheck(L, x >= INT16_MIN && x <= INT16_MAX && y >= INT16_MIN && y <= INT16_MAX, index, "line coordinate out of range");
        points[i].x = (int32_t)x;
        points[i].y = (int32_t)y;
    }
    *count = (uint32_t)n;
    return points;
}

lv_point_precise_t *lua_lvgl_build_line_points(lua_State *L, int opts_index, uint32_t *out_count)
{
    int top = lua_gettop(L);
    lv_point_precise_t small[LINE_STACK_POINTS];
    lua_getfield(L, opts_index, "points");
    lv_point_precise_t *source = read_points(L, -1, small, out_count);
    size_t size = *out_count * sizeof(*source);
    lv_point_precise_t *points = malloc(size);
    if (!points) {
        ESP_LOGE("lua_lvgl", "Line allocation failed (%zu bytes)", size);
        luaL_error(L, "lvgl line points allocation failed");
    }
    memcpy(points, source, size);
    lua_settop(L, top);
    return points;
}

int lua_lvgl_line_set_points(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lv_point_precise_t small[LINE_STACK_POINTS];
    uint32_t count;
    lv_point_precise_t *source = read_points(L, 2, small, &count);
    lv_obj_t *obj = lua_lvgl_lock_widget(L, ud, LUA_LVGL_OBJ_LINE);
    lua_lvgl_obj_record_t *record = ud->record;
    lv_point_precise_t *points = record->line_points;
    size_t size = count * sizeof(*points);
    if (count != record->line_point_count) points = malloc(size);
    if (!points) {
        lua_lvgl_unlock();
        ESP_LOGE("lua_lvgl", "Line allocation failed (%zu bytes)", size);
        return luaL_error(L, "lvgl line points allocation failed");
    }
    /* Invalidate the old extent before changing the borrowed point array. */
    lv_obj_invalidate(obj);
    memcpy(points, source, size);
    lv_line_set_points(obj, points, count);
    if (points != record->line_points) free(record->line_points);
    record->line_points = points;
    record->line_point_count = count;
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}
