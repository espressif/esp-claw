/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

#define LUA_LVGL_CHART_SERIES_MT "lvgl.chart.series"
#define LUA_LVGL_SPAN_MT "lvgl.span"

typedef struct {
    lua_lvgl_obj_record_t *chart_record;
    lv_chart_series_t *series;
    uint32_t generation;
} lua_lvgl_chart_series_ud_t;

typedef struct {
    lua_lvgl_obj_record_t *group_record;
    lv_span_t *span;
    uint32_t generation;
    bool valid;
} lua_lvgl_span_ud_t;

static char *lua_lvgl_strdup_lua(lua_State *L, const char *text)
{
    size_t len = strlen(text);
    char *copy = (char *)malloc(len + 1);

    if (!copy) {
        luaL_error(L, "lvgl string allocation failed");
    }
    memcpy(copy, text, len + 1);
    return copy;
}

static lv_obj_t *lua_lvgl_check_typed_obj(lua_State *L, int index, lua_lvgl_obj_type_t want, const char *what)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, index);
    lua_lvgl_obj_type_t type;
    const char *obj_error = NULL;
    lv_obj_t *obj = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);

    if (!obj) {
        luaL_error(L, "%s", obj_error);
    }
    if (type != want) {
        luaL_error(L, "lvgl %s requires a %s object", what, what);
    }
    return obj;
}

static void lua_lvgl_release_string_array(lua_lvgl_obj_record_t *record)
{
    if (!record || !record->string_array) {
        return;
    }
    for (size_t i = 0; i < record->string_array_count; i++) {
        free(record->string_array[i]);
    }
    free(record->string_array);
    record->string_array = NULL;
    record->string_array_count = 0;
}

