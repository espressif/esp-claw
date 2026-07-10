/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lua_module_lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "display_arbiter.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lvgl.h"

#ifdef __EMSCRIPTEN__
#ifndef LV_CHART_TYPE_CURVE
#define LV_CHART_TYPE_CURVE LV_CHART_TYPE_LINE
#endif
#ifndef LV_CHART_TYPE_STACKED
#define LV_CHART_TYPE_STACKED LV_CHART_TYPE_BAR
#endif
static inline void lua_lvgl_web_spinner_set_anim_duration(lv_obj_t *obj, uint32_t duration)
{
    (void)obj;
    (void)duration;
}
static inline void lua_lvgl_web_spinner_set_arc_sweep(lv_obj_t *obj, uint32_t sweep)
{
    (void)obj;
    (void)sweep;
}
static inline void lua_lvgl_web_tabview_set_tab_text(lv_obj_t *tv, uint32_t index, const char *text)
{
    (void)tv;
    (void)index;
    (void)text;
}
#define lv_spinner_set_anim_duration lua_lvgl_web_spinner_set_anim_duration
#define lv_spinner_set_arc_sweep lua_lvgl_web_spinner_set_arc_sweep
#define lv_tabview_set_tab_text lua_lvgl_web_tabview_set_tab_text
#endif

#define LUA_MODULE_LVGL_NAME "lvgl"
#define LUA_MODULE_LVGL_DEFAULT_BUFFER_LINES 40
#define LUA_MODULE_LVGL_DEFAULT_TICK_MS 5
#define LUA_MODULE_LVGL_DEFAULT_TASK_PERIOD_MS 10
#define LUA_MODULE_LVGL_TASK_STACK 8192
#define LUA_MODULE_LVGL_TASK_PRIO 5
#define LUA_MODULE_LVGL_TASK_STOP_TIMEOUT_MS 5000
#define LUA_MODULE_LVGL_PANEL_IF_IO 0
#define LUA_MODULE_LVGL_PANEL_IF_RGB 1
#define LUA_MODULE_LVGL_PANEL_IF_MIPI_DSI 2
#define LUA_MODULE_LVGL_FS_LETTER 'D'
#define LUA_MODULE_LVGL_PATH_MAX 256
#define LUA_LVGL_FONT_MT "lvgl.font"

typedef enum {
    LUA_LVGL_OBJ_GENERIC = 0,
    LUA_LVGL_OBJ_SCREEN,
    LUA_LVGL_OBJ_CONTAINER,
    LUA_LVGL_OBJ_LABEL,
    LUA_LVGL_OBJ_BUTTON,
    LUA_LVGL_OBJ_BAR,
    LUA_LVGL_OBJ_SLIDER,
    LUA_LVGL_OBJ_IMAGE,
    LUA_LVGL_OBJ_LINE,
    LUA_LVGL_OBJ_ARC,
    LUA_LVGL_OBJ_SPINNER,
    LUA_LVGL_OBJ_SCALE,
    LUA_LVGL_OBJ_CHECKBOX,
    LUA_LVGL_OBJ_SWITCH,
    LUA_LVGL_OBJ_DROPDOWN,
    LUA_LVGL_OBJ_ROLLER,
    LUA_LVGL_OBJ_KEYBOARD,
    LUA_LVGL_OBJ_LIST,
    LUA_LVGL_OBJ_LIST_TEXT,
    LUA_LVGL_OBJ_LIST_BUTTON,
    LUA_LVGL_OBJ_TEXTAREA,
    LUA_LVGL_OBJ_TABLE,
    LUA_LVGL_OBJ_BUTTONMATRIX,
    LUA_LVGL_OBJ_CALENDAR,
    LUA_LVGL_OBJ_CANVAS,
    LUA_LVGL_OBJ_CHART,
    LUA_LVGL_OBJ_IMAGEBUTTON,
    LUA_LVGL_OBJ_LED,
    LUA_LVGL_OBJ_MENU,
    LUA_LVGL_OBJ_MENU_PAGE,
    LUA_LVGL_OBJ_MENU_CONT,
    LUA_LVGL_OBJ_MENU_SECTION,
    LUA_LVGL_OBJ_MENU_SEPARATOR,
    LUA_LVGL_OBJ_MSGBOX,
    LUA_LVGL_OBJ_MSGBOX_CHILD,
    LUA_LVGL_OBJ_SPANGROUP,
    LUA_LVGL_OBJ_SPINBOX,
    LUA_LVGL_OBJ_TABVIEW,
    LUA_LVGL_OBJ_TAB_PAGE,
    LUA_LVGL_OBJ_TILEVIEW,
    LUA_LVGL_OBJ_TILE,
    LUA_LVGL_OBJ_WINDOW,
    LUA_LVGL_OBJ_WINDOW_CHILD,
} lua_lvgl_obj_type_t;

