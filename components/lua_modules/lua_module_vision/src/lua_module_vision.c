/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_vision.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "lua.h"
#include "sdkconfig.h"

#if CONFIG_LUA_MODULE_VISION_ESPDET
#include "espdet.h"
#endif
#if CONFIG_LUA_MODULE_VISION_MOTION_DETECT
#include "lua_image.h"
#include "motion_detect.h"
#endif

#if CONFIG_LUA_MODULE_VISION_MOTION_DETECT
#define LUA_MOTION_DETECT_MT "motion_detect.detector"

static const char *TAG = "lua_vision";

typedef struct {
    motion_detect_handle_t handle;
    motion_detect_config_t config;
} lua_motion_detector_t;

static bool lua_motion_get_table_integer(lua_State *L, int table_idx, const char *name, int *out)
{
    bool found = false;
    table_idx = lua_absindex(L, table_idx);
    lua_getfield(L, table_idx, name);
    if (lua_isinteger(L, -1) || lua_isnumber(L, -1)) {
        lua_Integer value = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : (lua_Integer)lua_tonumber(L, -1);
        if (value < INT_MIN || value > INT_MAX) {
            luaL_error(L, "motion_detect option '%s' is outside the integer range", name);
        }
        *out = (int)value;
        found = true;
    }
    lua_pop(L, 1);
    return found;
}

static void lua_motion_parse_roi(lua_State *L, int opts_idx, motion_detect_config_t *config)
{
    opts_idx = lua_absindex(L, opts_idx);
    lua_getfield(L, opts_idx, "roi");
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_motion_get_table_integer(L, -1, "x", &config->roi_x);
        lua_motion_get_table_integer(L, -1, "y", &config->roi_y);
        lua_motion_get_table_integer(L, -1, "width", &config->roi_width);
        lua_motion_get_table_integer(L, -1, "height", &config->roi_height);
    }
    lua_pop(L, 1);
}

static void lua_motion_parse_config(lua_State *L, int opts_idx, motion_detect_config_t *config)
{
    if (opts_idx <= 0 || lua_isnoneornil(L, opts_idx)) {
        return;
    }
    luaL_checktype(L, opts_idx, LUA_TTABLE);
    opts_idx = lua_absindex(L, opts_idx);

    lua_motion_parse_roi(L, opts_idx, config);
    lua_motion_get_table_integer(L, opts_idx, "pixel_diff_threshold", &config->pixel_diff_threshold);
    lua_motion_get_table_integer(L, opts_idx, "active_pixel_percent", &config->active_pixel_percent);
    lua_motion_get_table_integer(L, opts_idx, "confirm_frames", &config->confirm_frames);
    lua_motion_get_table_integer(L, opts_idx, "hold_frames", &config->hold_frames);
    lua_motion_get_table_integer(L, opts_idx, "block_size", &config->block_size);
    lua_motion_get_table_integer(L, opts_idx, "block_hit_pixels", &config->block_hit_pixels);
    lua_motion_get_table_integer(L, opts_idx, "box_padding", &config->box_padding);
    lua_motion_get_table_integer(L, opts_idx, "box_deadband", &config->box_deadband);
    lua_motion_get_table_integer(L, opts_idx, "box_snap_threshold", &config->box_snap_threshold);
}