static char **lua_lvgl_build_string_array(lua_State *L, int index, size_t *out_count, bool terminal_empty)
{
    size_t count;
    char **array;

    luaL_checktype(L, index, LUA_TTABLE);
    count = lua_rawlen(L, index);
    array = (char **)calloc(count + (terminal_empty ? 1 : 0), sizeof(char *));
    if (!array) {
        luaL_error(L, "lvgl string array allocation failed");
    }
    for (size_t i = 1; i <= count; i++) {
        lua_rawgeti(L, index, (lua_Integer)i);
        array[i - 1] = lua_lvgl_strdup_lua(L, luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    if (terminal_empty) {
        array[count] = lua_lvgl_strdup_lua(L, "");
    }
    *out_count = count + (terminal_empty ? 1 : 0);
    return array;
}

static lv_calendar_date_t lua_lvgl_date_from_table(lua_State *L, int index)
{
    lv_calendar_date_t d = {0};

    luaL_checktype(L, index, LUA_TTABLE);
    lua_rawgeti(L, index, 1);
    d.year = (uint16_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    lua_rawgeti(L, index, 2);
    d.month = (uint8_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    lua_rawgeti(L, index, 3);
    d.day = (uint8_t)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    luaL_argcheck(L, d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= 31, index, "invalid date");
    return d;
}

static lv_calendar_date_t *lua_lvgl_build_dates(lua_State *L, int index, size_t *out_count)
{
    size_t count;
    lv_calendar_date_t *dates;

    luaL_checktype(L, index, LUA_TTABLE);
    count = lua_rawlen(L, index);
    dates = (lv_calendar_date_t *)calloc(count ? count : 1, sizeof(*dates));
    if (!dates) {
        luaL_error(L, "lvgl date allocation failed");
    }
    for (size_t i = 1; i <= count; i++) {
        lua_rawgeti(L, index, (lua_Integer)i);
        dates[i - 1] = lua_lvgl_date_from_table(L, -1);
        lua_pop(L, 1);
    }
    *out_count = count;
    return dates;
}

static esp_err_t lua_lvgl_parse_chart_type(const char *value, lv_chart_type_t *out)
{
    if (!value || strcmp(value, "line") == 0) *out = LV_CHART_TYPE_LINE;
    else if (strcmp(value, "none") == 0) *out = LV_CHART_TYPE_NONE;
    else if (strcmp(value, "curve") == 0) *out = LV_CHART_TYPE_CURVE;
    else if (strcmp(value, "bar") == 0) *out = LV_CHART_TYPE_BAR;
    else if (strcmp(value, "stacked") == 0) *out = LV_CHART_TYPE_STACKED;
    else if (strcmp(value, "scatter") == 0) *out = LV_CHART_TYPE_SCATTER;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t lua_lvgl_parse_chart_update_mode(const char *value, lv_chart_update_mode_t *out)
{
    if (!value || strcmp(value, "shift") == 0) *out = LV_CHART_UPDATE_MODE_SHIFT;
    else if (strcmp(value, "circular") == 0) *out = LV_CHART_UPDATE_MODE_CIRCULAR;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t lua_lvgl_parse_chart_axis(const char *value, lv_chart_axis_t *out)
{
    if (!value || strcmp(value, "primary_y") == 0 || strcmp(value, "y") == 0) *out = LV_CHART_AXIS_PRIMARY_Y;
    else if (strcmp(value, "secondary_y") == 0) *out = LV_CHART_AXIS_SECONDARY_Y;
    else if (strcmp(value, "primary_x") == 0 || strcmp(value, "x") == 0) *out = LV_CHART_AXIS_PRIMARY_X;
    else if (strcmp(value, "secondary_x") == 0) *out = LV_CHART_AXIS_SECONDARY_X;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t lua_lvgl_parse_imagebutton_state(const char *value, lv_imagebutton_state_t *out)
{
    if (!value || strcmp(value, "released") == 0) *out = LV_IMAGEBUTTON_STATE_RELEASED;
    else if (strcmp(value, "pressed") == 0) *out = LV_IMAGEBUTTON_STATE_PRESSED;
    else if (strcmp(value, "disabled") == 0) *out = LV_IMAGEBUTTON_STATE_DISABLED;
    else if (strcmp(value, "checked_released") == 0) *out = LV_IMAGEBUTTON_STATE_CHECKED_RELEASED;
    else if (strcmp(value, "checked_pressed") == 0) *out = LV_IMAGEBUTTON_STATE_CHECKED_PRESSED;
    else if (strcmp(value, "checked_disabled") == 0) *out = LV_IMAGEBUTTON_STATE_CHECKED_DISABLED;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t lua_lvgl_parse_canvas_cf(const char *value, lv_color_format_t *out)
{
    if (!value || strcmp(value, "rgb565") == 0) *out = LV_COLOR_FORMAT_RGB565;
    else if (strcmp(value, "rgb888") == 0) *out = LV_COLOR_FORMAT_RGB888;
    else if (strcmp(value, "xrgb8888") == 0) *out = LV_COLOR_FORMAT_XRGB8888;
    else if (strcmp(value, "argb8888") == 0) *out = LV_COLOR_FORMAT_ARGB8888;
    else if (strcmp(value, "native") == 0) *out = LV_COLOR_FORMAT_NATIVE;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t lua_lvgl_parse_span_mode(const char *value, lv_span_mode_t *out)
{
    if (!value || strcmp(value, "fixed") == 0) *out = LV_SPAN_MODE_FIXED;
    else if (strcmp(value, "expand") == 0) *out = LV_SPAN_MODE_EXPAND;
    else if (strcmp(value, "break") == 0) *out = LV_SPAN_MODE_BREAK;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t lua_lvgl_parse_span_overflow(const char *value, lv_span_overflow_t *out)
{
    if (!value || strcmp(value, "clip") == 0) *out = LV_SPAN_OVERFLOW_CLIP;
    else if (strcmp(value, "ellipsis") == 0) *out = LV_SPAN_OVERFLOW_ELLIPSIS;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t lua_lvgl_parse_menu_header(const char *value, lv_menu_mode_header_t *out)
{
    if (!value || strcmp(value, "top_fixed") == 0) *out = LV_MENU_HEADER_TOP_FIXED;
    else if (strcmp(value, "top_unfixed") == 0) *out = LV_MENU_HEADER_TOP_UNFIXED;
    else if (strcmp(value, "bottom_fixed") == 0) *out = LV_MENU_HEADER_BOTTOM_FIXED;
    else return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static lua_lvgl_chart_series_ud_t *lua_lvgl_check_series(lua_State *L, int index)
{
    lua_lvgl_chart_series_ud_t *ud = (lua_lvgl_chart_series_ud_t *)luaL_checkudata(L, index, LUA_LVGL_CHART_SERIES_MT);

    if (!ud->series || !ud->chart_record || ud->generation != s_lvgl.generation ||
            !ud->chart_record->valid || ud->chart_record->generation != s_lvgl.generation) {
        luaL_error(L, "lvgl chart series has been deleted");
    }
    return ud;
}

static lua_lvgl_span_ud_t *lua_lvgl_check_span(lua_State *L, int index)
{
    lua_lvgl_span_ud_t *ud = (lua_lvgl_span_ud_t *)luaL_checkudata(L, index, LUA_LVGL_SPAN_MT);

    if (!ud->valid || !ud->span || !ud->group_record || ud->generation != s_lvgl.generation ||
            !ud->group_record->valid || ud->group_record->generation != s_lvgl.generation) {
        luaL_error(L, "lvgl span has been deleted");
    }
    return ud;
}

static int lua_lvgl_push_series(lua_State *L, lua_lvgl_obj_record_t *chart_record, lv_chart_series_t *series)
{
    lua_lvgl_chart_series_ud_t *ud = (lua_lvgl_chart_series_ud_t *)lua_newuserdata(L, sizeof(*ud));

    ud->chart_record = chart_record;
    ud->series = series;
    ud->generation = s_lvgl.generation;
    luaL_getmetatable(L, LUA_LVGL_CHART_SERIES_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_lvgl_push_span(lua_State *L, lua_lvgl_obj_record_t *group_record, lv_span_t *span)
{
    lua_lvgl_span_ud_t *ud = (lua_lvgl_span_ud_t *)lua_newuserdata(L, sizeof(*ud));

    ud->group_record = group_record;
    ud->span = span;
    ud->generation = s_lvgl.generation;
    ud->valid = true;
    luaL_getmetatable(L, LUA_LVGL_SPAN_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_lvgl_factory(lua_State *L, lua_lvgl_obj_type_t type, const char *name)
{
#define REJECT_IF_DISABLED(macro) do { if (!(macro)) return luaL_error(L, "lvgl %s is not enabled in firmware", name); } while (0)
    switch (type) {
    case LUA_LVGL_OBJ_BUTTONMATRIX: REJECT_IF_DISABLED(LV_USE_BUTTONMATRIX); break;
    case LUA_LVGL_OBJ_CALENDAR: REJECT_IF_DISABLED(LV_USE_CALENDAR); break;
    case LUA_LVGL_OBJ_CANVAS: REJECT_IF_DISABLED(LV_USE_CANVAS); break;
    case LUA_LVGL_OBJ_CHART: REJECT_IF_DISABLED(LV_USE_CHART); break;
    case LUA_LVGL_OBJ_IMAGEBUTTON: REJECT_IF_DISABLED(LV_USE_IMAGEBUTTON); break;
    case LUA_LVGL_OBJ_LED: REJECT_IF_DISABLED(LV_USE_LED); break;
    case LUA_LVGL_OBJ_MENU: REJECT_IF_DISABLED(LV_USE_MENU); break;
    case LUA_LVGL_OBJ_SPANGROUP: REJECT_IF_DISABLED(LV_USE_SPAN); break;
    case LUA_LVGL_OBJ_SPINBOX: REJECT_IF_DISABLED(LV_USE_SPINBOX); break;
    case LUA_LVGL_OBJ_TABVIEW: REJECT_IF_DISABLED(LV_USE_TABVIEW); break;
    case LUA_LVGL_OBJ_TILEVIEW: REJECT_IF_DISABLED(LV_USE_TILEVIEW); break;
    case LUA_LVGL_OBJ_WINDOW: REJECT_IF_DISABLED(LV_USE_WIN); break;
    default: break;
    }
#undef REJECT_IF_DISABLED
    return lua_lvgl_create_widget(L, type);
}

static int lua_lvgl_buttonmatrix(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_BUTTONMATRIX, "buttonmatrix"); }
static int lua_lvgl_calendar(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_CALENDAR, "calendar"); }
static int lua_lvgl_canvas(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_CANVAS, "canvas"); }
static int lua_lvgl_chart(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_CHART, "chart"); }
static int lua_lvgl_imagebutton(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_IMAGEBUTTON, "imagebutton"); }
static int lua_lvgl_led(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_LED, "led"); }
static int lua_lvgl_menu(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_MENU, "menu"); }
static int lua_lvgl_spangroup(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_SPANGROUP, "spangroup"); }
static int lua_lvgl_spinbox(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_SPINBOX, "spinbox"); }
static int lua_lvgl_tabview(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_TABVIEW, "tabview"); }
static int lua_lvgl_tileview(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_TILEVIEW, "tileview"); }
static int lua_lvgl_window(lua_State *L) { return lua_lvgl_factory(L, LUA_LVGL_OBJ_WINDOW, "window"); }

static int lua_lvgl_msgbox(lua_State *L)
{
#if LV_USE_MSGBOX
    lua_lvgl_obj_ud_t *parent_ud = NULL;
    lua_lvgl_opts_t opts;
    esp_err_t err;
    lv_obj_t *parent = NULL;
    lv_obj_t *obj;
    lua_lvgl_obj_ud_t *created_ud;
    const char *obj_error = NULL;

    if (!lua_isnoneornil(L, 1)) {
        parent_ud = lua_lvgl_check_ud(L, 1);
    }
    lua_lvgl_parse_opts(L, 2, &opts);
    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (parent_ud) {
        parent = lua_lvgl_validate_ud_locked(parent_ud, NULL, &obj_error);
        if (!parent) {
            lua_lvgl_unlock();
            return luaL_error(L, "%s", obj_error);
        }
    }
    obj = lv_msgbox_create(parent);
    if (!obj) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl msgbox create failed");
    }
    if (opts.has_opts) {
        lua_lvgl_apply_common_opts_locked(obj, &opts);
        if (lua_lvgl_has_field(L, 2, "title")) {
            lv_msgbox_add_title(obj, lua_lvgl_get_opt_string_field(L, 2, "title"));
        }
        if (lua_lvgl_has_field(L, 2, "text")) {
            lv_msgbox_add_text(obj, lua_lvgl_get_opt_string_field(L, 2, "text"));
        }
        lua_getfield(L, 2, "buttons");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            for (size_t i = 1; i <= lua_rawlen(L, -1); i++) {
                lua_rawgeti(L, -1, (lua_Integer)i);
                lv_msgbox_add_footer_button(obj, luaL_checkstring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        if (lua_lvgl_get_opt_bool_field(L, 2, "close_button", false)) {
            lv_msgbox_add_close_button(obj);
        }
        lua_lvgl_apply_style_opts_locked(L, 2, obj);
    }
    created_ud = lua_lvgl_push_obj(L, obj, LUA_LVGL_OBJ_MSGBOX);
    if (!created_ud) {
        lv_obj_delete(obj);
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    lua_lvgl_unlock();
    return 1;
#else
    return luaL_error(L, "lvgl msgbox is not enabled in firmware");
#endif
}

static int lua_lvgl_get_color_arg(lua_State *L, int index, lv_color_t *color)
{
    if (lua_lvgl_parse_color(L, index, color) != ESP_OK) {
        return luaL_error(L, "lvgl color must be a 0xRRGGBB number or '#RRGGBB' string");
    }
    return 0;
}

int lua_lvgl_buttonmatrix_set_map(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    char **map;
    size_t count;
    esp_err_t err;
    lv_obj_t *obj;

    map = lua_lvgl_build_string_array(L, 2, &count, true);
    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        for (size_t i = 0; i < count; i++) free(map[i]);
        free(map);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_BUTTONMATRIX, "buttonmatrix");
    lua_lvgl_release_string_array(ud->record);
    ud->record->string_array = map;
    ud->record->string_array_count = count;
    lv_buttonmatrix_set_map(obj, (const char * const *)map);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_buttonmatrix_set_selected(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    int index = (int)luaL_checkinteger(L, 2);

    luaL_argcheck(L, index > 0, 2, "index must be 1-based and positive");
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_BUTTONMATRIX, "buttonmatrix");
    lv_buttonmatrix_set_selected_button(obj, (uint32_t)(index - 1));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_buttonmatrix_get_selected(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    uint32_t selected;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_BUTTONMATRIX, "buttonmatrix");
    selected = lv_buttonmatrix_get_selected_button(obj);
    lua_lvgl_unlock();
    if (selected == LV_BUTTONMATRIX_BUTTON_NONE) lua_pushnil(L);
    else lua_pushinteger(L, (lua_Integer)selected + 1);
    return 1;
}

int lua_lvgl_buttonmatrix_get_button_text(lua_State *L)
{
    int index = (int)luaL_checkinteger(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    const char *text;

    luaL_argcheck(L, index > 0, 2, "index must be 1-based and positive");
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_BUTTONMATRIX, "buttonmatrix");
    text = lv_buttonmatrix_get_button_text(obj, (uint32_t)(index - 1));
    lua_pushstring(L, text ? text : "");
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_buttonmatrix_set_one_checked(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_BUTTONMATRIX, "buttonmatrix");
    lv_buttonmatrix_set_one_checked(obj, lua_toboolean(L, 2));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_calendar_set_today(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CALENDAR, "calendar");
    lv_calendar_set_today_date(obj, (uint32_t)luaL_checkinteger(L, 2), (uint32_t)luaL_checkinteger(L, 3), (uint32_t)luaL_checkinteger(L, 4));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_calendar_set_shown(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CALENDAR, "calendar");
    lv_calendar_set_month_shown(obj, (uint32_t)luaL_checkinteger(L, 2), (uint32_t)luaL_checkinteger(L, 3));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_calendar_set_highlighted(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    size_t count;
    lv_calendar_date_t *dates = lua_lvgl_build_dates(L, 2, &count);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;

    if (err != ESP_OK) {
        free(dates);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CALENDAR, "calendar");
    free(ud->record->data);
    ud->record->data = dates;
    ud->record->data_size = count;
    lv_calendar_set_highlighted_dates(obj, dates, count);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_calendar_get_pressed_date(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    lv_calendar_date_t date;
    lv_result_t res;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CALENDAR, "calendar");
    res = lv_calendar_get_pressed_date(obj, &date);
    lua_lvgl_unlock();
    if (res != LV_RESULT_OK) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, date.year); lua_setfield(L, -2, "year");
    lua_pushinteger(L, date.month); lua_setfield(L, -2, "month");
    lua_pushinteger(L, date.day); lua_setfield(L, -2, "day");
    return 1;
}

int lua_lvgl_canvas_fill_bg(lua_State *L)
{
    lv_color_t color;
    lv_opa_t opa = lua_isnoneornil(L, 3) ? LV_OPA_COVER : (lv_opa_t)luaL_checkinteger(L, 3);
    esp_err_t err;
    lv_obj_t *obj;

    lua_lvgl_get_color_arg(L, 2, &color);
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CANVAS, "canvas");
    lv_canvas_fill_bg(obj, color, opa);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_canvas_set_px(lua_State *L)
{
    lv_color_t color;
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    lv_opa_t opa = lua_isnoneornil(L, 5) ? LV_OPA_COVER : (lv_opa_t)luaL_checkinteger(L, 5);
    esp_err_t err;
    lv_obj_t *obj;

    lua_lvgl_get_color_arg(L, 4, &color);
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CANVAS, "canvas");
    lv_canvas_set_px(obj, x, y, color, opa);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_canvas_get_px(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    lv_color32_t color;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CANVAS, "canvas");
    color = lv_canvas_get_px(obj, (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3));
    lua_lvgl_unlock();
    lua_newtable(L);
    lua_pushinteger(L, color.red); lua_setfield(L, -2, "r");
    lua_pushinteger(L, color.green); lua_setfield(L, -2, "g");
    lua_pushinteger(L, color.blue); lua_setfield(L, -2, "b");
    lua_pushinteger(L, color.alpha); lua_setfield(L, -2, "a");
    return 1;
}

int lua_lvgl_chart_add_series(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lv_color_t color;
    lv_chart_axis_t axis;
    esp_err_t err;
    lv_obj_t *obj;
    lv_chart_series_t *series;

    lua_lvgl_get_color_arg(L, 2, &color);
    if (lua_lvgl_parse_chart_axis(lua_isnoneornil(L, 3) ? NULL : luaL_checkstring(L, 3), &axis) != ESP_OK) {
        return luaL_error(L, "lvgl chart axis is invalid");
    }
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CHART, "chart");
    series = lv_chart_add_series(obj, color, axis);
    if (!series) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl chart add_series failed");
    }
    lua_lvgl_push_series(L, ud->record, series);
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_chart_set_type(lua_State *L)
{
    lv_chart_type_t type;
    esp_err_t err;
    lv_obj_t *obj;

    if (lua_lvgl_parse_chart_type(luaL_checkstring(L, 2), &type) != ESP_OK) {
        return luaL_error(L, "lvgl chart type is invalid");
    }
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CHART, "chart");
    lv_chart_set_type(obj, type);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_chart_set_point_count(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CHART, "chart");
    lv_chart_set_point_count(obj, (uint32_t)luaL_checkinteger(L, 2));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_chart_set_range(lua_State *L)
{
    lv_chart_axis_t axis;
    esp_err_t err;
    lv_obj_t *obj;

    if (lua_lvgl_parse_chart_axis(lua_isnoneornil(L, 4) ? NULL : luaL_checkstring(L, 4), &axis) != ESP_OK) {
        return luaL_error(L, "lvgl chart axis is invalid");
    }
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CHART, "chart");
    lv_chart_set_axis_range(obj, axis, (int32_t)luaL_checkinteger(L, 2), (int32_t)luaL_checkinteger(L, 3));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_chart_set_next_value(lua_State *L)
{
    lua_lvgl_chart_series_ud_t *series_ud;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CHART, "chart");
    series_ud = lua_lvgl_check_series(L, 2);
    lv_chart_set_next_value(obj, series_ud->series, (int32_t)luaL_checkinteger(L, 3));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_chart_set_series_values(lua_State *L)
{
    lua_lvgl_chart_series_ud_t *series_ud;
    int32_t *values;
    size_t count;
    esp_err_t err;
    lv_obj_t *obj;

    luaL_checktype(L, 3, LUA_TTABLE);
    count = lua_rawlen(L, 3);
    values = (int32_t *)calloc(count ? count : 1, sizeof(*values));
    if (!values) return luaL_error(L, "lvgl chart values allocation failed");
    for (size_t i = 1; i <= count; i++) {
        lua_rawgeti(L, 3, (lua_Integer)i);
        values[i - 1] = (int32_t)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }
    err = lua_lvgl_lock();
    if (err != ESP_OK) {
        free(values);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CHART, "chart");
    series_ud = lua_lvgl_check_series(L, 2);
    lv_chart_set_series_values(obj, series_ud->series, values, count);
    free(values);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_chart_refresh(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_CHART, "chart");
    lv_chart_refresh(obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_imagebutton_set_src(lua_State *L)
{
    lv_imagebutton_state_t state;
    const char *mid = luaL_checkstring(L, 3);
    const char *left = lua_isnoneornil(L, 4) ? NULL : luaL_checkstring(L, 4);
    const char *right = lua_isnoneornil(L, 5) ? NULL : luaL_checkstring(L, 5);
    esp_err_t err;
    lv_obj_t *obj;

    if (lua_lvgl_parse_imagebutton_state(luaL_checkstring(L, 2), &state) != ESP_OK) {
        return luaL_error(L, "lvgl imagebutton state is invalid");
    }
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_IMAGEBUTTON, "imagebutton");
    lv_imagebutton_set_src(obj, state, left, mid, right);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_imagebutton_set_state(lua_State *L)
{
    lv_imagebutton_state_t state;
    esp_err_t err;
    lv_obj_t *obj;

    if (lua_lvgl_parse_imagebutton_state(luaL_checkstring(L, 2), &state) != ESP_OK) {
        return luaL_error(L, "lvgl imagebutton state is invalid");
    }
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_IMAGEBUTTON, "imagebutton");
    lv_imagebutton_set_state(obj, state);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_led_set_color(lua_State *L)
{
    lv_color_t color;
    esp_err_t err;
    lv_obj_t *obj;

    lua_lvgl_get_color_arg(L, 2, &color);
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_LED, "led");
    lv_led_set_color(obj, color);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_led_set_brightness(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_LED, "led");
    lv_led_set_brightness(obj, (uint8_t)luaL_checkinteger(L, 2));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_led_get_brightness(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *obj;
    uint8_t value;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_LED, "led");
    value = lv_led_get_brightness(obj);
    lua_lvgl_unlock();
    lua_pushinteger(L, value);
    return 1;
}

int lua_lvgl_led_on(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_LED, "led"); lv_led_on(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }
int lua_lvgl_led_off(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_LED, "led"); lv_led_off(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }
int lua_lvgl_led_toggle(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_LED, "led"); lv_led_toggle(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }

int lua_lvgl_menu_page(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *title = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *menu;
    lv_obj_t *page;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    menu = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MENU, "menu");
    page = lv_menu_page_create(menu, title);
    lua_lvgl_push_obj(L, page, LUA_LVGL_OBJ_MENU_PAGE);
    (void)ud;
    lua_lvgl_unlock();
    return 1;
}

static int lua_lvgl_push_menu_child(lua_State *L, int parent_index, lua_lvgl_obj_type_t type, lv_obj_t *(*fn)(lv_obj_t *), const char *what)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *parent;
    lv_obj_t *child;
    const char *obj_error = NULL;
    lua_lvgl_obj_ud_t *parent_ud = lua_lvgl_check_ud(L, parent_index);

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    parent = lua_lvgl_validate_ud_locked(parent_ud, NULL, &obj_error);
    if (!parent) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    child = fn(parent);
    if (!child || !lua_lvgl_push_obj(L, child, type)) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl %s create failed", what);
    }
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_menu_cont(lua_State *L) { return lua_lvgl_push_menu_child(L, 2, LUA_LVGL_OBJ_MENU_CONT, lv_menu_cont_create, "menu cont"); }
int lua_lvgl_menu_section(lua_State *L) { return lua_lvgl_push_menu_child(L, 2, LUA_LVGL_OBJ_MENU_SECTION, lv_menu_section_create, "menu section"); }
int lua_lvgl_menu_separator(lua_State *L) { return lua_lvgl_push_menu_child(L, 2, LUA_LVGL_OBJ_MENU_SEPARATOR, lv_menu_separator_create, "menu separator"); }

static int lua_lvgl_menu_set_page_common(lua_State *L, bool sidebar)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *menu;
    lv_obj_t *page = NULL;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    menu = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MENU, "menu");
    if (!lua_isnoneornil(L, 2)) {
        page = lua_lvgl_check_typed_obj(L, 2, LUA_LVGL_OBJ_MENU_PAGE, "menu_page");
    }
    if (sidebar) lv_menu_set_sidebar_page(menu, page);
    else lv_menu_set_page(menu, page);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_menu_set_page(lua_State *L) { return lua_lvgl_menu_set_page_common(L, false); }
int lua_lvgl_menu_set_sidebar_page(lua_State *L) { return lua_lvgl_menu_set_page_common(L, true); }

int lua_lvgl_menu_set_mode_header(lua_State *L)
{
    lv_menu_mode_header_t mode;
    esp_err_t err;
    lv_obj_t *menu;

    if (lua_lvgl_parse_menu_header(luaL_checkstring(L, 2), &mode) != ESP_OK) {
        return luaL_error(L, "lvgl menu header mode is invalid");
    }
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    menu = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MENU, "menu");
    lv_menu_set_mode_header(menu, mode);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_menu_set_root_back_button(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *menu;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    menu = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MENU, "menu");
    lv_menu_set_mode_root_back_button(menu, lua_toboolean(L, 2) ? LV_MENU_ROOT_BACK_BUTTON_ENABLED : LV_MENU_ROOT_BACK_BUTTON_DISABLED);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_menu_clear_history(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *menu;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    menu = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MENU, "menu");
    lv_menu_clear_history(menu);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_lvgl_msgbox_push_child(lua_State *L, lv_obj_t *child)
{
    if (!child || !lua_lvgl_push_obj(L, child, LUA_LVGL_OBJ_MSGBOX_CHILD)) {
        return luaL_error(L, "lvgl msgbox child allocation failed");
    }
    return 1;
}

int lua_lvgl_msgbox_add_title(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MSGBOX, "msgbox"); lua_lvgl_msgbox_push_child(L, lv_msgbox_add_title(obj, luaL_checkstring(L, 2))); lua_lvgl_unlock(); return 1; }
int lua_lvgl_msgbox_add_text(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MSGBOX, "msgbox"); lua_lvgl_msgbox_push_child(L, lv_msgbox_add_text(obj, luaL_checkstring(L, 2))); lua_lvgl_unlock(); return 1; }
int lua_lvgl_msgbox_add_footer_button(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MSGBOX, "msgbox"); lua_lvgl_msgbox_push_child(L, lv_msgbox_add_footer_button(obj, luaL_checkstring(L, 2))); lua_lvgl_unlock(); return 1; }
int lua_lvgl_msgbox_add_close_button(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MSGBOX, "msgbox"); lua_lvgl_msgbox_push_child(L, lv_msgbox_add_close_button(obj)); lua_lvgl_unlock(); return 1; }
int lua_lvgl_msgbox_close(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MSGBOX, "msgbox"); lv_msgbox_close(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }
int lua_lvgl_msgbox_close_async(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_MSGBOX, "msgbox"); lv_msgbox_close_async(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }

