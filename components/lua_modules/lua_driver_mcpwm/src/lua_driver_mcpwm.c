/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_mcpwm.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "cap_lua.h"
#include "cap_lua_hw.h"
#include "driver/gpio.h"
#include "driver/mcpwm_prelude.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "lauxlib.h"

#define LUA_DRIVER_MCPWM_METATABLE "mcpwm"
#define LUA_DRIVER_MCPWM_MAX_CHANNELS 2
#define LUA_DRIVER_MCPWM_DEFAULT_GROUP_ID 0
#define LUA_DRIVER_MCPWM_DEFAULT_RESOLUTION_HZ 1000000U
#define LUA_DRIVER_MCPWM_DEFAULT_FREQUENCY_HZ 1000U
#define LUA_DRIVER_MCPWM_DEFAULT_DUTY_PERCENT 50.0
#define LUA_DRIVER_MCPWM_MIN_PERIOD_TICKS 2U

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
    cap_lua_hw_lease_t lease;
    mcpwm_timer_handle_t timer;
    mcpwm_oper_handle_t oper;
    mcpwm_cmpr_handle_t comparator[LUA_DRIVER_MCPWM_MAX_CHANNELS];
    mcpwm_gen_handle_t generator[LUA_DRIVER_MCPWM_MAX_CHANNELS];
    lua_driver_mcpwm_config_t config;
    uint32_t period_ticks;
    bool timer_enabled;
    bool running;
    bool closing;
} lua_driver_mcpwm_ud_t;