/* Forward declaration so lua_lvgl_event_sub_t can hold a back-pointer to
 * its owning record. */
struct lua_lvgl_obj_record;

/* A single LVGL event subscription created by `obj:on(event_name, cb)`.
 *
 * Lifecycle and ownership are documented in RFC §4.2; the short version is:
 *   - linked into record->events while logically registered
 *   - linked into s_lvgl.event_queue while waiting for dispatch
 *   - `queued` coalesces repeated fires while still pending
 *   - `dead` is set when off() / record release happens while the sub is
 *     queued, so process_events frees the sub on dequeue rather than racing
 */
typedef struct lua_lvgl_event_sub {
    int callback_ref;        /* LUA_NOREF after release */
    lv_event_code_t code;
    struct lua_lvgl_obj_record *record;
    struct lua_lvgl_event_sub *next;       /* in record->events */
    struct lua_lvgl_event_sub *queue_next; /* in s_lvgl.event_queue */
    bool queued;
    bool dead;
} lua_lvgl_event_sub_t;

/* A registry ref deferred for unref so it can always be released on the
 * script task (luaL_unref is not safe to call from the LVGL task because
 * it touches the lua_State registry). */
typedef struct lua_lvgl_pending_unref {
    int ref;
    struct lua_lvgl_pending_unref *next;
} lua_lvgl_pending_unref_t;

typedef struct lua_lvgl_obj_record {
    lv_obj_t *obj;
    lv_obj_t *aux_obj;
    lv_point_precise_t *line_points;
    uint32_t line_point_count;
    int32_t *grid_cols;
    int32_t *grid_rows;
    char **string_array;
    size_t string_array_count;
    void *data;
    void *data2;
    size_t data_size;
    size_t data2_size;
    int value_cache;
    uint32_t generation;
    lua_lvgl_obj_type_t type;
    bool valid;
    lua_lvgl_event_sub_t *events;
    struct lua_lvgl_obj_record *next;
} lua_lvgl_obj_record_t;

typedef struct {
    lua_lvgl_obj_record_t *record;
} lua_lvgl_obj_ud_t;

typedef struct lua_lvgl_font_ud lua_lvgl_font_ud_t;

typedef struct lua_lvgl_font_record {
    lv_font_t *font;
    lua_lvgl_font_ud_t *ud;
    uint32_t generation;
    bool valid;
    struct lua_lvgl_font_record *next;
} lua_lvgl_font_record_t;

struct lua_lvgl_font_ud {
    lua_lvgl_font_record_t *record;
};

typedef struct {
    SemaphoreHandle_t mutex;
    bool lvgl_initialized;
    bool runtime_initialized;
    bool display_owner_acquired;
    volatile bool task_stop;
    TaskHandle_t task_handle;
    TaskHandle_t task_waiter;
    esp_timer_handle_t tick_timer;
    SemaphoreHandle_t flush_done;
    lv_display_t *display;
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    int width;
    int height;
    int panel_if;
    uint32_t generation;
    uint32_t tick_ms;
    uint32_t task_period_ms;
    void *draw_buf;
    size_t draw_buf_size;
    bool flush_callbacks_registered;
    volatile bool flush_pending;
    lua_State *runtime_owner;
    lua_lvgl_obj_record_t *records;
    lua_lvgl_event_sub_t *event_queue_head;
    lua_lvgl_event_sub_t *event_queue_tail;
    lua_lvgl_pending_unref_t *pending_unrefs;
    lua_lvgl_font_record_t *fonts;
    lv_fs_drv_t fs_drv;
    char data_root[LUA_MODULE_LVGL_PATH_MAX];
    bool fs_registered;
    /* P4: input devices. Only one indev of each kind is supported on a
     * single-script runtime; the underlying esp_lcd_touch_handle_t is owned
     * by board_manager, so we only borrow the pointer here and never free
     * it when the LVGL runtime is torn down. */
    lv_indev_t *touch_indev;
    void *touch_handle;
} lua_lvgl_state_t;

