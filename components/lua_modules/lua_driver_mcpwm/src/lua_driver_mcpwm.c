/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_mcpwm.h"

#include <stdbool.h>
#include <stdint.h>

#include "cap_lua.h"
#include "driver/mcpwm_prelude.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lauxlib.h"

#define LUA_DRIVER_MCPWM_METATABLE "mcpwm"
#define LUA_DRIVER_MCPWM_MAX_CHANNELS 2
#define LUA_DRIVER_MCPWM_DEFAULT_GROUP_ID 0
#define LUA_DRIVER_MCPWM_DEFAULT_RESOLUTION_HZ 1000000U
#define LUA_DRIVER_MCPWM_DEFAULT_FREQUENCY_HZ 1000U
#define LUA_DRIVER_MCPWM_DEFAULT_DUTY_PERCENT 50.0

static const char *TAG = "lua_driver_mcpwm";

typedef struct {
    int gpio[LUA_DRIVER_MCPWM_MAX_CHANNELS];
    int group_id;
    uint32_t resolution_hz;
    uint32_t frequency_hz;
    double duty_percent[LUA_DRIVER_MCPWM_MAX_CHANNELS];
    bool invert[LUA_DRIVER_MCPWM_MAX_CHANNELS];
    uint8_t channel_count;
} lua_driver_mcpwm_config_t;

typedef struct {
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t comparator[LUA_DRIVER_MCPWM_MAX_CHANNELS];
    mcpwm_gen_handle_t generator[LUA_DRIVER_MCPWM_MAX_CHANNELS];
    lua_driver_mcpwm_config_t config;
    uint32_t period_ticks;
    bool timer_enabled;
    bool running;
} lua_driver_mcpwm_ud_t;

static lua_driver_mcpwm_ud_t *lua_driver_mcpwm_get_ud(lua_State *L, int idx)
{
    lua_driver_mcpwm_ud_t *ud =
        (lua_driver_mcpwm_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_MCPWM_METATABLE);
    if (!ud || !ud->timer) {
        luaL_error(L, "mcpwm: invalid or closed handle");
    }
    return ud;
}

static esp_err_t lua_driver_mcpwm_calc_period_ticks(uint32_t resolution_hz,
                                                    uint32_t frequency_hz,
                                                    uint32_t *period_ticks)
{
    ESP_RETURN_ON_FALSE(frequency_hz > 0, ESP_ERR_INVALID_ARG, TAG, "frequency must be > 0");
    ESP_RETURN_ON_FALSE(resolution_hz >= frequency_hz, ESP_ERR_INVALID_ARG, TAG,
                        "resolution must be >= frequency");

    uint32_t ticks = resolution_hz / frequency_hz;
    ESP_RETURN_ON_FALSE(ticks > 0, ESP_ERR_INVALID_ARG, TAG, "period ticks must be > 0");

    *period_ticks = ticks;
    return ESP_OK;
}

static esp_err_t lua_driver_mcpwm_apply_generator_actions(lua_driver_mcpwm_ud_t *ud, uint8_t channel)
{
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_timer_event(
                            ud->generator[channel],
                            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                         MCPWM_TIMER_EVENT_EMPTY,
                                                         MCPWM_GEN_ACTION_HIGH)),
                        TAG, "set timer action failed");
    ESP_RETURN_ON_ERROR(mcpwm_generator_set_action_on_compare_event(
                            ud->generator[channel],
                            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP,
                                                           ud->comparator[channel],
                                                           MCPWM_GEN_ACTION_LOW)),
                        TAG, "set compare action failed");
    return ESP_OK;
}

static esp_err_t lua_driver_mcpwm_apply_duty(lua_driver_mcpwm_ud_t *ud, uint8_t channel)
{
    int force_level = -1;
    if (ud->config.duty_percent[channel] <= 0.0) {
        force_level = 0;
    } else if (ud->config.duty_percent[channel] >= 100.0) {
        force_level = 1;
    }

    if (force_level >= 0) {
        ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(ud->generator[channel], force_level, true),
                            TAG, "force level failed");
        return ESP_OK;
    }

    uint32_t compare_ticks =
        (uint32_t)((((double)ud->period_ticks * ud->config.duty_percent[channel]) / 100.0) + 0.5);
    if (compare_ticks == 0) {
        compare_ticks = 1;
    }
    if (compare_ticks >= ud->period_ticks) {
        compare_ticks = ud->period_ticks - 1;
    }

    ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(ud->generator[channel], -1, false),
                        TAG, "clear force level failed");
    ESP_RETURN_ON_ERROR(mcpwm_comparator_set_compare_value(ud->comparator[channel], compare_ticks),
                        TAG, "set compare failed");
    return ESP_OK;
}