int lua_lvgl_spangroup_add_span(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    const char *text = luaL_checkstring(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *group;
    lv_span_t *span;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    group = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPANGROUP, "spangroup");
    span = lv_spangroup_add_span(group);
    lv_spangroup_set_span_text(group, span, text);
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        /* P1 span styles are intentionally applied to the group-level widget
         * style selectors through the same compact style table used elsewhere. */
        lua_lvgl_apply_style_opts_locked(L, 3, group);
    }
    lv_spangroup_refresh(group);
    lua_lvgl_push_span(L, ud->record, span);
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_spangroup_get_span_count(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *group;
    uint32_t count;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    group = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPANGROUP, "spangroup");
    count = lv_spangroup_get_span_count(group);
    lua_lvgl_unlock();
    lua_pushinteger(L, count);
    return 1;
}

int lua_lvgl_spangroup_refresh(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *group;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    group = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPANGROUP, "spangroup");
    lv_spangroup_refresh(group);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_spinbox_set_step(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPINBOX, "spinbox"); lv_spinbox_set_step(obj, (uint32_t)luaL_checkinteger(L, 2)); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }
int lua_lvgl_spinbox_get_step(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; int32_t step; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPINBOX, "spinbox"); step = lv_spinbox_get_step(obj); lua_lvgl_unlock(); lua_pushinteger(L, step); return 1; }
int lua_lvgl_spinbox_increment(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPINBOX, "spinbox"); lv_spinbox_increment(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }
int lua_lvgl_spinbox_decrement(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPINBOX, "spinbox"); lv_spinbox_decrement(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }
int lua_lvgl_spinbox_step_next(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPINBOX, "spinbox"); lv_spinbox_step_next(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }
int lua_lvgl_spinbox_step_prev(lua_State *L) { esp_err_t err = lua_lvgl_lock(); lv_obj_t *obj; if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err); obj = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_SPINBOX, "spinbox"); lv_spinbox_step_prev(obj); lua_lvgl_unlock(); lua_pushboolean(L, 1); return 1; }

