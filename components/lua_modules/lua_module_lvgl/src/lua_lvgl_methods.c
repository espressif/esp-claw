/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Method-style API metatables.
 *
 * Each LVGL widget type owns its own metatable named "lvgl.obj.<type>". The
 * metatable carries:
 *   __index      = a per-type "methods" table that itself has a metatable
 *                  whose __index is the shared base methods table; this
 *                  yields a two-level inheritance chain so that all widgets
 *                  inherit the common operations (set_pos / get_pos /
 *                  align / set_style / set_flex / ... / delete / clean /
 *                  is_valid) without duplicating entries.
 *   __gc         = lua_lvgl_obj_gc (shared)
 *   __name       = "lvgl.obj.<type>" (purely informational / debug aid)
 *   __lvgl_obj   = true (sentinel field used by lua_lvgl_check_ud to
 *                  recognize any of the per-type metatables uniformly)
 *
 * Registration is performed once from luaopen_lvgl through
 * lua_lvgl_register_metatables(). After that, lua_lvgl_push_obj() selects
 * the appropriate metatable name via lua_lvgl_metatable_for_type().
 */

#include "lua_lvgl_private.h"

/* --- Base method table: methods every widget gets ---------------------- */

static const luaL_Reg lua_lvgl_base_methods[] = {
    {"set_pos", lua_lvgl_set_pos},
    {"get_pos", lua_lvgl_get_pos},
    {"set_size", lua_lvgl_set_size},
    {"get_size", lua_lvgl_get_size},
    {"align", lua_lvgl_align},
    {"is_valid", lua_lvgl_is_valid},
    {"set_style", lua_lvgl_set_style},
    {"set_flex", lua_lvgl_set_flex},
    {"set_grid", lua_lvgl_set_grid},
    {"set_grid_cell", lua_lvgl_set_grid_cell},
    {"set_scroll", lua_lvgl_set_scroll},
    {"on", lua_lvgl_obj_on},
    {"off", lua_lvgl_obj_off},
    {"delete", lua_lvgl_delete},
    {"clean", lua_lvgl_clean},
    {NULL, NULL},
};

/* --- Per-type extra method tables -------------------------------------- */