static esp_err_t lua_driver_mcpwm_destroy(lua_driver_mcpwm_ud_t *ud)
{
    esp_err_t first_err = ESP_OK;
    esp_err_t err;

    if (!ud) {
        return ESP_OK;
    }

    if (ud->running) {
        for (uint8_t i = 0; i < ud->config.channel_count; ++i) {
            if (!ud->generator[i]) {
                continue;
            }
            err = mcpwm_generator_set_force_level(ud->generator[i], 0, true);
            if (first_err == ESP_OK && err != ESP_OK) {
                first_err = err;
            }
        }
        ud->running = false;
    }

    if (ud->timer_enabled && ud->timer) {
        err = mcpwm_timer_disable(ud->timer);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->timer_enabled = false;
    }

    for (uint8_t i = 0; i < LUA_DRIVER_MCPWM_MAX_CHANNELS; ++i) {
        if (ud->generator[i]) {
            err = mcpwm_del_generator(ud->generator[i]);
            if (first_err == ESP_OK && err != ESP_OK) {
                first_err = err;
            }
            ud->generator[i] = NULL;
        }
    }

    for (uint8_t i = 0; i < LUA_DRIVER_MCPWM_MAX_CHANNELS; ++i) {
        if (ud->comparator[i]) {
            err = mcpwm_del_comparator(ud->comparator[i]);
            if (first_err == ESP_OK && err != ESP_OK) {
                first_err = err;
            }
            ud->comparator[i] = NULL;
        }
    }

    if (ud->oper) {
        err = mcpwm_del_operator(ud->oper);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->oper = NULL;
    }

    if (ud->timer) {
        err = mcpwm_del_timer(ud->timer);
        if (first_err == ESP_OK && err != ESP_OK) {
            first_err = err;
        }
        ud->timer = NULL;
    }

    return first_err;
}

static esp_err_t lua_driver_mcpwm_create(lua_driver_mcpwm_ud_t *ud)
{
    esp_err_t err;

    err = lua_driver_mcpwm_calc_period_ticks(ud->config.resolution_hz,
                                             ud->config.frequency_hz,
                                             &ud->period_ticks);
    if (err != ESP_OK) {
        return err;
    }

    mcpwm_timer_config_t timer_config = {
        .group_id = ud->config.group_id,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = ud->config.resolution_hz,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = ud->period_ticks,
        .intr_priority = 0,
        .flags = {
            .update_period_on_empty = 1,
        },
    };
    err = mcpwm_new_timer(&timer_config, &ud->timer);
    if (err != ESP_OK) {
        goto fail;
    }

    mcpwm_operator_config_t operator_config = {
        .group_id = ud->config.group_id,
        .intr_priority = 0,
    };
    err = mcpwm_new_operator(&operator_config, &ud->oper);
    if (err != ESP_OK) {
        goto fail;
    }

    err = mcpwm_operator_connect_timer(ud->oper, ud->timer);
    if (err != ESP_OK) {
        goto fail;
    }

    for (uint8_t i = 0; i < ud->config.channel_count; ++i) {
        mcpwm_comparator_config_t comparator_config = {
            .intr_priority = 0,
            .flags = {
                .update_cmp_on_tez = 1,
            },
        };
        err = mcpwm_new_comparator(ud->oper, &comparator_config, &ud->comparator[i]);
        if (err != ESP_OK) {
            goto fail;
        }

        mcpwm_generator_config_t generator_config = {
            .gen_gpio_num = ud->config.gpio[i],
            .flags = {
                .invert_pwm = ud->config.invert[i],
            },
        };
        err = mcpwm_new_generator(ud->oper, &generator_config, &ud->generator[i]);
        if (err != ESP_OK) {
            goto fail;
        }

        err = lua_driver_mcpwm_apply_generator_actions(ud, i);
        if (err != ESP_OK) {
            goto fail;
        }

        err = lua_driver_mcpwm_apply_duty(ud, i);
        if (err != ESP_OK) {
            goto fail;
        }
    }

    err = mcpwm_timer_enable(ud->timer);
    if (err != ESP_OK) {
        goto fail;
    }
    ud->timer_enabled = true;

    return ESP_OK;

fail:
    lua_driver_mcpwm_destroy(ud);
    return err;
}