typedef struct {
    bool has_opts;
    const char *text;
    const char *align_value;
    int x;
    int y;
    int w;
    int h;
    int min_value;
    int max_value;
    int value;
} lua_lvgl_opts_t;

extern lua_lvgl_state_t s_lvgl;

esp_err_t lua_lvgl_lock(void);
void lua_lvgl_unlock(void);
int lua_lvgl_error_esp(lua_State *L, const char *what, esp_err_t err);

lua_lvgl_obj_ud_t *lua_lvgl_check_ud(lua_State *L, int index);
void lua_lvgl_record_release_resources(lua_lvgl_obj_record_t *record);
lua_lvgl_obj_ud_t *lua_lvgl_push_obj(lua_State *L, lv_obj_t *obj, lua_lvgl_obj_type_t type);
lv_obj_t *lua_lvgl_validate_ud_locked(const lua_lvgl_obj_ud_t *ud,
                                      lua_lvgl_obj_type_t *out_type,
                                      const char **out_error);
void lua_lvgl_invalidate_records_locked(void);
int lua_lvgl_obj_gc(lua_State *L);

/* Method-style API support: build all per-type metatables under
 * "lvgl.obj.<type>" and resolve type->mt-name. See lua_lvgl_methods.c. */
const char *lua_lvgl_metatable_for_type(lua_lvgl_obj_type_t type);
void lua_lvgl_register_metatables(lua_State *L);

/* Per-widget method implementations (also reachable as plain C functions
 * because method tables only re-export them as Lua methods). All listed
 * here are non-static so the metatable wiring in lua_lvgl_methods.c can
 * compose method tables across translation units. */

/* lua_lvgl_value.c */
int lua_lvgl_set_text(lua_State *L);
int lua_lvgl_get_pos(lua_State *L);
int lua_lvgl_get_size(lua_State *L);
int lua_lvgl_get_value(lua_State *L);
int lua_lvgl_is_valid(lua_State *L);
int lua_lvgl_set_value(lua_State *L);
int lua_lvgl_set_range(lua_State *L);
int lua_lvgl_set_pos(lua_State *L);
int lua_lvgl_set_size(lua_State *L);
int lua_lvgl_align(lua_State *L);

/* lua_lvgl_style.c */
int lua_lvgl_set_style(lua_State *L);

/* lua_lvgl_font.c */
esp_err_t lua_lvgl_set_data_root(const char *data_root);
esp_err_t lua_lvgl_register_fs_locked(void);
void lua_lvgl_register_font_metatable(lua_State *L);
void lua_lvgl_release_fonts_locked(void);
lua_lvgl_font_ud_t *lua_lvgl_check_font(lua_State *L, int index);
lv_font_t *lua_lvgl_validate_font_locked(const lua_lvgl_font_ud_t *ud, const char **out_error);
void lua_lvgl_apply_font_style_field(lua_State *L, int index, lv_obj_t *obj);
extern const luaL_Reg lua_lvgl_font_module_funcs[];

/* lua_lvgl_layout.c */
int lua_lvgl_set_flex(lua_State *L);
int lua_lvgl_set_grid(lua_State *L);
int lua_lvgl_set_grid_cell(lua_State *L);
int lua_lvgl_set_scroll(lua_State *L);

/* lua_lvgl_object.c */
int lua_lvgl_delete(lua_State *L);
int lua_lvgl_clean(lua_State *L);

/* lua_lvgl_core_widgets.c */
int lua_lvgl_screen_load(lua_State *L);

/* lua_lvgl_extra_widgets.c */
int lua_lvgl_list_add_text(lua_State *L);
int lua_lvgl_list_add_button(lua_State *L);
int lua_lvgl_table_set_cell(lua_State *L);
int lua_lvgl_table_get_cell(lua_State *L);