static void lua_motion_validate_config(lua_State *L, const motion_detect_config_t *config)
{
    if (config->pixel_diff_threshold < 0 || config->pixel_diff_threshold > 255) {
        ESP_LOGE(TAG, "invalid motion pixel_diff_threshold=%d", config->pixel_diff_threshold);
        luaL_error(L, "motion_detect pixel_diff_threshold must be in [0, 255]");
    }
    if (config->active_pixel_percent < 1 || config->active_pixel_percent > 100) {
        ESP_LOGE(TAG, "invalid motion active_pixel_percent=%d", config->active_pixel_percent);
        luaL_error(L, "motion_detect active_pixel_percent must be in [1, 100]");
    }
    if (config->confirm_frames < 1) {
        ESP_LOGE(TAG, "invalid motion confirm_frames=%d", config->confirm_frames);
        luaL_error(L, "motion_detect confirm_frames must be >= 1");
    }
    if (config->hold_frames < 0) {
        ESP_LOGE(TAG, "invalid motion hold_frames=%d", config->hold_frames);
        luaL_error(L, "motion_detect hold_frames must be >= 0");
    }
    if (config->block_size < 1 || config->block_size > 255) {
        ESP_LOGE(TAG, "invalid motion block_size=%d", config->block_size);
        luaL_error(L, "motion_detect block_size must be in [1, 255]");
    }
    if (config->block_hit_pixels < 1 || config->block_hit_pixels > 255) {
        ESP_LOGE(TAG, "invalid motion block_hit_pixels=%d", config->block_hit_pixels);
        luaL_error(L, "motion_detect block_hit_pixels must be in [1, 255]");
    }
    if (config->box_padding < 0 || config->box_deadband < 0 || config->box_snap_threshold < 0) {
        ESP_LOGE(TAG, "invalid motion box settings: padding=%d deadband=%d snap=%d",
                 config->box_padding, config->box_deadband, config->box_snap_threshold);
        luaL_error(L, "motion_detect box padding/deadband/snap values must be >= 0");
    }
}

static lua_motion_detector_t *lua_motion_check_detector(lua_State *L, int index)
{
    return (lua_motion_detector_t *)luaL_checkudata(L, index, LUA_MOTION_DETECT_MT);
}

static const char *lua_motion_event_name(motion_detect_event_t event)
{
    switch (event) {
    case MOTION_DETECT_EVENT_ACTIVATED:
        return "started";
    case MOTION_DETECT_EVENT_CLEARED:
        return "stopped";
    case MOTION_DETECT_EVENT_NONE:
    default:
        return "none";
    }
}

static void lua_motion_push_box(lua_State *L, int x1, int y1, int x2, int y2)
{
    lua_newtable(L);
    lua_pushinteger(L, x1);
    lua_setfield(L, -2, "left");
    lua_pushinteger(L, y1);
    lua_setfield(L, -2, "top");
    lua_pushinteger(L, x2);
    lua_setfield(L, -2, "right");
    lua_pushinteger(L, y2);
    lua_setfield(L, -2, "bottom");
    lua_pushinteger(L, x1);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, y1);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, ((lua_Number)x1 + (lua_Number)x2) / 2.0);
    lua_setfield(L, -2, "cx");
    lua_pushnumber(L, ((lua_Number)y1 + (lua_Number)y2) / 2.0);
    lua_setfield(L, -2, "cy");
    lua_pushinteger(L, x2 - x1 + 1);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, y2 - y1 + 1);
    lua_setfield(L, -2, "height");
}

static void lua_motion_push_result(lua_State *L, const motion_detect_result_t *result)
{
    uint32_t roi_pixels = (uint32_t)result->roi_width * (uint32_t)result->roi_height;

    lua_newtable(L);
    lua_pushboolean(L, result->has_previous);
    lua_setfield(L, -2, "ready");
    lua_pushboolean(L, result->alert_active);
    lua_setfield(L, -2, "motion");
    lua_pushstring(L, lua_motion_event_name(result->event));
    lua_setfield(L, -2, "event");
    lua_pushnumber(L, roi_pixels > 0 ? (lua_Number)result->active_pixels / (lua_Number)roi_pixels : 0);
    lua_setfield(L, -2, "score");

    if (result->has_display_box) {
        lua_motion_push_box(L, result->display_x1, result->display_y1,
                            result->display_x2, result->display_y2);
        lua_setfield(L, -2, "box");
    }
}