static void lua_driver_mcpwm_parse_config(lua_State *L, int idx, lua_driver_mcpwm_config_t *config)
{
    luaL_checktype(L, idx, LUA_TTABLE);

    config->group_id = LUA_DRIVER_MCPWM_DEFAULT_GROUP_ID;
    config->resolution_hz = LUA_DRIVER_MCPWM_DEFAULT_RESOLUTION_HZ;
    config->frequency_hz = LUA_DRIVER_MCPWM_DEFAULT_FREQUENCY_HZ;
    config->channel_count = 1;
    for (uint8_t i = 0; i < LUA_DRIVER_MCPWM_MAX_CHANNELS; ++i) {
        config->duty_percent[i] = LUA_DRIVER_MCPWM_DEFAULT_DUTY_PERCENT;
        config->invert[i] = false;
        config->gpio[i] = -1;
    }

    lua_getfield(L, idx, "gpio");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, idx, "gpio_a");
    }
    if (lua_isnil(L, -1)) {
        luaL_error(L, "mcpwm.new: gpio or gpio_a is required");
    }
    config->gpio[0] = (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "gpio_b");
    if (!lua_isnil(L, -1)) {
        config->gpio[1] = (int)luaL_checkinteger(L, -1);
        config->channel_count = 2;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "group_id");
    if (!lua_isnil(L, -1)) {
        config->group_id = (int)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "resolution_hz");
    if (!lua_isnil(L, -1)) {
        config->resolution_hz = (uint32_t)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "frequency_hz");
    if (!lua_isnil(L, -1)) {
        config->frequency_hz = (uint32_t)luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "duty_percent");
    if (!lua_isnil(L, -1)) {
        config->duty_percent[0] = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "invert");
    if (!lua_isnil(L, -1)) {
        config->invert[0] = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "duty_percent_b");
    if (!lua_isnil(L, -1)) {
        config->duty_percent[1] = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "invert_b");
    if (!lua_isnil(L, -1)) {
        config->invert[1] = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);

    if (config->group_id < 0) {
        luaL_error(L, "mcpwm.new: group_id must be >= 0");
    }
    if (config->resolution_hz == 0) {
        luaL_error(L, "mcpwm.new: resolution_hz must be > 0");
    }
    if (config->frequency_hz == 0) {
        luaL_error(L, "mcpwm.new: frequency_hz must be > 0");
    }
    for (uint8_t i = 0; i < config->channel_count; ++i) {
        if (config->duty_percent[i] < 0.0 || config->duty_percent[i] > 100.0) {
            luaL_error(L, "mcpwm.new: duty_percent for channel %u must be between 0 and 100",
                       (unsigned)(i + 1));
        }
    }
    if (config->resolution_hz < config->frequency_hz) {
        luaL_error(L, "mcpwm.new: resolution_hz must be >= frequency_hz");
    }
    if (config->channel_count == 2 && config->gpio[0] == config->gpio[1]) {
        luaL_error(L, "mcpwm.new: gpio and gpio_b must be different");
    }
}

static uint8_t lua_driver_mcpwm_get_channel_index(lua_State *L,
                                                  lua_driver_mcpwm_ud_t *ud,
                                                  int arg_idx,
                                                  bool optional)
{
    lua_Integer channel = 1;

    if (!optional || !lua_isnoneornil(L, arg_idx)) {
        channel = luaL_checkinteger(L, arg_idx);
    }

    if (channel < 1 || channel > ud->config.channel_count) {
        luaL_error(L, "mcpwm channel must be between 1 and %u",
                   (unsigned)ud->config.channel_count);
    }

    return (uint8_t)(channel - 1);
}

static int lua_driver_mcpwm_gc(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud =
        (lua_driver_mcpwm_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_MCPWM_METATABLE);
    if (ud) {
        lua_driver_mcpwm_destroy(ud);
    }
    return 0;
}

static int lua_driver_mcpwm_start(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    if (!ud->running) {
        esp_err_t err = mcpwm_timer_start_stop(ud->timer, MCPWM_TIMER_START_NO_STOP);
        if (err != ESP_OK) {
            return luaL_error(L, "mcpwm start failed: %s", esp_err_to_name(err));
        }
        ud->running = true;
    }
    return 0;
}

static int lua_driver_mcpwm_stop(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    if (ud->running) {
        esp_err_t err = mcpwm_timer_start_stop(ud->timer, MCPWM_TIMER_STOP_EMPTY);
        if (err != ESP_OK) {
            return luaL_error(L, "mcpwm stop failed: %s", esp_err_to_name(err));
        }
        ud->running = false;
    }
    return 0;
}

