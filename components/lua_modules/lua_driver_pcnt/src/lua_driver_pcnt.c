/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_pcnt.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/pulse_cnt.h"
#include "esp_err.h"
#include "lauxlib.h"
#include "soc/soc_caps.h"

#define LUA_DRIVER_PCNT_METATABLE "pcnt.unit"
#define LUA_DRIVER_PCNT_DEFAULT_LOW_LIMIT (-32768)
#define LUA_DRIVER_PCNT_DEFAULT_HIGH_LIMIT 32767

typedef struct {
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t channels[SOC_PCNT_CHANNELS_PER_UNIT];
    int channel_count;
    bool enabled;
    bool running;
} lua_driver_pcnt_ud_t;

typedef struct {
    pcnt_chan_config_t chan_config;
    pcnt_channel_edge_action_t pos_edge;
    pcnt_channel_edge_action_t neg_edge;
    pcnt_channel_level_action_t high_level;
    pcnt_channel_level_action_t low_level;
} lua_driver_pcnt_channel_config_t;

static lua_driver_pcnt_ud_t *lua_driver_pcnt_get_ud(lua_State *L, int idx)
{
    lua_driver_pcnt_ud_t *ud =
        (lua_driver_pcnt_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_PCNT_METATABLE);
    if (!ud || !ud->unit) {
        luaL_error(L, "pcnt: invalid or closed unit");
    }
    return ud;
}

