/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_lvgl_private.h"

static int lua_lvgl_screen(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *screen;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (!s_lvgl.runtime_initialized) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl runtime is not initialized");
    }
    screen = lv_screen_active();
    if (!lua_lvgl_push_obj(L, screen, LUA_LVGL_OBJ_SCREEN)) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    lua_lvgl_unlock();
    return 1;
}

static int lua_lvgl_create_screen(lua_State *L)
{
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *screen;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    if (!s_lvgl.runtime_initialized) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl runtime is not initialized");
    }
    screen = lv_obj_create(NULL);
    if (!lua_lvgl_push_obj(L, screen, LUA_LVGL_OBJ_SCREEN)) {
        lv_obj_delete(screen);
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl object record allocation failed");
    }
    lua_lvgl_unlock();
    return 1;
}

int lua_lvgl_screen_load(lua_State *L)
{
    lua_lvgl_obj_ud_t *ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_obj_type_t type;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *screen;
    const char *obj_error = NULL;

    if (err != ESP_OK) {
        return lua_lvgl_error_esp(L, "lock", err);
    }
    screen = lua_lvgl_validate_ud_locked(ud, &type, &obj_error);
    if (!screen) {
        lua_lvgl_unlock();
        return luaL_error(L, "%s", obj_error);
    }
    if (type != LUA_LVGL_OBJ_SCREEN) {
        lua_lvgl_unlock();
        return luaL_error(L, "lvgl load requires a screen object");
    }
    lv_screen_load(screen);
    lua_lvgl_unlock();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_lvgl_create_widget(lua_State *L, lua_lvgl_obj_type_t type)
{
    lua_lvgl_obj_ud_t *parent_ud = lua_lvgl_check_ud(L, 1);
    lua_lvgl_opts_t opts;
    lv_align_t align;
    esp_err_t err = lua_lvgl_lock();
    lv_obj_t *parent;
    lv_obj_t *obj = NULL;
    lua_lvgl_obj_ud_t *created_ud;
    char *options = NULL;
    lv_point_precise_t *line_points = NULL;
    uint32_t line_point_count = 0;
    const char *obj_error = NULL;

    lua_lvgl_parse_opts(L, 2, &opts);
    if (opts.align_value && lua_lvgl_parse_align(L, opts.align_value, &align) != ESP_OK) {
        return luaL_error(L, "lvgl align must be top_left, top_mid, top_right, bottom_left, bottom_mid, bottom_right, left_mid, right_mid, or center");
    }
    if ((type == LUA_LVGL_OBJ_BAR || type == LUA_LVGL_OBJ_SLIDER || type == LUA_LVGL_OBJ_ARC || type == LUA_LVGL_OBJ_SCALE) &&
            opts.max_value <= opts.min_value) {
        return luaL_error(L, "lvgl option 'max' must be greater than 'min'");
    }
    if (type == LUA_LVGL_OBJ_LINE && (!opts.has_opts || !lua_lvgl_has_field(L, 2, "points"))) {
        return luaL_error(L, "lvgl line requires opts.points");
    }
    if (type == LUA_LVGL_OBJ_LINE) {
        line_points = lua_lvgl_build_line_points(L, 2, &line_point_count);
    }
    if ((type == LUA_LVGL_OBJ_DROPDOWN || type == LUA_LVGL_OBJ_ROLLER) &&
            opts.has_opts &&
            lua_lvgl_has_field(L, 2, "options")) {
        options = lua_lvgl_build_options_string(L, 2, "options");
    }

    if (err != ESP_OK) {
        free(options);
        free(line_points);
        return lua_lvgl_error_esp(L, "lock", err);
    }
    parent = lua_lvgl_validate_ud_locked(parent_ud, NULL, &obj_error);
    if (!parent) {
        lua_lvgl_unlock();
        free(options);
        free(line_points);
        return luaL_error(L, "%s", obj_error);
    }

    switch (type) {
    case LUA_LVGL_OBJ_GENERIC:
    case LUA_LVGL_OBJ_CONTAINER:
        obj = lv_obj_create(parent);
        break;
    case LUA_LVGL_OBJ_LABEL:
        obj = lv_label_create(parent);
        break;
    case LUA_LVGL_OBJ_BUTTON:
        obj = lv_button_create(parent);
        break;
    case LUA_LVGL_OBJ_BAR:
        obj = lv_bar_create(parent);
        break;
    case LUA_LVGL_OBJ_SLIDER:
        obj = lv_slider_create(parent);
        break;
    case LUA_LVGL_OBJ_IMAGE:
        obj = lv_image_create(parent);
        break;
    case LUA_LVGL_OBJ_LINE:
        obj = lv_line_create(parent);
        break;
    case LUA_LVGL_OBJ_ARC:
        obj = lv_arc_create(parent);
        break;
    case LUA_LVGL_OBJ_SPINNER:
        obj = lv_spinner_create(parent);
        break;
    case LUA_LVGL_OBJ_SCALE:
        obj = lv_scale_create(parent);
        break;
    case LUA_LVGL_OBJ_CHECKBOX:
        obj = lv_checkbox_create(parent);
        break;
    case LUA_LVGL_OBJ_SWITCH:
        obj = lv_switch_create(parent);
        break;
    case LUA_LVGL_OBJ_DROPDOWN:
        obj = lv_dropdown_create(parent);
        break;
    case LUA_LVGL_OBJ_ROLLER:
        obj = lv_roller_create(parent);
        break;
    case LUA_LVGL_OBJ_KEYBOARD:
        obj = lv_keyboard_create(parent);
        break;
    case LUA_LVGL_OBJ_LIST:
        obj = lv_list_create(parent);
        break;
    case LUA_LVGL_OBJ_TEXTAREA:
        obj = lv_textarea_create(parent);
        break;
    case LUA_LVGL_OBJ_TABLE:
        obj = lv_table_create(parent);
        break;
#if LV_USE_BUTTONMATRIX
    case LUA_LVGL_OBJ_BUTTONMATRIX:
        obj = lv_buttonmatrix_create(parent);
        break;
#endif
#if LV_USE_CALENDAR
    case LUA_LVGL_OBJ_CALENDAR:
        obj = lv_calendar_create(parent);
        break;
#endif
#if LV_USE_CANVAS
    case LUA_LVGL_OBJ_CANVAS:
        obj = lv_canvas_create(parent);
        break;
#endif
#if LV_USE_CHART
    case LUA_LVGL_OBJ_CHART:
        obj = lv_chart_create(parent);
        break;
#endif
#if LV_USE_IMAGEBUTTON
    case LUA_LVGL_OBJ_IMAGEBUTTON:
        obj = lv_imagebutton_create(parent);
        break;
#endif
#if LV_USE_LED
    case LUA_LVGL_OBJ_LED:
        obj = lv_led_create(parent);
        break;
#endif
#if LV_USE_MENU
    case LUA_LVGL_OBJ_MENU:
        obj = lv_menu_create(parent);
        break;
#endif
#if LV_USE_MSGBOX
    case LUA_LVGL_OBJ_MSGBOX:
        obj = lv_msgbox_create(parent);
        break;
#endif
#if LV_USE_SPAN
    case LUA_LVGL_OBJ_SPANGROUP:
        obj = lv_spangroup_create(parent);
        break;
#endif
#if LV_USE_SPINBOX
    case LUA_LVGL_OBJ_SPINBOX:
        obj = lv_spinbox_create(parent);
        break;
#endif
#if LV_USE_TABVIEW
    case LUA_LVGL_OBJ_TABVIEW:
        obj = lv_tabview_create(parent);
        break;
#endif
#if LV_USE_TILEVIEW
    case LUA_LVGL_OBJ_TILEVIEW:
        obj = lv_tileview_create(parent);
        break;
#endif
#if LV_USE_WIN
    case LUA_LVGL_OBJ_WINDOW:
        obj = lv_win_create(parent);
        break;
#endif
    default:
        obj = lv_obj_create(parent);
        break;
    }
    if (!obj) {
        lua_lvgl_unlock();
        free(options);
        free(line_points);
        return luaL_error(L, "lvgl object create failed");
    }

    if (opts.has_opts) {
        lua_lvgl_apply_common_opts_locked(obj, &opts);
        if (type == LUA_LVGL_OBJ_LABEL && opts.text) {
            lv_label_set_text(obj, opts.text);
        } else if (type == LUA_LVGL_OBJ_BUTTON && opts.text) {
            lv_obj_t *label = lv_label_create(obj);
            lv_label_set_text(label, opts.text);
            lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        } else if (type == LUA_LVGL_OBJ_CHECKBOX && opts.text) {
            lv_checkbox_set_text(obj, opts.text);
        } else if (type == LUA_LVGL_OBJ_DROPDOWN && opts.text) {
            lv_dropdown_set_text(obj, opts.text);
        } else if (type == LUA_LVGL_OBJ_TEXTAREA && opts.text) {
            lv_textarea_set_text(obj, opts.text);
        }
        if (type == LUA_LVGL_OBJ_BAR || type == LUA_LVGL_OBJ_SLIDER) {
            if (type == LUA_LVGL_OBJ_BAR) {
                lv_bar_set_range(obj, opts.min_value, opts.max_value);
                lv_bar_set_value(obj, opts.value, LV_ANIM_OFF);
            } else {
                lv_slider_set_range(obj, opts.min_value, opts.max_value);
                lv_slider_set_value(obj, opts.value, LV_ANIM_OFF);
            }
        }
        if (type == LUA_LVGL_OBJ_IMAGE && lua_lvgl_has_field(L, 2, "src")) {
            lv_image_set_src(obj, lua_lvgl_get_opt_string_field(L, 2, "src"));
        }
        if (type == LUA_LVGL_OBJ_LINE) {
            bool y_invert = lua_lvgl_get_opt_bool_field(L, 2, "y_invert", false);

            lv_line_set_points(obj, line_points, line_point_count);
            lv_line_set_y_invert(obj, y_invert);
        }
        if (type == LUA_LVGL_OBJ_ARC) {
            lv_arc_mode_t mode;

            lv_arc_set_range(obj, opts.min_value, opts.max_value);
            lv_arc_set_value(obj, opts.value);
            if (lua_lvgl_has_field(L, 2, "start_angle") || lua_lvgl_has_field(L, 2, "end_angle")) {
                lv_arc_set_angles(obj,
                                  lua_lvgl_get_opt_int_field(L, 2, "start_angle", 135),
                                  lua_lvgl_get_opt_int_field(L, 2, "end_angle", 45));
            }
            if (lua_lvgl_has_field(L, 2, "bg_start_angle") || lua_lvgl_has_field(L, 2, "bg_end_angle")) {
                lv_arc_set_bg_angles(obj,
                                     lua_lvgl_get_opt_int_field(L, 2, "bg_start_angle", 135),
                                     lua_lvgl_get_opt_int_field(L, 2, "bg_end_angle", 45));
            }
            if (lua_lvgl_has_field(L, 2, "rotation")) {
                lv_arc_set_rotation(obj, lua_lvgl_get_opt_int_field(L, 2, "rotation", 0));
            }
            if (lua_lvgl_has_field(L, 2, "mode")) {
                if (lua_lvgl_parse_arc_mode(lua_lvgl_get_opt_string_field(L, 2, "mode"), &mode) != ESP_OK) {
                    lv_obj_delete(obj);
                    lua_lvgl_unlock();
                    free(options);
                    free(line_points);
                    return luaL_error(L, "lvgl arc mode must be normal, symmetrical, or reverse");
                }
                lv_arc_set_mode(obj, mode);
            }
        }
        if (type == LUA_LVGL_OBJ_SPINNER) {
            if (lua_lvgl_has_field(L, 2, "anim_ms")) {
                lv_spinner_set_anim_duration(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "anim_ms", 1000));
            }
            if (lua_lvgl_has_field(L, 2, "arc_sweep")) {
                lv_spinner_set_arc_sweep(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "arc_sweep", 60));
            }
        }
        if (type == LUA_LVGL_OBJ_SCALE) {
            lv_scale_mode_t mode;

            lv_scale_set_range(obj, opts.min_value, opts.max_value);
            if (lua_lvgl_has_field(L, 2, "mode")) {
                if (lua_lvgl_parse_scale_mode(lua_lvgl_get_opt_string_field(L, 2, "mode"), &mode) != ESP_OK) {
                    lv_obj_delete(obj);
                    lua_lvgl_unlock();
                    free(options);
                    free(line_points);
                    return luaL_error(L, "lvgl scale mode is invalid");
                }
                lv_scale_set_mode(obj, mode);
            }
            if (lua_lvgl_has_field(L, 2, "total_ticks")) {
                lv_scale_set_total_tick_count(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "total_ticks", 11));
            }
            if (lua_lvgl_has_field(L, 2, "major_tick_every")) {
                lv_scale_set_major_tick_every(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "major_tick_every", 5));
            }
            if (lua_lvgl_has_field(L, 2, "label_show")) {
                lv_scale_set_label_show(obj, lua_lvgl_get_opt_bool_field(L, 2, "label_show", true));
            }
            if (lua_lvgl_has_field(L, 2, "angle_range")) {
                lv_scale_set_angle_range(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "angle_range", 270));
            }
            if (lua_lvgl_has_field(L, 2, "rotation")) {
                lv_scale_set_rotation(obj, lua_lvgl_get_opt_int_field(L, 2, "rotation", 0));
            }
        }
        if (type == LUA_LVGL_OBJ_CHECKBOX || type == LUA_LVGL_OBJ_SWITCH) {
            if (lua_lvgl_get_opt_bool_field(L, 2, "checked", false)) {
                lv_obj_add_state(obj, LV_STATE_CHECKED);
            }
        }
        if (type == LUA_LVGL_OBJ_DROPDOWN) {
            lv_dir_t dir;

            if (options) {
                lv_dropdown_set_options(obj, options);
            }
            if (lua_lvgl_has_field(L, 2, "selected")) {
                int selected = lua_lvgl_get_opt_int_field(L, 2, "selected", 1);
                lv_dropdown_set_selected(obj, selected > 0 ? (uint16_t)(selected - 1) : 0);
            }
            if (lua_lvgl_has_field(L, 2, "dir")) {
                if (lua_lvgl_parse_dir(lua_lvgl_get_opt_string_field(L, 2, "dir"), &dir) != ESP_OK) {
                    lv_obj_delete(obj);
                    lua_lvgl_unlock();
                    free(options);
                    free(line_points);
                    return luaL_error(L, "lvgl dropdown dir is invalid");
                }
                lv_dropdown_set_dir(obj, dir);
            }
            if (lua_lvgl_has_field(L, 2, "symbol")) {
                lv_dropdown_set_symbol(obj, lua_lvgl_get_opt_string_field(L, 2, "symbol"));
            }
        }
        if (type == LUA_LVGL_OBJ_ROLLER) {
            lv_roller_mode_t mode = LV_ROLLER_MODE_NORMAL;

            if (lua_lvgl_has_field(L, 2, "mode") &&
                    lua_lvgl_parse_roller_mode(lua_lvgl_get_opt_string_field(L, 2, "mode"), &mode) != ESP_OK) {
                lv_obj_delete(obj);
                lua_lvgl_unlock();
                free(options);
                free(line_points);
                return luaL_error(L, "lvgl roller mode must be normal or infinite");
            }
            if (options) {
                lv_roller_set_options(obj, options, mode);
            }
            if (lua_lvgl_has_field(L, 2, "selected")) {
                int selected = lua_lvgl_get_opt_int_field(L, 2, "selected", 1);
                lv_roller_set_selected(obj, selected > 0 ? (uint32_t)(selected - 1) : 0, LV_ANIM_OFF);
            }
            if (lua_lvgl_has_field(L, 2, "visible_rows")) {
                lv_roller_set_visible_row_count(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "visible_rows", 3));
            }
        }
        if (type == LUA_LVGL_OBJ_KEYBOARD) {
            lv_keyboard_mode_t mode;

            if (lua_lvgl_has_field(L, 2, "mode")) {
                if (lua_lvgl_parse_keyboard_mode(lua_lvgl_get_opt_string_field(L, 2, "mode"), &mode) != ESP_OK) {
                    lv_obj_delete(obj);
                    lua_lvgl_unlock();
                    free(options);
                    free(line_points);
                    return luaL_error(L, "lvgl keyboard mode is invalid");
                }
                lv_keyboard_set_mode(obj, mode);
            }
            if (lua_lvgl_has_field(L, 2, "popovers")) {
                lv_keyboard_set_popovers(obj, lua_lvgl_get_opt_bool_field(L, 2, "popovers", true));
            }
            lua_getfield(L, 2, "textarea");
            if (!lua_isnil(L, -1)) {
                lua_lvgl_obj_ud_t *textarea_ud = lua_lvgl_check_ud(L, -1);
                lua_lvgl_obj_type_t textarea_type;
                lv_obj_t *textarea = lua_lvgl_validate_ud_locked(textarea_ud, &textarea_type, &obj_error);

                if (!textarea || textarea_type != LUA_LVGL_OBJ_TEXTAREA) {
                    lua_pop(L, 1);
                    lv_obj_delete(obj);
                    lua_lvgl_unlock();
                    free(options);
                    free(line_points);
                    return luaL_error(L, "lvgl keyboard textarea must be a textarea object");
                }
                lv_keyboard_set_textarea(obj, textarea);
            }
            lua_pop(L, 1);
        }
        if (type == LUA_LVGL_OBJ_TEXTAREA) {
            if (lua_lvgl_has_field(L, 2, "placeholder")) {
                lv_textarea_set_placeholder_text(obj, lua_lvgl_get_opt_string_field(L, 2, "placeholder"));
            }
            if (lua_lvgl_has_field(L, 2, "one_line")) {
                lv_textarea_set_one_line(obj, lua_lvgl_get_opt_bool_field(L, 2, "one_line", false));
            }
            if (lua_lvgl_has_field(L, 2, "password")) {
                lv_textarea_set_password_mode(obj, lua_lvgl_get_opt_bool_field(L, 2, "password", false));
            }
            if (lua_lvgl_has_field(L, 2, "max_length")) {
                lv_textarea_set_max_length(obj, (uint32_t)lua_lvgl_get_opt_int_field(L, 2, "max_length", 0));
            }
            if (lua_lvgl_has_field(L, 2, "accepted_chars")) {
                lv_textarea_set_accepted_chars(obj, lua_lvgl_get_opt_string_field(L, 2, "accepted_chars"));
            }
        }
        if (type == LUA_LVGL_OBJ_TABLE) {
            int rows = lua_lvgl_get_opt_int_field(L, 2, "rows", 0);
            int cols = lua_lvgl_get_opt_int_field(L, 2, "cols", 0);

            if (rows > 0) {
                lv_table_set_row_count(obj, (uint32_t)rows);
            }
            if (cols > 0) {
                lv_table_set_column_count(obj, (uint32_t)cols);
            }
            lua_getfield(L, 2, "cells");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                for (size_t r = 1; r <= lua_rawlen(L, -1); r++) {
                    lua_rawgeti(L, -1, (lua_Integer)r);
                    luaL_checktype(L, -1, LUA_TTABLE);
                    for (size_t c = 1; c <= lua_rawlen(L, -1); c++) {
                        lua_rawgeti(L, -1, (lua_Integer)c);
                        if (!lua_isnil(L, -1)) {
                            lv_table_set_cell_value(obj, (uint32_t)(r - 1), (uint32_t)(c - 1), luaL_checkstring(L, -1));
                        }
                        lua_pop(L, 1);
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
            lua_getfield(L, 2, "column_widths");
            if (!lua_isnil(L, -1)) {
                luaL_checktype(L, -1, LUA_TTABLE);
                for (size_t c = 1; c <= lua_rawlen(L, -1); c++) {
                    lua_rawgeti(L, -1, (lua_Integer)c);
                    lv_table_set_column_width(obj, (uint32_t)(c - 1), (int32_t)luaL_checkinteger(L, -1));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_lvgl_apply_style_opts_locked(L, 2, obj);
    }

    created_ud = lua_lvgl_push_obj(L, obj, type);
    if (!created_ud) {
        lv_obj_delete(obj);
        lua_lvgl_unlock();
        free(options);
        free(line_points);
        return luaL_error(L, "lvgl object record allocation failed");
    }
    if (created_ud->record) {
        created_ud->record->line_points = line_points;
        created_ud->record->line_point_count = line_point_count;
        created_ud->record->value_cache = opts.value;
        if (type == LUA_LVGL_OBJ_BUTTON) {
            created_ud->record->aux_obj = lv_obj_get_child(obj, 0);
        }
    }
    if (opts.has_opts) {
        lua_lvgl_apply_complex_widget_opts(L, created_ud, type);
    }
    free(options);
    lua_lvgl_unlock();
    return 1;
}

static int lua_lvgl_label(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_LABEL);
}

static int lua_lvgl_object(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_GENERIC);
}

static int lua_lvgl_container(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_CONTAINER);
}

static int lua_lvgl_button(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_BUTTON);
}

static int lua_lvgl_bar(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_BAR);
}

static int lua_lvgl_slider(lua_State *L)
{
    return lua_lvgl_create_widget(L, LUA_LVGL_OBJ_SLIDER);
}

/* Only widget factories and screen-related runtime helpers are registered on
 * the `lvgl` module table. Object operations (including screen:load()) are
 * registered as methods in lua_lvgl_methods.c. */
const luaL_Reg lua_lvgl_core_widget_funcs[] = {
    {"screen", lua_lvgl_screen},
    {"create_screen", lua_lvgl_create_screen},
    {"object", lua_lvgl_object},
    {"container", lua_lvgl_container},
    {"label", lua_lvgl_label},
    {"button", lua_lvgl_button},
    {"bar", lua_lvgl_bar},
    {"slider", lua_lvgl_slider},
    {NULL, NULL},
};
