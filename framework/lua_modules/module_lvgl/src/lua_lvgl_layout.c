/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

int lua_lvgl_set_flex(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lv_flex_flow_t flow;
    lv_flex_align_t main_align;
    lv_flex_align_t cross_align;
    lv_flex_align_t track_align;
    const char *flow_text;
    const char *main_text;
    const char *cross_text;
    const char *track_text;
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_checktype(L, 2, LUA_TTABLE);
    flow_text = lua_lvgl_get_opt_string_field(L, 2, "flow");
    main_text = lua_lvgl_get_opt_string_field(L, 2, "main");
    cross_text = lua_lvgl_get_opt_string_field(L, 2, "cross");
    track_text = lua_lvgl_get_opt_string_field(L, 2, "track");
    if (lua_lvgl_parse_flex_flow(flow_text, &flow) != ESP_OK ||
            lua_lvgl_parse_flex_align(main_text, &main_align) != ESP_OK ||
            lua_lvgl_parse_flex_align(cross_text, &cross_align) != ESP_OK ||
            lua_lvgl_parse_flex_align(track_text, &track_align) != ESP_OK) {
        return luaL_error(L, "lvgl flex option is invalid");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_set_flex_flow(obj, flow);
    lv_obj_set_flex_align(obj, main_align, cross_align, track_align);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_set_grid(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int32_t *cols;
    int32_t *rows;
    lv_grid_align_t col_align;
    lv_grid_align_t row_align;
    const char *col_align_text;
    const char *row_align_text;
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_checktype(L, 2, LUA_TTABLE);
    cols = lua_lvgl_build_grid_tracks(L, 2, "cols");
    rows = lua_lvgl_build_grid_tracks(L, 2, "rows");
    col_align_text = lua_lvgl_get_opt_string_field(L, 2, "col_align");
    row_align_text = lua_lvgl_get_opt_string_field(L, 2, "row_align");
    if (lua_lvgl_parse_grid_align(col_align_text, &col_align) != ESP_OK ||
            lua_lvgl_parse_grid_align(row_align_text, &row_align) != ESP_OK) {
        free(cols);
        free(rows);
        return luaL_error(L, "lvgl grid align option is invalid");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        free(cols);
        free(rows);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        free(cols);
        free(rows);
        return luaL_error(L, "%s", obj_error);
    }
    free(ud->record->grid_cols);
    free(ud->record->grid_rows);
    ud->record->grid_cols = cols;
    ud->record->grid_rows = rows;
    lv_obj_set_grid_dsc_array(obj, cols, rows);
    lv_obj_set_grid_align(obj, col_align, row_align);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_set_grid_cell(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    int col;
    int row;
    int col_span;
    int row_span;
    lv_grid_align_t col_align;
    lv_grid_align_t row_align;
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_checktype(L, 2, LUA_TTABLE);
    col = lua_lvgl_get_opt_int_field(L, 2, "col", 1);
    row = lua_lvgl_get_opt_int_field(L, 2, "row", 1);
    col_span = lua_lvgl_get_opt_int_field(L, 2, "col_span", 1);
    row_span = lua_lvgl_get_opt_int_field(L, 2, "row_span", 1);
    if (col < 1 || row < 1 || col_span < 1 || row_span < 1 ||
            lua_lvgl_parse_grid_align(lua_lvgl_get_opt_string_field(L, 2, "col_align"), &col_align) != ESP_OK ||
            lua_lvgl_parse_grid_align(lua_lvgl_get_opt_string_field(L, 2, "row_align"), &row_align) != ESP_OK) {
        return luaL_error(L, "lvgl grid cell option is invalid");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_set_grid_cell(obj, col_align, col - 1, col_span, row_align, row - 1, row_span);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_set_scroll(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lv_dir_t dir;
    lv_scrollbar_mode_t scrollbar;
    lv_scroll_snap_t snap_x;
    lv_scroll_snap_t snap_y;
    esp_err_t err;
    lv_obj_t *obj;
    const char *obj_error = NULL;

    luaL_checktype(L, 2, LUA_TTABLE);
    if (lua_lvgl_parse_dir(lua_lvgl_get_opt_string_field(L, 2, "dir"), &dir) != ESP_OK ||
            lua_lvgl_parse_scrollbar(lua_lvgl_get_opt_string_field(L, 2, "scrollbar"), &scrollbar) != ESP_OK ||
            lua_lvgl_parse_scroll_snap(lua_lvgl_get_opt_string_field(L, 2, "snap_x"), &snap_x) != ESP_OK ||
            lua_lvgl_parse_scroll_snap(lua_lvgl_get_opt_string_field(L, 2, "snap_y"), &snap_y) != ESP_OK) {
        return luaL_error(L, "lvgl scroll option is invalid");
    }

    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_validate_ud_locked(ud, NULL, &obj_error);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    lv_obj_set_scroll_dir(obj, dir);
    lv_obj_set_scrollbar_mode(obj, scrollbar);
    lv_obj_set_scroll_snap_x(obj, snap_x);
    lv_obj_set_scroll_snap_y(obj, snap_y);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

/* Layout helpers are exposed via the base method table in
 * lua_lvgl_methods.c. */
