/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

int lua_lvgl_get_opt_int_field(lua_State *L, int index, const char *field, int default_value)
{
    int value = default_value;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        if (!lua_isinteger(L, -1)) {
            luaL_error(L, "lvgl option '%s' must be an integer", field);
        }
        value = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

const char *lua_lvgl_get_opt_string_field(lua_State *L, int index, const char *field)
{
    const char *value = NULL;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        value = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

bool lua_lvgl_get_opt_bool_field(lua_State *L, int index, const char *field, bool default_value)
{
    bool value = default_value;

    lua_getfield(L, index, field);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

bool lua_lvgl_has_field(lua_State *L, int index, const char *field)
{
    bool has_field;

    lua_getfield(L, index, field);
    has_field = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return has_field;
}

bool lua_lvgl_opt_table(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) {
        return false;
    }
    luaL_checktype(L, index, LUA_TTABLE);
    return true;
}

esp_err_t lua_lvgl_parse_color(lua_State *L, int index, lv_color_t *out_color)
{
    uint32_t value;

    if (lua_isinteger(L, index)) {
        value = (uint32_t)lua_tointeger(L, index);
    } else if (lua_isstring(L, index)) {
        const char *text = lua_tostring(L, index);
        char *end = NULL;

        if (!text || text[0] != '#' || strlen(text) != 7) {
            return ESP_ERR_INVALID_ARG;
        }
        value = (uint32_t)strtoul(text + 1, &end, 16);
        if (!end || *end != '\0') {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    if (value > 0xFFFFFFU) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_color = lv_color_hex(value);
    (void)L;
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_align(lua_State *L, const char *value, lv_align_t *out_align)
{
    if (!value || strcmp(value, "top_left") == 0 || strcmp(value, "default") == 0) {
        *out_align = LV_ALIGN_TOP_LEFT;
        return ESP_OK;
    }
    if (strcmp(value, "top_mid") == 0 || strcmp(value, "top") == 0) {
        *out_align = LV_ALIGN_TOP_MID;
        return ESP_OK;
    }
    if (strcmp(value, "top_right") == 0) {
        *out_align = LV_ALIGN_TOP_RIGHT;
        return ESP_OK;
    }
    if (strcmp(value, "bottom_left") == 0) {
        *out_align = LV_ALIGN_BOTTOM_LEFT;
        return ESP_OK;
    }
    if (strcmp(value, "bottom_mid") == 0 || strcmp(value, "bottom") == 0) {
        *out_align = LV_ALIGN_BOTTOM_MID;
        return ESP_OK;
    }
    if (strcmp(value, "bottom_right") == 0) {
        *out_align = LV_ALIGN_BOTTOM_RIGHT;
        return ESP_OK;
    }
    if (strcmp(value, "left_mid") == 0 || strcmp(value, "left") == 0) {
        *out_align = LV_ALIGN_LEFT_MID;
        return ESP_OK;
    }
    if (strcmp(value, "right_mid") == 0 || strcmp(value, "right") == 0) {
        *out_align = LV_ALIGN_RIGHT_MID;
        return ESP_OK;
    }
    if (strcmp(value, "center") == 0 || strcmp(value, "centre") == 0) {
        *out_align = LV_ALIGN_CENTER;
        return ESP_OK;
    }
    (void)L;
    return ESP_ERR_INVALID_ARG;
}

esp_err_t lua_lvgl_parse_flex_flow(const char *value, lv_flex_flow_t *out_flow)
{
    if (!value || strcmp(value, "row") == 0) {
        *out_flow = LV_FLEX_FLOW_ROW;
    } else if (strcmp(value, "column") == 0) {
        *out_flow = LV_FLEX_FLOW_COLUMN;
    } else if (strcmp(value, "row_wrap") == 0) {
        *out_flow = LV_FLEX_FLOW_ROW_WRAP;
    } else if (strcmp(value, "row_reverse") == 0) {
        *out_flow = LV_FLEX_FLOW_ROW_REVERSE;
    } else if (strcmp(value, "row_wrap_reverse") == 0) {
        *out_flow = LV_FLEX_FLOW_ROW_WRAP_REVERSE;
    } else if (strcmp(value, "column_wrap") == 0) {
        *out_flow = LV_FLEX_FLOW_COLUMN_WRAP;
    } else if (strcmp(value, "column_reverse") == 0) {
        *out_flow = LV_FLEX_FLOW_COLUMN_REVERSE;
    } else if (strcmp(value, "column_wrap_reverse") == 0) {
        *out_flow = LV_FLEX_FLOW_COLUMN_WRAP_REVERSE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_flex_align(const char *value, lv_flex_align_t *out_align)
{
    if (!value || strcmp(value, "start") == 0) {
        *out_align = LV_FLEX_ALIGN_START;
    } else if (strcmp(value, "end") == 0) {
        *out_align = LV_FLEX_ALIGN_END;
    } else if (strcmp(value, "center") == 0) {
        *out_align = LV_FLEX_ALIGN_CENTER;
    } else if (strcmp(value, "space_evenly") == 0) {
        *out_align = LV_FLEX_ALIGN_SPACE_EVENLY;
    } else if (strcmp(value, "space_around") == 0) {
        *out_align = LV_FLEX_ALIGN_SPACE_AROUND;
    } else if (strcmp(value, "space_between") == 0) {
        *out_align = LV_FLEX_ALIGN_SPACE_BETWEEN;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_grid_align(const char *value, lv_grid_align_t *out_align)
{
    if (!value || strcmp(value, "start") == 0) {
        *out_align = LV_GRID_ALIGN_START;
    } else if (strcmp(value, "center") == 0) {
        *out_align = LV_GRID_ALIGN_CENTER;
    } else if (strcmp(value, "end") == 0) {
        *out_align = LV_GRID_ALIGN_END;
    } else if (strcmp(value, "stretch") == 0) {
        *out_align = LV_GRID_ALIGN_STRETCH;
    } else if (strcmp(value, "space_evenly") == 0) {
        *out_align = LV_GRID_ALIGN_SPACE_EVENLY;
    } else if (strcmp(value, "space_around") == 0) {
        *out_align = LV_GRID_ALIGN_SPACE_AROUND;
    } else if (strcmp(value, "space_between") == 0) {
        *out_align = LV_GRID_ALIGN_SPACE_BETWEEN;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_dir(const char *value, lv_dir_t *out_dir)
{
    if (!value || strcmp(value, "none") == 0) {
        *out_dir = LV_DIR_NONE;
    } else if (strcmp(value, "left") == 0) {
        *out_dir = LV_DIR_LEFT;
    } else if (strcmp(value, "right") == 0) {
        *out_dir = LV_DIR_RIGHT;
    } else if (strcmp(value, "top") == 0) {
        *out_dir = LV_DIR_TOP;
    } else if (strcmp(value, "bottom") == 0) {
        *out_dir = LV_DIR_BOTTOM;
    } else if (strcmp(value, "hor") == 0) {
        *out_dir = LV_DIR_HOR;
    } else if (strcmp(value, "ver") == 0) {
        *out_dir = LV_DIR_VER;
    } else if (strcmp(value, "all") == 0) {
        *out_dir = LV_DIR_ALL;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_scrollbar(const char *value, lv_scrollbar_mode_t *out_mode)
{
    if (!value || strcmp(value, "auto") == 0) {
        *out_mode = LV_SCROLLBAR_MODE_AUTO;
    } else if (strcmp(value, "off") == 0) {
        *out_mode = LV_SCROLLBAR_MODE_OFF;
    } else if (strcmp(value, "on") == 0) {
        *out_mode = LV_SCROLLBAR_MODE_ON;
    } else if (strcmp(value, "active") == 0) {
        *out_mode = LV_SCROLLBAR_MODE_ACTIVE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_scroll_snap(const char *value, lv_scroll_snap_t *out_snap)
{
    if (!value || strcmp(value, "none") == 0) {
        *out_snap = LV_SCROLL_SNAP_NONE;
    } else if (strcmp(value, "start") == 0) {
        *out_snap = LV_SCROLL_SNAP_START;
    } else if (strcmp(value, "end") == 0) {
        *out_snap = LV_SCROLL_SNAP_END;
    } else if (strcmp(value, "center") == 0) {
        *out_snap = LV_SCROLL_SNAP_CENTER;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_arc_mode(const char *value, lv_arc_mode_t *out_mode)
{
    if (!value || strcmp(value, "normal") == 0) {
        *out_mode = LV_ARC_MODE_NORMAL;
    } else if (strcmp(value, "symmetrical") == 0 || strcmp(value, "symmetric") == 0) {
        *out_mode = LV_ARC_MODE_SYMMETRICAL;
    } else if (strcmp(value, "reverse") == 0) {
        *out_mode = LV_ARC_MODE_REVERSE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_scale_mode(const char *value, lv_scale_mode_t *out_mode)
{
    if (!value || strcmp(value, "horizontal_bottom") == 0) {
        *out_mode = LV_SCALE_MODE_HORIZONTAL_BOTTOM;
    } else if (strcmp(value, "horizontal_top") == 0) {
        *out_mode = LV_SCALE_MODE_HORIZONTAL_TOP;
    } else if (strcmp(value, "vertical_left") == 0) {
        *out_mode = LV_SCALE_MODE_VERTICAL_LEFT;
    } else if (strcmp(value, "vertical_right") == 0) {
        *out_mode = LV_SCALE_MODE_VERTICAL_RIGHT;
    } else if (strcmp(value, "round_inner") == 0) {
        *out_mode = LV_SCALE_MODE_ROUND_INNER;
    } else if (strcmp(value, "round_outer") == 0) {
        *out_mode = LV_SCALE_MODE_ROUND_OUTER;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_keyboard_mode(const char *value, lv_keyboard_mode_t *out_mode)
{
    if (!value || strcmp(value, "text_lower") == 0 || strcmp(value, "lower") == 0) {
        *out_mode = LV_KEYBOARD_MODE_TEXT_LOWER;
    } else if (strcmp(value, "text_upper") == 0 || strcmp(value, "upper") == 0) {
        *out_mode = LV_KEYBOARD_MODE_TEXT_UPPER;
    } else if (strcmp(value, "special") == 0) {
        *out_mode = LV_KEYBOARD_MODE_SPECIAL;
    } else if (strcmp(value, "number") == 0) {
        *out_mode = LV_KEYBOARD_MODE_NUMBER;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t lua_lvgl_parse_roller_mode(const char *value, lv_roller_mode_t *out_mode)
{
    if (!value || strcmp(value, "normal") == 0) {
        *out_mode = LV_ROLLER_MODE_NORMAL;
    } else if (strcmp(value, "infinite") == 0) {
        *out_mode = LV_ROLLER_MODE_INFINITE;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

void lua_lvgl_parse_opts(lua_State *L, int index, lua_lvgl_opts_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->w = -1;
    opts->h = -1;
    opts->max_value = 100;

    if (!lua_lvgl_opt_table(L, index)) {
        return;
    }

    opts->has_opts = true;
    opts->x = lua_lvgl_get_opt_int_field(L, index, "x", 0);
    opts->y = lua_lvgl_get_opt_int_field(L, index, "y", 0);
    opts->w = lua_lvgl_get_opt_int_field(L, index, "w", -1);
    opts->h = lua_lvgl_get_opt_int_field(L, index, "h", -1);
    opts->align_value = lua_lvgl_get_opt_string_field(L, index, "align");
    opts->text = lua_lvgl_get_opt_string_field(L, index, "text");
    opts->min_value = lua_lvgl_get_opt_int_field(L, index, "min", 0);
    opts->max_value = lua_lvgl_get_opt_int_field(L, index, "max", 100);
    opts->value = lua_lvgl_get_opt_int_field(L, index, "value", opts->min_value);
}

void lua_lvgl_apply_common_opts_locked(lv_obj_t *obj, const lua_lvgl_opts_t *opts)
{
    lv_align_t align;

    if (!opts->has_opts) {
        return;
    }

    if (opts->w > 0 && opts->h > 0) {
        lv_obj_set_size(obj, opts->w, opts->h);
    }
    if (opts->align_value && lua_lvgl_parse_align(NULL, opts->align_value, &align) == ESP_OK) {
        lv_obj_align(obj, align, opts->x, opts->y);
    } else {
        lv_obj_set_pos(obj, opts->x, opts->y);
    }
}
char *lua_lvgl_build_options_string(lua_State *L, int index, const char *field)
{
    char *result = NULL;

    lua_getfield(L, index, field);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return NULL;
    }
    if (lua_isstring(L, -1)) {
        const char *text = lua_tostring(L, -1);
        size_t len = strlen(text);

        result = (char *)malloc(len + 1);
        if (!result) {
            lua_pop(L, 1);
            luaL_error(L, "lvgl options allocation failed");
        }
        memcpy(result, text, len + 1);
        lua_pop(L, 1);
        return result;
    }
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "lvgl option '%s' must be a string or table", field);
    }

    size_t count = lua_rawlen(L, -1);
    size_t total = 1;
    for (size_t i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, (lua_Integer)i);
        total += strlen(luaL_checkstring(L, -1)) + (i > 1 ? 1 : 0);
        lua_pop(L, 1);
    }

    result = (char *)malloc(total);
    if (!result) {
        lua_pop(L, 1);
        luaL_error(L, "lvgl options allocation failed");
    }
    result[0] = '\0';
    for (size_t i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, (lua_Integer)i);
        if (i > 1) {
            strcat(result, "\n");
        }
        strcat(result, luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return result;
}

static int32_t lua_lvgl_parse_grid_track(lua_State *L, int index)
{
    if (lua_isinteger(L, index)) {
        return (int32_t)lua_tointeger(L, index);
    }
    if (lua_isstring(L, index)) {
        const char *value = lua_tostring(L, index);

        if (strcmp(value, "content") == 0) {
            return LV_GRID_CONTENT;
        }
        if (strcmp(value, "fr") == 0) {
            return LV_GRID_FR(1);
        }
    }
    luaL_error(L, "lvgl grid track must be an integer, 'fr', or 'content'");
    return 0;
}

int32_t *lua_lvgl_build_grid_tracks(lua_State *L, int opts_index, const char *field)
{
    int32_t *tracks;
    size_t count;

    lua_getfield(L, opts_index, field);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "lvgl grid option '%s' must be a table", field);
    }

    count = lua_rawlen(L, -1);
    if (count == 0) {
        lua_pop(L, 1);
        luaL_error(L, "lvgl grid option '%s' must not be empty", field);
    }
    tracks = (int32_t *)calloc(count + 1, sizeof(*tracks));
    if (!tracks) {
        lua_pop(L, 1);
        luaL_error(L, "lvgl grid descriptor allocation failed");
    }
    for (size_t i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, (lua_Integer)i);
        tracks[i - 1] = lua_lvgl_parse_grid_track(L, -1);
        lua_pop(L, 1);
    }
    tracks[count] = LV_GRID_TEMPLATE_LAST;
    lua_pop(L, 1);
    return tracks;
}

lv_point_precise_t *lua_lvgl_build_line_points(lua_State *L, int opts_index, uint32_t *out_count)
{
    lv_point_precise_t *points;
    size_t count;

    lua_getfield(L, opts_index, "points");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "lvgl line option 'points' must be a table");
    }

    count = lua_rawlen(L, -1);
    if (count < 2) {
        lua_pop(L, 1);
        luaL_error(L, "lvgl line option 'points' requires at least two points");
    }
    points = (lv_point_precise_t *)calloc(count, sizeof(*points));
    if (!points) {
        lua_pop(L, 1);
        luaL_error(L, "lvgl line points allocation failed");
    }

    for (size_t i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, (lua_Integer)i);
        if (!lua_istable(L, -1)) {
            free(points);
            lua_pop(L, 2);
            luaL_error(L, "lvgl line point must be a table");
        }
        points[i - 1].x = lua_lvgl_get_opt_int_field(L, -1, "x", 0);
        points[i - 1].y = lua_lvgl_get_opt_int_field(L, -1, "y", 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    *out_count = (uint32_t)count;
    return points;
}