static int lua_motion_detector_detect(lua_State *L)
{
    lua_motion_detector_t *detector = lua_motion_check_detector(L, 1);
    lua_image_view_t view = {0};
    motion_detect_result_t result = {0};

    luaL_checkany(L, 2);
    if (!lua_isnoneornil(L, 3)) {
        ESP_LOGE(TAG, "motion detector detect got unexpected options argument");
        return luaL_error(L, "motion_detect detector:detect(frame) does not accept per-call options");
    }

    esp_err_t err = lua_image_require_format(L, 2, LUA_IMAGE_FORMAT_RGB565LE, &view);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "motion frame require failed: %s", esp_err_to_name(err));
        return luaL_error(L, "motion_detect expects an RGB565-capable image.frame: %s", esp_err_to_name(err));
    }
    err = motion_detect_process_rgb565(detector->handle, view.data, view.bytes,
                                       view.width, view.height, &detector->config, &result);
    lua_image_release_view(&view);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "motion detect failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_INVALID_ARG) {
            return luaL_error(L, "motion_detect invalid ROI or block settings");
        }
        return luaL_error(L, "motion_detect failed: %s", esp_err_to_name(err));
    }

    lua_motion_push_result(L, &result);
    return 1;
}

static lua_motion_detector_t *lua_motion_push_detector(lua_State *L, const motion_detect_config_t *config)
{
    lua_motion_detector_t *detector = (lua_motion_detector_t *)lua_newuserdata(L, sizeof(*detector));
    memset(detector, 0, sizeof(*detector));
    if (config != NULL) {
        detector->config = *config;
    } else {
        motion_detect_config_set_defaults(&detector->config);
    }
    luaL_getmetatable(L, LUA_MOTION_DETECT_MT);
    lua_setmetatable(L, -2);

    esp_err_t err = motion_detect_create(&detector->handle);
    if (err != ESP_OK) {
        luaL_error(L, "motion_detect create failed: %s", esp_err_to_name(err));
    }
    return detector;
}

static int lua_motion_detector_new(lua_State *L)
{
    motion_detect_config_t config;

    motion_detect_config_set_defaults(&config);
    lua_motion_parse_config(L, 1, &config);
    lua_motion_validate_config(L, &config);
    lua_motion_push_detector(L, &config);
    return 1;
}

static int lua_motion_detector_reset(lua_State *L)
{
    motion_detect_reset(lua_motion_check_detector(L, 1)->handle);
    return 0;
}

static int lua_motion_detector_close(lua_State *L)
{
    motion_detect_close(lua_motion_check_detector(L, 1)->handle);
    return 0;
}

static int lua_motion_detector_gc(lua_State *L)
{
    lua_motion_detector_t *detector = (lua_motion_detector_t *)luaL_checkudata(L, 1, LUA_MOTION_DETECT_MT);
    motion_detect_delete(detector->handle);
    detector->handle = NULL;
    return 0;
}

static void lua_motion_register_metatable(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MOTION_DETECT_MT)) {
        static const luaL_Reg methods[] = {
            {"detect", lua_motion_detector_detect},
            {"reset", lua_motion_detector_reset},
            {"close", lua_motion_detector_close},
            {NULL, NULL},
        };
        static const luaL_Reg metamethods[] = {
            {"__gc", lua_motion_detector_gc},
            {NULL, NULL},
        };
        lua_newtable(L);
        luaL_setfuncs(L, methods, 0);
        lua_setfield(L, -2, "__index");
        luaL_setfuncs(L, metamethods, 0);
    }
    lua_pop(L, 1);
}

int luaopen_motion_detect(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"new", lua_motion_detector_new},
        {NULL, NULL},
    };
    lua_motion_register_metatable(L);
    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}
#endif

esp_err_t lua_module_vision_register(void)
{
#if !CONFIG_LUA_MODULE_VISION_MOTION_DETECT && !CONFIG_LUA_MODULE_VISION_ESPDET && !CONFIG_LUA_MODULE_VISION_COLOR_DETECT
    return ESP_OK;
#else
    static const cap_lua_module_t modules[] = {
#if CONFIG_LUA_MODULE_VISION_MOTION_DETECT
        {"motion_detect", luaopen_motion_detect},
#endif
#if CONFIG_LUA_MODULE_VISION_ESPDET
        {"espdet", luaopen_espdet},
#endif
#if CONFIG_LUA_MODULE_VISION_COLOR_DETECT
        {"color_detect", luaopen_color_detect_dl},
#endif
    };
    return cap_lua_register_modules(modules, sizeof(modules) / sizeof(modules[0]));
#endif
}