static lua_driver_mcpwm_ud_t *lua_driver_mcpwm_get_ud(lua_State *L, int idx)
{
    lua_driver_mcpwm_ud_t *ud =
        (lua_driver_mcpwm_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_MCPWM_METATABLE);
    if (!ud || !ud->timer || ud->closing) {
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
    ESP_RETURN_ON_FALSE(ticks >= LUA_DRIVER_MCPWM_MIN_PERIOD_TICKS, ESP_ERR_INVALID_ARG, TAG, "period must contain at least two ticks");

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

static void lua_driver_mcpwm_wait_period(lua_driver_mcpwm_ud_t *ud)
{
    /* One extra tick accounts for the current scheduler tick being incomplete. */
    uint64_t ticks = ((uint64_t)ud->period_ticks * configTICK_RATE_HZ + ud->config.resolution_hz - 1) / ud->config.resolution_hz;
    vTaskDelay((TickType_t)(ticks + 1));
}

static esp_err_t lua_driver_mcpwm_set_compare(lua_driver_mcpwm_ud_t *ud, uint8_t channel, double duty_percent)
{
    if (duty_percent <= 0.0 || duty_percent >= 100.0) {
        return ESP_OK;
    }
    uint32_t ticks = (uint32_t)((double)ud->period_ticks * duty_percent / 100.0 + 0.5);
    if (ticks == 0) {
        ticks = 1;
    } else if (ticks >= ud->period_ticks) {
        ticks = ud->period_ticks - 1;
    }
    return mcpwm_comparator_set_compare_value(ud->comparator[channel], ticks);
}

static esp_err_t lua_driver_mcpwm_set_output(lua_driver_mcpwm_ud_t *ud, uint8_t channel, double duty_percent)
{
    int level = duty_percent <= 0.0 ? 0 : (duty_percent >= 100.0 ? 1 : -1);
    return mcpwm_generator_set_force_level(ud->generator[channel], level, true);
}

static esp_err_t lua_driver_mcpwm_stop_output(lua_driver_mcpwm_ud_t *ud)
{
    for (uint8_t i = 0; i < ud->config.channel_count; ++i) {
        if (ud->generator[i]) {
            /* Force the physical GPIO low, including inverted outputs. */
            ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(ud->generator[i], ud->config.invert[i] ? 1 : 0, true), TAG, "set idle level failed");
        }
    }
    if (ud->running) {
        ESP_RETURN_ON_ERROR(mcpwm_timer_start_stop(ud->timer, MCPWM_TIMER_STOP_EMPTY), TAG, "stop timer failed");
        lua_driver_mcpwm_wait_period(ud);
        ud->running = false;
    }
    return ESP_OK;
}

static esp_err_t lua_driver_mcpwm_start_output(lua_driver_mcpwm_ud_t *ud)
{
    if (ud->running) {
        return ESP_OK;
    }
    for (uint8_t i = 0; i < ud->config.channel_count; ++i) {
        ESP_RETURN_ON_ERROR(lua_driver_mcpwm_set_compare(ud, i, ud->config.duty_percent[i]), TAG, "prepare compare failed");
    }
    ESP_RETURN_ON_ERROR(mcpwm_timer_start_stop(ud->timer, MCPWM_TIMER_START_NO_STOP), TAG, "start timer failed");
    ud->running = true;
    /* Keep outputs idle until all shadow compare values have taken effect. */
    lua_driver_mcpwm_wait_period(ud);
    for (uint8_t i = 0; i < ud->config.channel_count; ++i) {
        esp_err_t err = lua_driver_mcpwm_set_output(ud, i, ud->config.duty_percent[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start channel %u failed: %s", (unsigned)(i + 1), esp_err_to_name(err));
            (void)lua_driver_mcpwm_stop_output(ud);
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t lua_driver_mcpwm_destroy(lua_driver_mcpwm_ud_t *ud)
{
    if (!ud) {
        return ESP_OK;
    }
    ud->closing = true;
    ESP_RETURN_ON_ERROR(lua_driver_mcpwm_stop_output(ud), TAG, "stop before close failed");
    if (ud->timer_enabled) {
        ESP_RETURN_ON_ERROR(mcpwm_timer_disable(ud->timer), TAG, "disable timer failed");
        ud->timer_enabled = false;
    }
    /* Retain failed handles so close can be retried without losing ownership. */
    for (uint8_t i = 0; i < ud->config.channel_count; ++i) {
        if (ud->generator[i]) {
            ESP_RETURN_ON_ERROR(mcpwm_del_generator(ud->generator[i]), TAG, "delete generator failed");
            ud->generator[i] = NULL;
        }
        if (ud->comparator[i]) {
            ESP_RETURN_ON_ERROR(mcpwm_del_comparator(ud->comparator[i]), TAG, "delete comparator failed");
            ud->comparator[i] = NULL;
        }
    }
    if (ud->oper) {
        ESP_RETURN_ON_ERROR(mcpwm_del_operator(ud->oper), TAG, "delete operator failed");
        ud->oper = NULL;
    }
    if (ud->timer) {
        ESP_RETURN_ON_ERROR(mcpwm_del_timer(ud->timer), TAG, "delete timer failed");
        ud->timer = NULL;
    }
    return cap_lua_hw_release(&ud->lease);
}

static esp_err_t lua_driver_mcpwm_create(lua_driver_mcpwm_ud_t *ud)
{
    ESP_RETURN_ON_ERROR(lua_driver_mcpwm_calc_period_ticks(ud->config.resolution_hz, ud->config.frequency_hz, &ud->period_ticks), TAG, "invalid initial period");
    mcpwm_timer_config_t timer_config = {
        .group_id = ud->config.group_id,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = ud->config.resolution_hz,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
        .period_ticks = ud->period_ticks,
        /* Period changes are only made while the timer is stopped. */
        .flags.update_period_on_empty = false,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_timer(&timer_config, &ud->timer), TAG, "create timer failed");
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
    ESP_RETURN_ON_ERROR(mcpwm_timer_get_resolution(ud->timer, &ud->config.resolution_hz), TAG, "get timer resolution failed");
#else
    /* Older IDF has no public API to read the adjusted timer resolution. */
    ESP_LOGW(TAG, "using requested timer resolution; hardware rounding may affect PWM frequency");
#endif
    ESP_RETURN_ON_ERROR(lua_driver_mcpwm_calc_period_ticks(ud->config.resolution_hz, ud->config.frequency_hz, &ud->period_ticks), TAG, "invalid actual period");
    ESP_RETURN_ON_ERROR(mcpwm_timer_set_period(ud->timer, ud->period_ticks), TAG, "set initial period failed");

    mcpwm_operator_config_t operator_config = {
        .group_id = ud->config.group_id,
    };
    ESP_RETURN_ON_ERROR(mcpwm_new_operator(&operator_config, &ud->oper), TAG, "create operator failed");
    ESP_RETURN_ON_ERROR(mcpwm_operator_connect_timer(ud->oper, ud->timer), TAG, "connect timer failed");

    for (uint8_t i = 0; i < ud->config.channel_count; ++i) {
        mcpwm_comparator_config_t comparator_config = {
            .flags.update_cmp_on_tez = true,
        };
        ESP_RETURN_ON_ERROR(mcpwm_new_comparator(ud->oper, &comparator_config, &ud->comparator[i]), TAG, "create comparator %u failed", (unsigned)(i + 1));
        mcpwm_generator_config_t generator_config = {
            .gen_gpio_num = ud->config.gpio[i],
            .flags.invert_pwm = ud->config.invert[i],
        };
        ESP_RETURN_ON_ERROR(mcpwm_new_generator(ud->oper, &generator_config, &ud->generator[i]), TAG, "create generator %u failed", (unsigned)(i + 1));
        ESP_RETURN_ON_ERROR(mcpwm_generator_set_force_level(ud->generator[i], ud->config.invert[i] ? 1 : 0, true), TAG, "set initial idle level failed");
        ESP_RETURN_ON_ERROR(lua_driver_mcpwm_apply_generator_actions(ud, i), TAG, "configure generator failed");
    }
    ESP_RETURN_ON_ERROR(mcpwm_timer_enable(ud->timer), TAG, "enable timer failed");
    ud->timer_enabled = true;
    return ESP_OK;
}

static lua_Integer lua_driver_mcpwm_check_integer(lua_State *L, int idx, uint32_t min, uint32_t max)
{
    lua_Integer value = luaL_checkinteger(L, idx);
    luaL_argcheck(L, value >= 0 && (uint64_t)value >= min && (uint64_t)value <= max, idx, "integer out of range");
    return value;
}

static int lua_driver_mcpwm_check_gpio(lua_State *L, int idx)
{
    int gpio = (int)lua_driver_mcpwm_check_integer(L, idx, 0, GPIO_NUM_MAX - 1);
    luaL_argcheck(L, GPIO_IS_VALID_OUTPUT_GPIO(gpio), idx, "invalid output GPIO");
    return gpio;
}

static bool lua_driver_mcpwm_check_boolean(lua_State *L, int idx)
{
    luaL_checktype(L, idx, LUA_TBOOLEAN);
    return lua_toboolean(L, idx);
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
    config->gpio[0] = lua_driver_mcpwm_check_gpio(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "gpio_b");
    if (!lua_isnil(L, -1)) {
        config->gpio[1] = lua_driver_mcpwm_check_gpio(L, -1);
        config->channel_count = 2;
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "group_id");
    if (!lua_isnil(L, -1)) {
        config->group_id = (int)lua_driver_mcpwm_check_integer(L, -1, 0, INT_MAX);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "resolution_hz");
    if (!lua_isnil(L, -1)) {
        config->resolution_hz = (uint32_t)lua_driver_mcpwm_check_integer(L, -1, 1, UINT32_MAX);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "frequency_hz");
    if (!lua_isnil(L, -1)) {
        config->frequency_hz = (uint32_t)lua_driver_mcpwm_check_integer(L, -1, 1, UINT32_MAX);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "duty_percent");
    if (!lua_isnil(L, -1)) {
        config->duty_percent[0] = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "invert");
    if (!lua_isnil(L, -1)) {
        config->invert[0] = lua_driver_mcpwm_check_boolean(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "duty_percent_b");
    if (!lua_isnil(L, -1)) {
        config->duty_percent[1] = luaL_checknumber(L, -1);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "invert_b");
    if (!lua_isnil(L, -1)) {
        config->invert[1] = lua_driver_mcpwm_check_boolean(L, -1);
    }
    lua_pop(L, 1);

    for (uint8_t i = 0; i < config->channel_count; ++i) {
        if (!isfinite(config->duty_percent[i]) || config->duty_percent[i] < 0.0 || config->duty_percent[i] > 100.0) {
            luaL_error(L, "mcpwm.new: duty_percent for channel %d must be finite and between 0 and 100",
                       (int)(i + 1));
        }
    }
    if (config->resolution_hz < config->frequency_hz) {
        luaL_error(L, "mcpwm.new: resolution_hz must be >= frequency_hz");
    }
    if (config->channel_count == 2 && config->gpio[0] == config->gpio[1]) {
        luaL_error(L, "mcpwm.new: gpio and gpio_b must be different");
    }
}

static uint8_t lua_driver_mcpwm_get_channel_index(lua_State *L, lua_driver_mcpwm_ud_t *ud, int arg_idx)
{
    lua_Integer channel = luaL_checkinteger(L, arg_idx);

    if (channel < 1 || channel > ud->config.channel_count) {
        luaL_error(L, "mcpwm channel must be between 1 and %d",
                   (int)ud->config.channel_count);
    }

    return (uint8_t)(channel - 1);
}

static int lua_driver_mcpwm_gc(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud =
        (lua_driver_mcpwm_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_MCPWM_METATABLE);
    if (ud) {
        esp_err_t err = lua_driver_mcpwm_destroy(ud);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "GC cleanup failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

static int lua_driver_mcpwm_start(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    esp_err_t err = lua_driver_mcpwm_start_output(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm start failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_mcpwm_stop(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    esp_err_t err = lua_driver_mcpwm_stop_output(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm stop failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_mcpwm_set_enabled(lua_State *L)
{
    return lua_driver_mcpwm_check_boolean(L, 2) ? lua_driver_mcpwm_start(L) : lua_driver_mcpwm_stop(L);
}

static int lua_driver_mcpwm_set_duty(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    uint8_t channel = 0;
    double duty_percent = 0.0;

    if (lua_gettop(L) >= 3) {
        channel = lua_driver_mcpwm_get_channel_index(L, ud, 2);
        duty_percent = luaL_checknumber(L, 3);
    } else {
        channel = 0;
        duty_percent = luaL_checknumber(L, 2);
    }

    if (!isfinite(duty_percent) || duty_percent < 0.0 || duty_percent > 100.0) {
        return luaL_error(L, "mcpwm duty_percent must be finite and between 0 and 100");
    }

    if (ud->running) {
        esp_err_t err = lua_driver_mcpwm_set_compare(ud, channel, duty_percent);
        if (err == ESP_OK) {
            double old_duty = ud->config.duty_percent[channel];
            if ((old_duty <= 0.0 || old_duty >= 100.0) && duty_percent > 0.0 && duty_percent < 100.0) {
                lua_driver_mcpwm_wait_period(ud);
            }
            err = lua_driver_mcpwm_set_output(ud, channel, duty_percent);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set duty channel %u failed: %s", (unsigned)(channel + 1), esp_err_to_name(err));
            (void)lua_driver_mcpwm_stop_output(ud);
            return luaL_error(L, "mcpwm set_duty failed: %s", esp_err_to_name(err));
        }
    }
    ud->config.duty_percent[channel] = duty_percent;
    return 0;
}

static int lua_driver_mcpwm_set_frequency(lua_State *L)
{
    lua_driver_mcpwm_ud_t *ud = lua_driver_mcpwm_get_ud(L, 1);
    uint32_t frequency_hz = (uint32_t)lua_driver_mcpwm_check_integer(L, 2, 1, UINT32_MAX);

    uint32_t period_ticks = 0;
    esp_err_t err = lua_driver_mcpwm_calc_period_ticks(ud->config.resolution_hz,
                                                       frequency_hz,
                                                       &period_ticks);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm set_frequency failed: %s", esp_err_to_name(err));
    }

    bool restart = ud->running;
    err = lua_driver_mcpwm_stop_output(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm set_frequency stop failed: %s", esp_err_to_name(err));
    }
    err = mcpwm_timer_set_period(ud->timer, period_ticks);
    if (err != ESP_OK) {
        return luaL_error(L, "mcpwm set_frequency failed (output stopped): %s", esp_err_to_name(err));
    }
    ud->config.frequency_hz = frequency_hz;
    ud->period_ticks = period_ticks;
    if (restart) {
        err = lua_driver_mcpwm_start_output(ud);
        if (err != ESP_OK) {
            return luaL_error(L, "mcpwm set_frequency restart failed: %s", esp_err_to_name(err));
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

    /* Attach the finalizer before acquiring hardware resources. */
    luaL_getmetatable(L, LUA_DRIVER_MCPWM_METATABLE);
    lua_setmetatable(L, -2);
    char key_a[CLAW_HW_KEY_SIZE], key_b[CLAW_HW_KEY_SIZE];
    const char *keys[] = {
        claw_hw_key_gpio(key_a, sizeof(key_a), config.gpio[0]),
        config.channel_count > 1 ? claw_hw_key_gpio(key_b, sizeof(key_b), config.gpio[1]) : NULL,
    };
    esp_err_t err = cap_lua_hw_claim(L, &ud->lease, "lua/mcpwm", keys, config.channel_count);
    if (err == ESP_OK) err = lua_driver_mcpwm_create(ud);
    if (err != ESP_OK) {
        (void)lua_driver_mcpwm_destroy(ud);
        return luaL_error(L, "mcpwm new failed: %s", esp_err_to_name(err));
    }

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