static int lua_driver_mcpwm_set_enabled(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    if (lua_toboolean(L, 2)) {
        if (!ud->running) {
            esp_err_t err = mcpwm_timer_start_stop(ud->timer, MCPWM_TIMER_START_NO_STOP);
            if (err != ESP_OK) {
                return luaL_error(L, "mcpwm enable failed: %s", esp_err_to_name(err));
            }
            ud->running = true;
        }
    } else if (ud->running) {
        esp_err_t err = mcpwm_timer_start_stop(ud->timer, MCPWM_TIMER_STOP_EMPTY);
        if (err != ESP_OK) {
            return luaL_error(L, "mcpwm disable failed: %s", esp_err_to_name(err));
        }
        ud->running = false;
    }
    return 0;
}

static int lua_driver_mcpwm_set_duty(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    uint8_t channel = 0;
    double duty_percent = 0.0;

    if (lua_gettop(L) >= 3) {
        channel = lua_driver_mcpwm_get_channel_index(L, ud, 2, false);
        duty_percent = luaL_checknumber(L, 3);
    } else {
        channel = 0;
        duty_percent = luaL_checknumber(L, 2);
    }

    if (duty_percent < 0.0 || duty_percent > 100.0) {
        return luaL_error(L, "mcpwm duty_percent must be between 0 and 100");
    }

    ud->config.duty_percent[channel] = duty_percent;
    esp_err_t err = lua_driver_mcpwm_apply_duty(ud, channel);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm set_duty failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_mcpwm_set_frequency(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    uint32_t frequency_hz = (uint32_t)luaL_checkinteger(L, 2);
    if (frequency_hz == 0) {
        return luaL_error(L, "mcpwm frequency_hz must be > 0");
    }
    if (ud->config.resolution_hz < frequency_hz) {
        return luaL_error(L, "mcpwm resolution_hz must be >= frequency_hz");
    }

    uint32_t period_ticks = 0;
    esp_err_t err = lua_driver_mcpwm_calc_period_ticks(ud->config.resolution_hz,
                                                       frequency_hz,
                                                       &period_ticks);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm set_frequency failed: %s", esp_err_to_name(err));
    }

    err = mcpwm_timer_set_period(ud->timer, period_ticks);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm set_frequency failed: %s", esp_err_to_name(err));
    }

    ud->config.frequency_hz = frequency_hz;
    ud->period_ticks = period_ticks;
    for (uint8_t i = 0; i < ud->config.channel_count; ++i) {
        err = lua_driver_mcpwm_apply_duty(ud, i);
        if (err != ESP_OK) {
            return luaL_error(L, "mcpwm set_frequency failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

static int lua_driver_mcpwm_get_channel_count(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    lua_pushinteger(L, ud->config.channel_count);
    return 1;
}

static int lua_driver_mcpwm_close(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud =
        (lua_driver_mcpwm_ud_t *)luaL_checkudata(L, 1, LUA_DRIVER_MCPWM_METATABLE);
    esp_err_t err = lua_driver_mcpwm_destroy(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm close failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_mcpwm_new(lua_State *L)
{
    lua_driver_mcpwm_config_t config = {0};
    lua_driver_mcpwm_parse_config(L, 1, &config);

    lua_driver_mcpwm_ud_t *ud =
        (lua_driver_mcpwm_ud_t *)lua_newuserdata(L, sizeof(*ud));
    *ud = (lua_driver_mcpwm_ud_t) {
        .config = config,
    };

    esp_err_t err = lua_driver_mcpwm_create(ud);
    if (err != ESP_OK) {
        lua_driver_mcpwm_destroy(ud);
        return luaL_error(L, "mcpwm new failed: %s", esp_err_to_name(err));
    }

    luaL_getmetatable(L, LUA_DRIVER_MCPWM_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_mcpwm(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_MCPWM_METATABLE)) {
        lua_pushcfunction(L, lua_driver_mcpwm_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_mcpwm_start);
        lua_setfield(L, -2, "start");
        lua_pushcfunction(L, lua_driver_mcpwm_stop);
        lua_setfield(L, -2, "stop");
        lua_pushcfunction(L, lua_driver_mcpwm_set_duty);
        lua_setfield(L, -2, "set_duty");
        lua_pushcfunction(L, lua_driver_mcpwm_set_frequency);
        lua_setfield(L, -2, "set_frequency");
        lua_pushcfunction(L, lua_driver_mcpwm_get_channel_count);
        lua_setfield(L, -2, "get_channel_count");
        lua_pushcfunction(L, lua_driver_mcpwm_set_enabled);
        lua_setfield(L, -2, "set_enabled");
        lua_pushcfunction(L, lua_driver_mcpwm_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_mcpwm_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_mcpwm_register(void)
{
    return cap_lua_register_module("mcpwm", luaopen_mcpwm);
}