/* lua_lvgl_complex_widgets.c */
int lua_lvgl_buttonmatrix_set_map(lua_State *L);
int lua_lvgl_buttonmatrix_set_selected(lua_State *L);
int lua_lvgl_buttonmatrix_get_selected(lua_State *L);
int lua_lvgl_buttonmatrix_get_button_text(lua_State *L);
int lua_lvgl_buttonmatrix_set_one_checked(lua_State *L);
int lua_lvgl_calendar_set_today(lua_State *L);
int lua_lvgl_calendar_set_shown(lua_State *L);
int lua_lvgl_calendar_set_highlighted(lua_State *L);
int lua_lvgl_calendar_get_pressed_date(lua_State *L);
int lua_lvgl_canvas_fill_bg(lua_State *L);
int lua_lvgl_canvas_set_px(lua_State *L);
int lua_lvgl_canvas_get_px(lua_State *L);
int lua_lvgl_chart_add_series(lua_State *L);
int lua_lvgl_chart_set_type(lua_State *L);
int lua_lvgl_chart_set_point_count(lua_State *L);
int lua_lvgl_chart_set_range(lua_State *L);
int lua_lvgl_chart_set_next_value(lua_State *L);
int lua_lvgl_chart_set_series_values(lua_State *L);
int lua_lvgl_chart_refresh(lua_State *L);
int lua_lvgl_imagebutton_set_src(lua_State *L);
int lua_lvgl_imagebutton_set_state(lua_State *L);
int lua_lvgl_led_set_color(lua_State *L);
int lua_lvgl_led_set_brightness(lua_State *L);
int lua_lvgl_led_get_brightness(lua_State *L);
int lua_lvgl_led_on(lua_State *L);
int lua_lvgl_led_off(lua_State *L);
int lua_lvgl_led_toggle(lua_State *L);
int lua_lvgl_menu_page(lua_State *L);
int lua_lvgl_menu_cont(lua_State *L);
int lua_lvgl_menu_section(lua_State *L);
int lua_lvgl_menu_separator(lua_State *L);
int lua_lvgl_menu_set_page(lua_State *L);
int lua_lvgl_menu_set_sidebar_page(lua_State *L);
int lua_lvgl_menu_set_mode_header(lua_State *L);
int lua_lvgl_menu_set_root_back_button(lua_State *L);
int lua_lvgl_menu_clear_history(lua_State *L);
int lua_lvgl_msgbox_add_title(lua_State *L);
int lua_lvgl_msgbox_add_text(lua_State *L);
int lua_lvgl_msgbox_add_footer_button(lua_State *L);
int lua_lvgl_msgbox_add_close_button(lua_State *L);
int lua_lvgl_msgbox_close(lua_State *L);
int lua_lvgl_msgbox_close_async(lua_State *L);
int lua_lvgl_spangroup_add_span(lua_State *L);
int lua_lvgl_spangroup_get_span_count(lua_State *L);
int lua_lvgl_spangroup_refresh(lua_State *L);
int lua_lvgl_spinbox_set_step(lua_State *L);
int lua_lvgl_spinbox_get_step(lua_State *L);
int lua_lvgl_spinbox_increment(lua_State *L);
int lua_lvgl_spinbox_decrement(lua_State *L);
int lua_lvgl_spinbox_step_next(lua_State *L);
int lua_lvgl_spinbox_step_prev(lua_State *L);
int lua_lvgl_tabview_add_tab(lua_State *L);
int lua_lvgl_tabview_set_active(lua_State *L);
int lua_lvgl_tabview_get_active(lua_State *L);
int lua_lvgl_tabview_get_tab_count(lua_State *L);
int lua_lvgl_tabview_set_tab_text(lua_State *L);
int lua_lvgl_tileview_add_tile(lua_State *L);
int lua_lvgl_tileview_set_tile(lua_State *L);
int lua_lvgl_tileview_set_tile_by_index(lua_State *L);
int lua_lvgl_tileview_get_active_tile(lua_State *L);
int lua_lvgl_window_add_title(lua_State *L);
int lua_lvgl_window_add_button(lua_State *L);
int lua_lvgl_window_get_header(lua_State *L);
int lua_lvgl_window_get_content(lua_State *L);
int lua_lvgl_span_set_text(lua_State *L);
int lua_lvgl_span_get_text(lua_State *L);
int lua_lvgl_span_set_style(lua_State *L);
int lua_lvgl_span_delete(lua_State *L);
int lua_lvgl_span_gc(lua_State *L);
int lua_lvgl_chart_series_gc(lua_State *L);
void lua_lvgl_register_handle_metatables(lua_State *L);
void lua_lvgl_apply_complex_widget_opts(lua_State *L, lua_lvgl_obj_ud_t *ud, lua_lvgl_obj_type_t type);