static const luaL_Reg lua_lvgl_screen_methods[] = {
    {"load", lua_lvgl_screen_load},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_label_methods[] = {
    {"set_text", lua_lvgl_set_text},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_button_methods[] = {
    {"set_text", lua_lvgl_set_text},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_bar_methods[] = {
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {"set_range", lua_lvgl_set_range},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_slider_methods[] = {
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {"set_range", lua_lvgl_set_range},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_arc_methods[] = {
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {"set_range", lua_lvgl_set_range},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_scale_methods[] = {
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {"set_range", lua_lvgl_set_range},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_checkbox_methods[] = {
    {"set_text", lua_lvgl_set_text},
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_switch_methods[] = {
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_dropdown_methods[] = {
    {"set_text", lua_lvgl_set_text},
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_roller_methods[] = {
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_textarea_methods[] = {
    {"set_text", lua_lvgl_set_text},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_list_methods[] = {
    {"add_text", lua_lvgl_list_add_text},
    {"add_button", lua_lvgl_list_add_button},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_list_text_methods[] = {
    {"set_text", lua_lvgl_set_text},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_list_button_methods[] = {
    {"set_text", lua_lvgl_set_text},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_table_methods[] = {
    {"set_cell", lua_lvgl_table_set_cell},
    {"get_cell", lua_lvgl_table_get_cell},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_buttonmatrix_methods[] = {
    {"set_map", lua_lvgl_buttonmatrix_set_map},
    {"set_selected", lua_lvgl_buttonmatrix_set_selected},
    {"get_selected", lua_lvgl_buttonmatrix_get_selected},
    {"get_button_text", lua_lvgl_buttonmatrix_get_button_text},
    {"set_one_checked", lua_lvgl_buttonmatrix_set_one_checked},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_calendar_methods[] = {
    {"set_today", lua_lvgl_calendar_set_today},
    {"set_shown", lua_lvgl_calendar_set_shown},
    {"set_highlighted", lua_lvgl_calendar_set_highlighted},
    {"get_pressed_date", lua_lvgl_calendar_get_pressed_date},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_canvas_methods[] = {
    {"fill_bg", lua_lvgl_canvas_fill_bg},
    {"set_px", lua_lvgl_canvas_set_px},
    {"get_px", lua_lvgl_canvas_get_px},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_chart_methods[] = {
    {"add_series", lua_lvgl_chart_add_series},
    {"set_type", lua_lvgl_chart_set_type},
    {"set_point_count", lua_lvgl_chart_set_point_count},
    {"set_range", lua_lvgl_chart_set_range},
    {"set_next_value", lua_lvgl_chart_set_next_value},
    {"set_series_values", lua_lvgl_chart_set_series_values},
    {"refresh", lua_lvgl_chart_refresh},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_imagebutton_methods[] = {
    {"set_src", lua_lvgl_imagebutton_set_src},
    {"set_state", lua_lvgl_imagebutton_set_state},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_led_methods[] = {
    {"set_color", lua_lvgl_led_set_color},
    {"set_brightness", lua_lvgl_led_set_brightness},
    {"get_brightness", lua_lvgl_led_get_brightness},
    {"on", lua_lvgl_led_on},
    {"off", lua_lvgl_led_off},
    {"toggle", lua_lvgl_led_toggle},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_menu_methods[] = {
    {"page", lua_lvgl_menu_page},
    {"cont", lua_lvgl_menu_cont},
    {"section", lua_lvgl_menu_section},
    {"separator", lua_lvgl_menu_separator},
    {"set_page", lua_lvgl_menu_set_page},
    {"set_sidebar_page", lua_lvgl_menu_set_sidebar_page},
    {"set_mode_header", lua_lvgl_menu_set_mode_header},
    {"set_root_back_button", lua_lvgl_menu_set_root_back_button},
    {"clear_history", lua_lvgl_menu_clear_history},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_msgbox_methods[] = {
    {"add_title", lua_lvgl_msgbox_add_title},
    {"add_text", lua_lvgl_msgbox_add_text},
    {"add_footer_button", lua_lvgl_msgbox_add_footer_button},
    {"add_close_button", lua_lvgl_msgbox_add_close_button},
    {"close", lua_lvgl_msgbox_close},
    {"close_async", lua_lvgl_msgbox_close_async},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_spangroup_methods[] = {
    {"add_span", lua_lvgl_spangroup_add_span},
    {"get_span_count", lua_lvgl_spangroup_get_span_count},
    {"refresh", lua_lvgl_spangroup_refresh},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_spinbox_methods[] = {
    {"set_value", lua_lvgl_set_value},
    {"get_value", lua_lvgl_get_value},
    {"set_range", lua_lvgl_set_range},
    {"set_step", lua_lvgl_spinbox_set_step},
    {"get_step", lua_lvgl_spinbox_get_step},
    {"increment", lua_lvgl_spinbox_increment},
    {"decrement", lua_lvgl_spinbox_decrement},
    {"step_next", lua_lvgl_spinbox_step_next},
    {"step_prev", lua_lvgl_spinbox_step_prev},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_tabview_methods[] = {
    {"add_tab", lua_lvgl_tabview_add_tab},
    {"set_active", lua_lvgl_tabview_set_active},
    {"get_active", lua_lvgl_tabview_get_active},
    {"get_tab_count", lua_lvgl_tabview_get_tab_count},
    {"set_tab_text", lua_lvgl_tabview_set_tab_text},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_tileview_methods[] = {
    {"add_tile", lua_lvgl_tileview_add_tile},
    {"set_tile", lua_lvgl_tileview_set_tile},
    {"set_tile_by_index", lua_lvgl_tileview_set_tile_by_index},
    {"get_active_tile", lua_lvgl_tileview_get_active_tile},
    {NULL, NULL},
};

static const luaL_Reg lua_lvgl_window_methods[] = {
    {"add_title", lua_lvgl_window_add_title},
    {"add_button", lua_lvgl_window_add_button},
    {"get_header", lua_lvgl_window_get_header},
    {"get_content", lua_lvgl_window_get_content},
    {NULL, NULL},
};

/* image / line / spinner / keyboard / generic / container expose the base
 * method set only; an empty per-type extra table is enough. */
static const luaL_Reg lua_lvgl_no_extra_methods[] = {
    {NULL, NULL},
};

/* --- Type -> metatable name + extra methods table ---------------------- */

typedef struct {
    lua_lvgl_obj_type_t type;
    const char *mt_name;
    const luaL_Reg *extra_methods;
} lua_lvgl_widget_descriptor_t;

static const lua_lvgl_widget_descriptor_t s_widget_descriptors[] = {
    {LUA_LVGL_OBJ_GENERIC,     "lvgl.obj.generic",     lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_SCREEN,      "lvgl.obj.screen",      lua_lvgl_screen_methods},
    {LUA_LVGL_OBJ_CONTAINER,   "lvgl.obj.container",   lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_LABEL,       "lvgl.obj.label",       lua_lvgl_label_methods},
    {LUA_LVGL_OBJ_BUTTON,      "lvgl.obj.button",      lua_lvgl_button_methods},
    {LUA_LVGL_OBJ_BAR,         "lvgl.obj.bar",         lua_lvgl_bar_methods},
    {LUA_LVGL_OBJ_SLIDER,      "lvgl.obj.slider",      lua_lvgl_slider_methods},
    {LUA_LVGL_OBJ_IMAGE,       "lvgl.obj.image",       lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_LINE,        "lvgl.obj.line",        lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_ARC,         "lvgl.obj.arc",         lua_lvgl_arc_methods},
    {LUA_LVGL_OBJ_SPINNER,     "lvgl.obj.spinner",     lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_SCALE,       "lvgl.obj.scale",       lua_lvgl_scale_methods},
    {LUA_LVGL_OBJ_CHECKBOX,    "lvgl.obj.checkbox",    lua_lvgl_checkbox_methods},
    {LUA_LVGL_OBJ_SWITCH,      "lvgl.obj.switch",      lua_lvgl_switch_methods},
    {LUA_LVGL_OBJ_DROPDOWN,    "lvgl.obj.dropdown",    lua_lvgl_dropdown_methods},
    {LUA_LVGL_OBJ_ROLLER,      "lvgl.obj.roller",      lua_lvgl_roller_methods},
    {LUA_LVGL_OBJ_KEYBOARD,    "lvgl.obj.keyboard",    lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_LIST,        "lvgl.obj.list",        lua_lvgl_list_methods},
    {LUA_LVGL_OBJ_LIST_TEXT,   "lvgl.obj.list_text",   lua_lvgl_list_text_methods},
    {LUA_LVGL_OBJ_LIST_BUTTON, "lvgl.obj.list_button", lua_lvgl_list_button_methods},
    {LUA_LVGL_OBJ_TEXTAREA,    "lvgl.obj.textarea",    lua_lvgl_textarea_methods},
    {LUA_LVGL_OBJ_TABLE,       "lvgl.obj.table",       lua_lvgl_table_methods},
    {LUA_LVGL_OBJ_BUTTONMATRIX, "lvgl.obj.buttonmatrix", lua_lvgl_buttonmatrix_methods},
    {LUA_LVGL_OBJ_CALENDAR,    "lvgl.obj.calendar",    lua_lvgl_calendar_methods},
    {LUA_LVGL_OBJ_CANVAS,      "lvgl.obj.canvas",      lua_lvgl_canvas_methods},
    {LUA_LVGL_OBJ_CHART,       "lvgl.obj.chart",       lua_lvgl_chart_methods},
    {LUA_LVGL_OBJ_IMAGEBUTTON, "lvgl.obj.imagebutton", lua_lvgl_imagebutton_methods},
    {LUA_LVGL_OBJ_LED,         "lvgl.obj.led",         lua_lvgl_led_methods},
    {LUA_LVGL_OBJ_MENU,        "lvgl.obj.menu",        lua_lvgl_menu_methods},
    {LUA_LVGL_OBJ_MENU_PAGE,   "lvgl.obj.menu_page",   lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_MENU_CONT,   "lvgl.obj.menu_cont",   lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_MENU_SECTION, "lvgl.obj.menu_section", lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_MENU_SEPARATOR, "lvgl.obj.menu_separator", lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_MSGBOX,      "lvgl.obj.msgbox",      lua_lvgl_msgbox_methods},
    {LUA_LVGL_OBJ_MSGBOX_CHILD, "lvgl.obj.msgbox_child", lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_SPANGROUP,   "lvgl.obj.spangroup",   lua_lvgl_spangroup_methods},
    {LUA_LVGL_OBJ_SPINBOX,     "lvgl.obj.spinbox",     lua_lvgl_spinbox_methods},
    {LUA_LVGL_OBJ_TABVIEW,     "lvgl.obj.tabview",     lua_lvgl_tabview_methods},
    {LUA_LVGL_OBJ_TAB_PAGE,    "lvgl.obj.tab_page",    lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_TILEVIEW,    "lvgl.obj.tileview",    lua_lvgl_tileview_methods},
    {LUA_LVGL_OBJ_TILE,        "lvgl.obj.tile",        lua_lvgl_no_extra_methods},
    {LUA_LVGL_OBJ_WINDOW,      "lvgl.obj.window",      lua_lvgl_window_methods},
    {LUA_LVGL_OBJ_WINDOW_CHILD, "lvgl.obj.window_child", lua_lvgl_no_extra_methods},
};

#define LUA_LVGL_WIDGET_DESCRIPTOR_COUNT \
    (sizeof(s_widget_descriptors) / sizeof(s_widget_descriptors[0]))

const char *lua_lvgl_metatable_for_type(lua_lvgl_obj_type_t type)
{
    for (size_t i = 0; i < LUA_LVGL_WIDGET_DESCRIPTOR_COUNT; i++) {
        if (s_widget_descriptors[i].type == type) {
            return s_widget_descriptors[i].mt_name;
        }
    }
    /* Defensive fallback: an unknown type still gets a valid metatable so
     * the userdata remains recognizable to lua_lvgl_check_ud. */
    return "lvgl.obj.generic";
}

/* --- Metatable construction helpers ------------------------------------ */

/* Build a per-type "methods" table whose metatable's __index points to the
 * shared base methods table at stack absolute index `base_idx`. The newly
 * built methods table is left at the top of the stack on return.
 *
 * Effect on stack: pushes exactly one new value (the methods table).
 */
static void lua_lvgl_build_methods_table(lua_State *L, int base_idx, const luaL_Reg *extra_methods)
{
    lua_newtable(L);                       /* methods */
    if (extra_methods != NULL) {
        luaL_setfuncs(L, extra_methods, 0);
    }

    lua_newtable(L);                       /* mt_for_methods */
    lua_pushvalue(L, base_idx);            /* base_methods */
    lua_setfield(L, -2, "__index");        /* mt_for_methods.__index = base */
    lua_setmetatable(L, -2);               /* setmetatable(methods, mt_for_methods) */
}

static void lua_lvgl_register_one_metatable(lua_State *L,
                                            int base_idx,
                                            const lua_lvgl_widget_descriptor_t *desc)
{
    if (luaL_newmetatable(L, desc->mt_name) == 0) {
        /* Already exists (e.g. interpreter re-init). Drop the duplicate
         * stack entry and skip rebuilding to avoid trampling on existing
         * Lua references to this metatable. */
        lua_pop(L, 1);
        return;
    }
    /* mt is now at top of stack. */

    lua_lvgl_build_methods_table(L, base_idx, desc->extra_methods);
    lua_setfield(L, -2, "__index");

    lua_pushcfunction(L, lua_lvgl_obj_gc);
    lua_setfield(L, -2, "__gc");

    lua_pushstring(L, desc->mt_name);
    lua_setfield(L, -2, "__name");

    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "__lvgl_obj");

    lua_pop(L, 1);
}

void lua_lvgl_register_metatables(lua_State *L)
{
    int base_idx;

    lua_lvgl_register_handle_metatables(L);

    /* Build the base methods table once and keep it on the stack while we
     * register every per-type metatable so each can reference the same
     * shared instance via __index. */
    lua_newtable(L);
    luaL_setfuncs(L, lua_lvgl_base_methods, 0);
    base_idx = lua_gettop(L);

    for (size_t i = 0; i < LUA_LVGL_WIDGET_DESCRIPTOR_COUNT; i++) {
        lua_lvgl_register_one_metatable(L, base_idx, &s_widget_descriptors[i]);
    }

    lua_pop(L, 1);
}