static bool lua_driver_pcnt_get_bool_field(lua_State *L, int table_idx, const char *name,
                                           bool default_value)
{
    bool value = default_value;

    lua_getfield(L, table_idx, name);
    if (!lua_isnil(L, -1)) {
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    return value;
}

static int lua_driver_pcnt_get_int_field(lua_State *L, int table_idx, const char *name,
                                         int default_value)
{
    int value = default_value;

    lua_getfield(L, table_idx, name);
    if (!lua_isnil(L, -1)) {
        value = (int)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);

    return value;
}

static bool lua_driver_pcnt_has_field(lua_State *L, int table_idx, const char *name)
{
    bool has_field;

    lua_getfield(L, table_idx, name);
    has_field = !lua_isnil(L, -1);
    lua_pop(L, 1);

    return has_field;
}

static pcnt_channel_edge_action_t lua_driver_pcnt_parse_edge_action(lua_State *L,
                                                                    const char *value)
{
    if (strcmp(value, "hold") == 0) {
        return PCNT_CHANNEL_EDGE_ACTION_HOLD;
    }
    if (strcmp(value, "increase") == 0) {
        return PCNT_CHANNEL_EDGE_ACTION_INCREASE;
    }
    if (strcmp(value, "decrease") == 0) {
        return PCNT_CHANNEL_EDGE_ACTION_DECREASE;
    }

    luaL_error(L, "pcnt edge action must be 'hold', 'increase', or 'decrease'");
    return PCNT_CHANNEL_EDGE_ACTION_HOLD;
}

static pcnt_channel_level_action_t lua_driver_pcnt_parse_level_action(lua_State *L,
                                                                      const char *value)
{
    if (strcmp(value, "keep") == 0) {
        return PCNT_CHANNEL_LEVEL_ACTION_KEEP;
    }
    if (strcmp(value, "inverse") == 0) {
        return PCNT_CHANNEL_LEVEL_ACTION_INVERSE;
    }
    if (strcmp(value, "hold") == 0) {
        return PCNT_CHANNEL_LEVEL_ACTION_HOLD;
    }

    luaL_error(L, "pcnt level action must be 'keep', 'inverse', or 'hold'");
    return PCNT_CHANNEL_LEVEL_ACTION_KEEP;
}

static pcnt_channel_edge_action_t lua_driver_pcnt_get_edge_action_field(lua_State *L,
                                                                        int table_idx,
                                                                        const char *name,
                                                                        const char *default_value)
{
    pcnt_channel_edge_action_t value;

    lua_getfield(L, table_idx, name);
    value = lua_driver_pcnt_parse_edge_action(
        L, lua_isnil(L, -1) ? default_value : luaL_checkstring(L, -1));
    lua_pop(L, 1);

    return value;
}

static pcnt_channel_level_action_t lua_driver_pcnt_get_level_action_field(lua_State *L,
                                                                          int table_idx,
                                                                          const char *name,
                                                                          const char *default_value)
{
    pcnt_channel_level_action_t value;

    lua_getfield(L, table_idx, name);
    value = lua_driver_pcnt_parse_level_action(
        L, lua_isnil(L, -1) ? default_value : luaL_checkstring(L, -1));
    lua_pop(L, 1);

    return value;
}

static bool lua_driver_pcnt_table_has_channel_config(lua_State *L, int table_idx)
{
    return lua_driver_pcnt_has_field(L, table_idx, "edge_gpio") ||
           lua_driver_pcnt_has_field(L, table_idx, "level_gpio");
}

static void lua_driver_pcnt_resolve_channel_config(lua_State *L,
                                                   int table_idx,
                                                   lua_driver_pcnt_channel_config_t *out_config)
{
    memset(out_config, 0, sizeof(*out_config));

    out_config->chan_config.edge_gpio_num =
        lua_driver_pcnt_get_int_field(L, table_idx, "edge_gpio", -1);
    out_config->chan_config.level_gpio_num =
        lua_driver_pcnt_get_int_field(L, table_idx, "level_gpio", -1);
    out_config->chan_config.flags.invert_edge_input =
        lua_driver_pcnt_get_bool_field(L, table_idx, "invert_edge", false);
    out_config->chan_config.flags.invert_level_input =
        lua_driver_pcnt_get_bool_field(L, table_idx, "invert_level", false);

    out_config->pos_edge =
        lua_driver_pcnt_get_edge_action_field(L, table_idx, "pos_edge", "increase");
    out_config->neg_edge =
        lua_driver_pcnt_get_edge_action_field(L, table_idx, "neg_edge", "hold");
    out_config->high_level =
        lua_driver_pcnt_get_level_action_field(L, table_idx, "high_level", "keep");
    out_config->low_level =
        lua_driver_pcnt_get_level_action_field(L, table_idx, "low_level", "keep");
}

static esp_err_t lua_driver_pcnt_destroy(lua_driver_pcnt_ud_t *ud)
{
    esp_err_t first_err = ESP_OK;
    esp_err_t err;

    if (!ud) {
        return ESP_OK;
    }

    if (ud->running && ud->unit) {
        err = pcnt_unit_stop(ud->unit);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->running = false;
    }

    if (ud->enabled && ud->unit) {
        err = pcnt_unit_disable(ud->unit);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->enabled = false;
    }

    for (int i = 0; i < ud->channel_count; ++i) {
        if (ud->channels[i]) {
            err = pcnt_del_channel(ud->channels[i]);
            if (first_err == ESP_OK && err != ESP_OK) {
                first_err = err;
            }
            ud->channels[i] = NULL;
        }
    }
    ud->channel_count = 0;

    if (ud->unit) {
        err = pcnt_del_unit(ud->unit);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->unit = NULL;
    }

    return first_err;
}

static esp_err_t lua_driver_pcnt_add_channel_from_table(lua_State *L,
                                                        lua_driver_pcnt_ud_t *ud,
                                                        int table_idx)
{
    lua_driver_pcnt_channel_config_t resolved_config;
    lua_driver_pcnt_resolve_channel_config(L, table_idx, &resolved_config);

    if (ud->enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ud->channel_count >= SOC_PCNT_CHANNELS_PER_UNIT) {
        return ESP_ERR_NOT_FOUND;
    }

    pcnt_channel_handle_t channel = NULL;
    esp_err_t err = pcnt_new_channel(ud->unit, &resolved_config.chan_config, &channel);
    if (err != ESP_OK) {
        return err;
    }

    err = pcnt_channel_set_edge_action(channel,
                                       resolved_config.pos_edge,
                                       resolved_config.neg_edge);
    if (err != ESP_OK) {
        pcnt_del_channel(channel);
        return err;
    }

    err = pcnt_channel_set_level_action(channel,
                                        resolved_config.high_level,
                                        resolved_config.low_level);
    if (err != ESP_OK) {
        pcnt_del_channel(channel);
        return err;
    }

    ud->channels[ud->channel_count++] = channel;
    return ESP_OK;
}

static int lua_driver_pcnt_add_channel(lua_State *L)
{
    lua_driver_pcnt_ud_t *ud = lua_driver_pcnt_get_ud(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    esp_err_t err = lua_driver_pcnt_add_channel_from_table(L, ud, 2);
    if (err != ESP_OK) {
        return luaL_error(L, "pcnt add_channel failed: %s", esp_err_to_name(err));
    }

    lua_settop(L, 1);
    return 1;
}

static int lua_driver_pcnt_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    int low_limit = lua_driver_pcnt_get_int_field(
        L, 1, "low_limit", LUA_DRIVER_PCNT_DEFAULT_LOW_LIMIT);
    int high_limit = lua_driver_pcnt_get_int_field(
        L, 1, "high_limit", LUA_DRIVER_PCNT_DEFAULT_HIGH_LIMIT);
    bool accum_count = lua_driver_pcnt_get_bool_field(L, 1, "accum_count", false);
    bool has_channel_config = lua_driver_pcnt_table_has_channel_config(L, 1);
    lua_driver_pcnt_channel_config_t initial_channel_config;
    if (has_channel_config) {
        lua_driver_pcnt_resolve_channel_config(L, 1, &initial_channel_config);
    }

    bool has_glitch_filter = false;
    uint32_t glitch_ns = 0;
    lua_getfield(L, 1, "glitch_ns");
    if (!lua_isnil(L, -1)) {
        lua_Integer value = luaL_checkinteger(L, -1);
        if (value < 0) {
            lua_pop(L, 1);
            return luaL_error(L, "pcnt glitch_ns must be >= 0");
        }
        has_glitch_filter = true;
        glitch_ns = (uint32_t)value;
    }
    lua_pop(L, 1);

    if (low_limit >= 0) {
        return luaL_error(L, "pcnt low_limit must be lower than 0");
    }
    if (high_limit <= 0) {
        return luaL_error(L, "pcnt high_limit must be higher than 0");
    }
    if (low_limit >= high_limit) {
        return luaL_error(L, "pcnt low_limit must be lower than high_limit");
    }

    pcnt_unit_config_t unit_config = {
        .low_limit = low_limit,
        .high_limit = high_limit,
        .intr_priority = 0,
        .flags = {
            .accum_count = accum_count,
        },
    };

    lua_driver_pcnt_ud_t *ud = (lua_driver_pcnt_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    luaL_getmetatable(L, LUA_DRIVER_PCNT_METATABLE);
    lua_setmetatable(L, -2);

    esp_err_t err = pcnt_new_unit(&unit_config, &ud->unit);
    if (err != ESP_OK) {
        ud->unit = NULL;
        return luaL_error(L, "pcnt new unit failed: %s", esp_err_to_name(err));
    }

    if (has_glitch_filter) {
        pcnt_glitch_filter_config_t filter_config = {
            .max_glitch_ns = glitch_ns,
        };
        err = pcnt_unit_set_glitch_filter(ud->unit, &filter_config);
        if (err != ESP_OK) {
            lua_driver_pcnt_destroy(ud);
            return luaL_error(L, "pcnt set glitch filter failed: %s", esp_err_to_name(err));
        }
    }

    if (has_channel_config) {
        pcnt_channel_handle_t channel = NULL;
        err = pcnt_new_channel(ud->unit, &initial_channel_config.chan_config, &channel);
        if (err == ESP_OK) {
            err = pcnt_channel_set_edge_action(channel,
                                               initial_channel_config.pos_edge,
                                               initial_channel_config.neg_edge);
        }
        if (err == ESP_OK) {
            err = pcnt_channel_set_level_action(channel,
                                                initial_channel_config.high_level,
                                                initial_channel_config.low_level);
        }
        if (err != ESP_OK) {
            if (channel) {
                pcnt_del_channel(channel);
            }
            lua_driver_pcnt_destroy(ud);
            return luaL_error(L, "pcnt add initial channel failed: %s", esp_err_to_name(err));
        }
        ud->channels[ud->channel_count++] = channel;
    }

    return 1;
}

static int lua_driver_pcnt_start(lua_State *L)
{
    lua_driver_pcnt_ud_t *ud = lua_driver_pcnt_get_ud(L, 1);
    esp_err_t err;

    if (!ud->enabled) {
        err = pcnt_unit_enable(ud->unit);
        if (err != ESP_OK) {
            return luaL_error(L, "pcnt enable failed: %s", esp_err_to_name(err));
        }
        ud->enabled = true;
    }

    if (!ud->running) {
        err = pcnt_unit_start(ud->unit);
        if (err != ESP_OK) {
            return luaL_error(L, "pcnt start failed: %s", esp_err_to_name(err));
        }
        ud->running = true;
    }

    return 0;
}

static int lua_driver_pcnt_stop(lua_State *L)
{
    lua_driver_pcnt_ud_t *ud = lua_driver_pcnt_get_ud(L, 1);

    if (ud->running) {
        esp_err_t err = pcnt_unit_stop(ud->unit);
        if (err != ESP_OK) {
            return luaL_error(L, "pcnt stop failed: %s", esp_err_to_name(err));
        }
        ud->running = false;
    }

    return 0;
}

static int lua_driver_pcnt_clear(lua_State *L)
{
    lua_driver_pcnt_ud_t *ud = lua_driver_pcnt_get_ud(L, 1);
    esp_err_t err = pcnt_unit_clear_count(ud->unit);
    if (err != ESP_OK) {
        return luaL_error(L, "pcnt clear failed: %s", esp_err_to_name(err));
    }

    return 0;
}

static int lua_driver_pcnt_get_count(lua_State *L)
{
    lua_driver_pcnt_ud_t *ud = lua_driver_pcnt_get_ud(L, 1);
    int count = 0;
    esp_err_t err = pcnt_unit_get_count(ud->unit, &count);
    if (err != ESP_OK) {
        return luaL_error(L, "pcnt get_count failed: %s", esp_err_to_name(err));
    }

    lua_pushinteger(L, count);
    return 1;
}

static int lua_driver_pcnt_gc(lua_State *L)
{
    lua_driver_pcnt_ud_t *ud =
        (lua_driver_pcnt_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_PCNT_METATABLE);
    if (ud) {
        (void)lua_driver_pcnt_destroy(ud);
    }
    return 0;
}

static int lua_driver_pcnt_close(lua_State *L)
{
    lua_driver_pcnt_ud_t *ud =
        (lua_driver_pcnt_ud_t *)luaL_checkudata(L, 1, LUA_DRIVER_PCNT_METATABLE);
    esp_err_t err = lua_driver_pcnt_destroy(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "pcnt close failed: %s", esp_err_to_name(err));
    }
    return 0;
}

int luaopen_pcnt(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_PCNT_METATABLE)) {
        lua_pushcfunction(L, lua_driver_pcnt_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_pcnt_add_channel);
        lua_setfield(L, -2, "add_channel");
        lua_pushcfunction(L, lua_driver_pcnt_start);
        lua_setfield(L, -2, "start");
        lua_pushcfunction(L, lua_driver_pcnt_stop);
        lua_setfield(L, -2, "stop");
        lua_pushcfunction(L, lua_driver_pcnt_clear);
        lua_setfield(L, -2, "clear");
        lua_pushcfunction(L, lua_driver_pcnt_get_count);
        lua_setfield(L, -2, "get_count");
        lua_pushcfunction(L, lua_driver_pcnt_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_pcnt_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_pcnt_register(void)
{
    return cap_lua_register_module("pcnt", luaopen_pcnt);
}