int lua_lvgl_tabview_add_tab(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *tv;
    lv_obj_t *tab;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TABVIEW, "tabview");
    tab = lv_tabview_add_tab(tv, luaL_checkstring(L, 2));
    lua_lvgl_push_obj(L, tab, LUA_LVGL_OBJ_TAB_PAGE);
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_tabview_set_active(lua_State *L)
{
    int index = (int)luaL_checkinteger(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *tv;

    luaL_argcheck(L, index > 0, 2, "index must be 1-based and positive");
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TABVIEW, "tabview");
    lv_tabview_set_active(tv, (uint32_t)(index - 1), lua_toboolean(L, 3) ? LV_ANIM_ON : LV_ANIM_OFF);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_tabview_get_active(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *tv;
    uint32_t active;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TABVIEW, "tabview");
    active = lv_tabview_get_tab_active(tv);
    lua_lvgl_unlock();
    lua_pushinteger(L, active + 1);
    return 1;
}

int lua_lvgl_tabview_get_tab_count(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *tv;
    uint32_t count;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TABVIEW, "tabview");
    count = lv_tabview_get_tab_count(tv);
    lua_lvgl_unlock();
    lua_pushinteger(L, count);
    return 1;
}

int lua_lvgl_tabview_set_tab_text(lua_State *L)
{
    int index = (int)luaL_checkinteger(L, 2);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *tv;

    luaL_argcheck(L, index > 0, 2, "index must be 1-based and positive");
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TABVIEW, "tabview");
    lv_tabview_set_tab_text(tv, (uint32_t)(index - 1), luaL_checkstring(L, 3));
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_tileview_add_tile(lua_State *L)
{
    int col = (int)luaL_checkinteger(L, 2);
    int row = (int)luaL_checkinteger(L, 3);
    lv_dir_t dir;
    esp_err_t err;
    lv_obj_t *tv;
    lv_obj_t *tile;

    luaL_argcheck(L, col > 0 && row > 0, 2, "col and row are 1-based and must be positive");
    if (lua_lvgl_parse_dir(lua_isnoneornil(L, 4) ? "all" : luaL_checkstring(L, 4), &dir) != ESP_OK) {
        return luaL_error(L, "lvgl tileview dir is invalid");
    }
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TILEVIEW, "tileview");
    tile = lv_tileview_add_tile(tv, (uint8_t)(col - 1), (uint8_t)(row - 1), dir);
    lua_lvgl_push_obj(L, tile, LUA_LVGL_OBJ_TILE);
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_tileview_set_tile(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *tv;
    lv_obj_t *tile;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TILEVIEW, "tileview");
    tile = lua_lvgl_check_typed_obj(L, 2, LUA_LVGL_OBJ_TILE, "tile");
    lv_tileview_set_tile(tv, tile, lua_toboolean(L, 3) ? LV_ANIM_ON : LV_ANIM_OFF);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_tileview_set_tile_by_index(lua_State *L)
{
    int col = (int)luaL_checkinteger(L, 2);
    int row = (int)luaL_checkinteger(L, 3);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *tv;

    luaL_argcheck(L, col > 0 && row > 0, 2, "col and row are 1-based and must be positive");
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TILEVIEW, "tileview");
    lv_tileview_set_tile_by_index(tv, (uint32_t)(col - 1), (uint32_t)(row - 1), lua_toboolean(L, 4) ? LV_ANIM_ON : LV_ANIM_OFF);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_tileview_get_active_tile(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *tv;
    lv_obj_t *tile;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    tv = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_TILEVIEW, "tileview");
    tile = lv_tileview_get_tile_active(tv);
    if (!tile || !lua_lvgl_push_obj(L, tile, LUA_LVGL_OBJ_TILE)) {
        lua_lvgl_unlock();
        lua_pushnil(L);
        return 1;
    }
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_window_add_title(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *win;
    lv_obj_t *child;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    win = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_WINDOW, "window");
    child = lv_win_add_title(win, luaL_checkstring(L, 2));
    lua_lvgl_push_obj(L, child, LUA_LVGL_OBJ_LABEL);
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_window_add_button(lua_State *L)
{
    const char *icon = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
    int width = lua_isnoneornil(L, 3) ? 32 : (int)luaL_checkinteger(L, 3);
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *win;
    lv_obj_t *child;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    win = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_WINDOW, "window");
    child = lv_win_add_button(win, icon, width);
    lua_lvgl_push_obj(L, child, LUA_LVGL_OBJ_BUTTON);
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_window_get_header(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *win;
    lv_obj_t *child;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    win = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_WINDOW, "window");
    child = lv_win_get_header(win);
    lua_lvgl_push_obj(L, child, LUA_LVGL_OBJ_WINDOW_CHILD);
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_window_get_content(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *win;
    lv_obj_t *child;

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    win = lua_lvgl_check_typed_obj(L, 1, LUA_LVGL_OBJ_WINDOW, "window");
    child = lv_win_get_content(win);
    lua_lvgl_push_obj(L, child, LUA_LVGL_OBJ_WINDOW_CHILD);
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_span_set_text(lua_State *L)
{
    lua_lvgl_span_ud_t *ud;
    esp_err_t err = lua_lvgl_lock();

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    ud = lua_lvgl_check_span(L, 1);
    lv_spangroup_set_span_text(ud->group_record->obj, ud->span, luaL_checkstring(L, 2));
    lv_spangroup_refresh(ud->group_record->obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_span_get_text(lua_State *L)
{
    lua_lvgl_span_ud_t *ud;
    const char *text;
    esp_err_t err = lua_lvgl_lock();

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    ud = lua_lvgl_check_span(L, 1);
    text = lv_span_get_text(ud->span);
    lua_pushstring(L, text ? text : "");
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_span_set_style(lua_State *L)
{
    lua_lvgl_span_ud_t *ud;
    esp_err_t err;

    luaL_checktype(L, 2, LUA_TTABLE);
    err = lua_lvgl_lock();
    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    ud = lua_lvgl_check_span(L, 1);
    lua_lvgl_apply_style_opts_locked(L, 2, ud->group_record->obj);
    lv_spangroup_refresh(ud->group_record->obj);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_span_delete(lua_State *L)
{
    lua_lvgl_span_ud_t *ud;
    esp_err_t err = lua_lvgl_lock();

    if (err != ESP_OK) return lua_lvgl_error_esp(L, "lock", err);
    ud = lua_lvgl_check_span(L, 1);
    lv_spangroup_delete_span(ud->group_record->obj, ud->span);
    lv_spangroup_refresh(ud->group_record->obj);
    ud->valid = false;
    ud->span = NULL;
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_span_gc(lua_State *L)
{
    lua_lvgl_span_ud_t *ud = (lua_lvgl_span_ud_t *)lua_touserdata(L, 1);
    if (ud) {
        ud->valid = false;
        ud->span = NULL;
        ud->group_record = NULL;
    }
    return 0;
}

int lua_lvgl_chart_series_gc(lua_State *L)
{
    lua_lvgl_chart_series_ud_t *ud = (lua_lvgl_chart_series_ud_t *)lua_touserdata(L, 1);
    if (ud) {
        ud->series = NULL;
        ud->chart_record = NULL;
    }
    return 0;
}

static void lua_lvgl_apply_complex_opts(lua_State *L, lua_lvgl_obj_ud_t *ud, lua_lvgl_obj_type_t type)
{
    lv_obj_t *obj = ud->record->obj;

    switch (type) {
    case LUA_LVGL_OBJ_BUTTONMATRIX:
        if (lua_lvgl_has_field(L, 2, "map")) {
            lua_getfield(L, 2, "map");
            size_t count;
            char **map = lua_lvgl_build_string_array(L, -1, &count, true);
            lua_lvgl_release_string_array(ud->record);
            ud->record->string_array = map;
            ud->record->string_array_count = count;
            lv_buttonmatrix_set_map(obj, (const char * const *)map);
            lua_pop(L, 1);
        }
        if (lua_lvgl_has_field(L, 2, "one_checked")) {
            lv_buttonmatrix_set_one_checked(obj, lua_lvgl_get_opt_bool_field(L, 2, "one_checked", false));
        }
        break;
    case LUA_LVGL_OBJ_CALENDAR:
        lua_getfield(L, 2, "today");
        if (!lua_isnil(L, -1)) {
            lv_calendar_date_t d = lua_lvgl_date_from_table(L, -1);
            lv_calendar_set_today_date(obj, d.year, d.month, d.day);
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "shown");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            lua_rawgeti(L, -1, 1);
            uint32_t year = (uint32_t)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 2);
            uint32_t month = (uint32_t)luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            lv_calendar_set_month_shown(obj, year, month);
        }
        lua_pop(L, 1);
        if (lua_lvgl_has_field(L, 2, "highlighted")) {
            lua_getfield(L, 2, "highlighted");
            size_t count;
            lv_calendar_date_t *dates = lua_lvgl_build_dates(L, -1, &count);
            free(ud->record->data);
            ud->record->data = dates;
            ud->record->data_size = count;
            lv_calendar_set_highlighted_dates(obj, dates, count);
            lua_pop(L, 1);
        }
        if (lua_lvgl_has_field(L, 2, "day_names")) {
            lua_getfield(L, 2, "day_names");
            size_t count;
            char **names = lua_lvgl_build_string_array(L, -1, &count, false);
            if (count != 7) {
                for (size_t i = 0; i < count; i++) free(names[i]);
                free(names);
                lua_pop(L, 1);
                luaL_error(L, "lvgl calendar day_names requires 7 strings");
            }
            lua_lvgl_release_string_array(ud->record);
            ud->record->string_array = names;
            ud->record->string_array_count = count;
            lv_calendar_set_day_names(obj, (const char **)names);
            lua_pop(L, 1);
        }
        break;
    case LUA_LVGL_OBJ_CANVAS: {
        lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
        int w = lua_lvgl_get_opt_int_field(L, 2, "w", 0);
        int h = lua_lvgl_get_opt_int_field(L, 2, "h", 0);
        if (w > 0 && h > 0) {
            if (lua_lvgl_parse_canvas_cf(lua_lvgl_get_opt_string_field(L, 2, "color_format"), &cf) != ESP_OK) {
                luaL_error(L, "lvgl canvas color_format is invalid");
            }
            uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, cf);
            size_t size = (size_t)stride * (size_t)h;
            void *buf = heap_caps_calloc(1, size, MALLOC_CAP_DEFAULT);
            if (!buf) {
                luaL_error(L, "lvgl canvas buffer allocation failed");
            }
            free(ud->record->data);
            ud->record->data = buf;
            ud->record->data_size = size;
            lv_canvas_set_buffer(obj, buf, w, h, cf);
        }
        break;
    }
    case LUA_LVGL_OBJ_CHART: {
        lv_chart_type_t chart_type = LV_CHART_TYPE_LINE;
        lv_chart_update_mode_t update_mode = LV_CHART_UPDATE_MODE_SHIFT;
        if (lua_lvgl_parse_chart_type(lua_lvgl_get_opt_string_field(L, 2, "type"), &chart_type) != ESP_OK ||
                lua_lvgl_parse_chart_update_mode(lua_lvgl_get_opt_string_field(L, 2, "update_mode"), &update_mode) != ESP_OK) {
            luaL_error(L, "lvgl chart option is invalid");
        }
        lv_chart_set_type(obj, chart_type);
        lv_chart_set_update_mode(obj, update_mode);
        if (lua_lvgl_has_field(L, 2, "point_count")) {
            lv_chart_set_point_count(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "point_count", 10));
        }
        lv_chart_set_axis_range(obj, LV_CHART_AXIS_PRIMARY_Y,
                                lua_lvgl_get_opt_int_field(L, 2, "min", 0),
                                lua_lvgl_get_opt_int_field(L, 2, "max", 100));
        break;
    }
    case LUA_LVGL_OBJ_IMAGEBUTTON:
        if (lua_lvgl_has_field(L, 2, "src")) {
            lv_imagebutton_set_src(obj, LV_IMAGEBUTTON_STATE_RELEASED, NULL, lua_lvgl_get_opt_string_field(L, 2, "src"), NULL);
        }
        break;
    case LUA_LVGL_OBJ_LED:
        if (lua_lvgl_has_field(L, 2, "color")) {
            lv_color_t color;
            lua_getfield(L, 2, "color");
            if (lua_lvgl_parse_color(L, -1, &color) != ESP_OK) {
                luaL_error(L, "lvgl led color is invalid");
            }
            lv_led_set_color(obj, color);
            lua_pop(L, 1);
        }
        if (lua_lvgl_has_field(L, 2, "brightness")) {
            lv_led_set_brightness(obj, (uint8_t)lua_lvgl_get_opt_int_field(L, 2, "brightness", 255));
        }
        if (lua_lvgl_get_opt_bool_field(L, 2, "on", false)) {
            lv_led_on(obj);
        }
        break;
    case LUA_LVGL_OBJ_SPANGROUP: {
        lv_span_mode_t mode = LV_SPAN_MODE_FIXED;
        lv_span_overflow_t overflow = LV_SPAN_OVERFLOW_CLIP;
        if (lua_lvgl_parse_span_mode(lua_lvgl_get_opt_string_field(L, 2, "mode"), &mode) != ESP_OK ||
                lua_lvgl_parse_span_overflow(lua_lvgl_get_opt_string_field(L, 2, "overflow"), &overflow) != ESP_OK) {
            luaL_error(L, "lvgl spangroup option is invalid");
        }
        lv_spangroup_set_mode(obj, mode);
        lv_spangroup_set_overflow(obj, overflow);
        if (lua_lvgl_has_field(L, 2, "indent")) lv_spangroup_set_indent(obj, lua_lvgl_get_opt_int_field(L, 2, "indent", 0));
        if (lua_lvgl_has_field(L, 2, "max_lines")) lv_spangroup_set_max_lines(obj, lua_lvgl_get_opt_int_field(L, 2, "max_lines", -1));
        lua_getfield(L, 2, "spans");
        if (!lua_isnil(L, -1)) {
            luaL_checktype(L, -1, LUA_TTABLE);
            for (size_t i = 1; i <= lua_rawlen(L, -1); i++) {
                lua_rawgeti(L, -1, (lua_Integer)i);
                lv_span_t *span = lv_spangroup_add_span(obj);
                lv_spangroup_set_span_text(obj, span, luaL_checkstring(L, -1));
                lua_pop(L, 1);
            }
            lv_spangroup_refresh(obj);
        }
        lua_pop(L, 1);
        break;
    }
    case LUA_LVGL_OBJ_SPINBOX:
        lv_spinbox_set_range(obj, lua_lvgl_get_opt_int_field(L, 2, "min", 0), lua_lvgl_get_opt_int_field(L, 2, "max", 100));
        lv_spinbox_set_value(obj, lua_lvgl_get_opt_int_field(L, 2, "value", 0));
        if (lua_lvgl_has_field(L, 2, "step")) lv_spinbox_set_step(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "step", 1));
        if (lua_lvgl_has_field(L, 2, "digit_count") || lua_lvgl_has_field(L, 2, "dec_point_pos")) {
            lv_spinbox_set_digit_format(obj,
                                        (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "digit_count", 4),
                                        (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "dec_point_pos", 0));
        }
        if (lua_lvgl_has_field(L, 2, "rollover")) lv_spinbox_set_rollover(obj, lua_lvgl_get_opt_bool_field(L, 2, "rollover", false));
        break;
    case LUA_LVGL_OBJ_TABVIEW:
        if (lua_lvgl_has_field(L, 2, "tab_bar_position")) {
            lv_dir_t dir;
            if (lua_lvgl_parse_dir(lua_lvgl_get_opt_string_field(L, 2, "tab_bar_position"), &dir) != ESP_OK) {
                luaL_error(L, "lvgl tabview tab_bar_position is invalid");
            }
            lv_tabview_set_tab_bar_position(obj, dir);
        }
        if (lua_lvgl_has_field(L, 2, "tab_bar_size")) {
            lv_tabview_set_tab_bar_size(obj, lua_lvgl_get_opt_int_field(L, 2, "tab_bar_size", 40));
        }
        break;
    default:
        break;
    }
}

/* Hook called from lua_lvgl_create_widget after base options are applied. */
void lua_lvgl_apply_complex_widget_opts(lua_State *L, lua_lvgl_obj_ud_t *ud, lua_lvgl_obj_type_t type)
{
    if (lua_lvgl_opt_table(L, 2)) {
        lua_lvgl_apply_complex_opts(L, ud, type);
    }
}

void lua_lvgl_register_handle_metatables(lua_State *L)
{
    static const luaL_Reg series_methods[] = {
        {NULL, NULL},
    };
    static const luaL_Reg span_methods[] = {
        {"set_text", lua_lvgl_span_set_text},
        {"get_text", lua_lvgl_span_get_text},
        {"set_style", lua_lvgl_span_set_style},
        {"delete", lua_lvgl_span_delete},
        {NULL, NULL},
    };

    if (luaL_newmetatable(L, LUA_LVGL_CHART_SERIES_MT)) {
        lua_newtable(L);
        luaL_setfuncs(L, series_methods, 0);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_lvgl_chart_series_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, LUA_LVGL_SPAN_MT)) {
        lua_newtable(L);
        luaL_setfuncs(L, span_methods, 0);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_lvgl_span_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
}

const luaL_Reg lua_lvgl_complex_widget_funcs[] = {
    {"buttonmatrix", lua_lvgl_buttonmatrix},
    {"calendar", lua_lvgl_calendar},
    {"canvas", lua_lvgl_canvas},
    {"chart", lua_lvgl_chart},
    {"imagebutton", lua_lvgl_imagebutton},
    {"led", lua_lvgl_led},
    {"menu", lua_lvgl_menu},
    {"msgbox", lua_lvgl_msgbox},
    {"spangroup", lua_lvgl_spangroup},
    {"spinbox", lua_lvgl_spinbox},
    {"tabview", lua_lvgl_tabview},
    {"tileview", lua_lvgl_tileview},
    {"window", lua_lvgl_window},
    {NULL, NULL},
};