/* lua_lvgl_events.c */
int lua_lvgl_obj_on(lua_State *L);
int lua_lvgl_obj_off(lua_State *L);
/* Free a sub linked into record->events: detach from LVGL, defer unref of
 * its callback ref via pending_unrefs (or mark dead if currently queued).
 * Caller must hold lua_lvgl_lock(). */
void lua_lvgl_record_release_events_locked(lua_lvgl_obj_record_t *record);
/* Append a registry ref to s_lvgl.pending_unrefs; ref must come from
 * luaL_ref(). Safe to call from LVGL task. Caller must hold lua_lvgl_lock(). */
void lua_lvgl_queue_pending_unref_locked(int ref);
/* Drain pending_unrefs by luaL_unref'ing each on L. Caller must hold
 * lua_lvgl_lock() and must be running on the script task. */
void lua_lvgl_drain_pending_unrefs_locked(lua_State *L);

int lua_lvgl_get_opt_int_field(lua_State *L, int index, const char *field, int default_value);
const char *lua_lvgl_get_opt_string_field(lua_State *L, int index, const char *field);
bool lua_lvgl_get_opt_bool_field(lua_State *L, int index, const char *field, bool default_value);
bool lua_lvgl_has_field(lua_State *L, int index, const char *field);
bool lua_lvgl_opt_table(lua_State *L, int index);
esp_err_t lua_lvgl_parse_color(lua_State *L, int index, lv_color_t *out_color);
esp_err_t lua_lvgl_parse_align(lua_State *L, const char *value, lv_align_t *out_align);
esp_err_t lua_lvgl_parse_flex_flow(const char *value, lv_flex_flow_t *out_flow);
esp_err_t lua_lvgl_parse_flex_align(const char *value, lv_flex_align_t *out_align);
esp_err_t lua_lvgl_parse_grid_align(const char *value, lv_grid_align_t *out_align);
esp_err_t lua_lvgl_parse_dir(const char *value, lv_dir_t *out_dir);
esp_err_t lua_lvgl_parse_scrollbar(const char *value, lv_scrollbar_mode_t *out_mode);
esp_err_t lua_lvgl_parse_scroll_snap(const char *value, lv_scroll_snap_t *out_snap);
esp_err_t lua_lvgl_parse_arc_mode(const char *value, lv_arc_mode_t *out_mode);
esp_err_t lua_lvgl_parse_scale_mode(const char *value, lv_scale_mode_t *out_mode);
esp_err_t lua_lvgl_parse_keyboard_mode(const char *value, lv_keyboard_mode_t *out_mode);
esp_err_t lua_lvgl_parse_roller_mode(const char *value, lv_roller_mode_t *out_mode);
void lua_lvgl_parse_opts(lua_State *L, int index, lua_lvgl_opts_t *opts);
void lua_lvgl_apply_common_opts_locked(lv_obj_t *obj, const lua_lvgl_opts_t *opts);
char *lua_lvgl_build_options_string(lua_State *L, int index, const char *field);
int32_t *lua_lvgl_build_grid_tracks(lua_State *L, int opts_index, const char *field);
lv_point_precise_t *lua_lvgl_build_line_points(lua_State *L, int opts_index, uint32_t *out_count);

void lua_lvgl_apply_style_opts_locked(lua_State *L, int index, lv_obj_t *obj);

esp_err_t lua_lvgl_deinit_runtime(void);
void lua_lvgl_exit_cleanup(lua_State *L);
int lua_lvgl_create_widget(lua_State *L, lua_lvgl_obj_type_t type);

/* Module-level function tables: only runtime + factories + event-loop
 * helpers are exposed on the `lvgl` module table. Object setters/getters
 * are exposed exclusively as methods through metatables built in
 * lua_lvgl_methods.c. */
extern const luaL_Reg lua_lvgl_runtime_funcs[];
extern const luaL_Reg lua_lvgl_core_widget_funcs[];
extern const luaL_Reg lua_lvgl_extra_widget_funcs[];
extern const luaL_Reg lua_lvgl_complex_widget_funcs[];
extern const luaL_Reg lua_lvgl_event_module_funcs[];
extern const luaL_Reg lua_lvgl_indev_module_funcs[];
extern const luaL_Reg lua_lvgl_demo_module_funcs[];

/* lua_lvgl_indev.c: tear down all currently registered indevs.
 * Caller must hold lua_lvgl_lock() and the LVGL task must already be
 * stopped (so no read_cb is running). */
void lua_lvgl_indev_release_locked(void);
