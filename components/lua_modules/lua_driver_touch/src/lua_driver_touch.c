/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_driver_touch.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "cap_lua_hw.h"
#include "driver/touch_sens.h"
#include "esp_check.h"
#include "esp_board_device.h"
#include "esp_log.h"
#include "lauxlib.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if __has_include("hal/touch_sensor_periph.h")
#include "hal/touch_sensor_periph.h"
#else
#include "soc/touch_sensor_periph.h"
#endif

#define LUA_DRIVER_TOUCH_METATABLE        "touch.device"
#define LUA_DRIVER_TOUCH_DEFAULT_NAME     "touch_inputs"
#define LUA_DRIVER_TOUCH_MAX_NAME_LEN     64
#define LUA_DRIVER_TOUCH_MAX_KEYS         (TOUCH_MAX_CHAN_ID - TOUCH_MIN_CHAN_ID + 1)
#define LUA_DRIVER_TOUCH_INIT_SCAN_TIMES  3

#if SOC_TOUCH_SENSOR_VERSION == 1
#define LUA_TOUCH_SAMPLE_CFG_DEFAULT() TOUCH_SENSOR_V1_DEFAULT_SAMPLE_CONFIG(5.0, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_1V7)
#define LUA_TOUCH_CHAN_CFG_DEFAULT() { \
    .abs_active_thresh = {1000}, \
    .charge_speed = TOUCH_CHARGE_SPEED_7, \
    .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT, \
    .group = TOUCH_CHAN_TRIG_GROUP_BOTH, \
}
#elif SOC_TOUCH_SENSOR_VERSION == 2
#define LUA_TOUCH_SAMPLE_CFG_DEFAULT() TOUCH_SENSOR_V2_DEFAULT_SAMPLE_CONFIG(500, TOUCH_VOLT_LIM_L_0V5, TOUCH_VOLT_LIM_H_2V2)
#define LUA_TOUCH_CHAN_CFG_DEFAULT() { \
    .active_thresh = {2000}, \
    .charge_speed = TOUCH_CHARGE_SPEED_7, \
    .init_charge_volt = TOUCH_INIT_CHARGE_VOLT_DEFAULT, \
}
#elif SOC_TOUCH_SENSOR_VERSION == 3
#define LUA_TOUCH_SAMPLE_CFG_DEFAULT() TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG2(3, 29, 8, 3), \
                                       TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG2(2, 88, 31, 7), \
                                       TOUCH_SENSOR_V3_DEFAULT_SAMPLE_CONFIG2(3, 10, 31, 7)
#define LUA_TOUCH_CHAN_CFG_DEFAULT() { \
    .active_thresh = {1000, 2500, 5000}, \
}
#else
#error "Unsupported touch sensor version"
#endif

typedef struct {
    touch_sensor_handle_t sens_handle;
    touch_channel_handle_t chan_handle[LUA_DRIVER_TOUCH_MAX_KEYS];
    int channel_id[LUA_DRIVER_TOUCH_MAX_KEYS];
    int gpio_num[LUA_DRIVER_TOUCH_MAX_KEYS];
    uint32_t threshold[LUA_DRIVER_TOUCH_MAX_KEYS];
    int key_count;
    bool enabled;
    bool scanning;
} lua_driver_touch_handle_t;

typedef struct {
    lua_driver_touch_handle_t *handle;
    cap_lua_hw_lease_t lease;
    bool closing;
    char device_name[LUA_DRIVER_TOUCH_MAX_NAME_LEN];
} lua_driver_touch_ud_t;

typedef struct {
    int gpio_num[LUA_DRIVER_TOUCH_MAX_KEYS];
    int threshold_milli;
    int key_count;
} lua_touch_resolved_cfg_t;

static const char *TAG = "lua_driver_touch";

static int lua_driver_touch_gpio_to_channel(int gpio_num)
{
    /* Use the target's map instead of assuming GPIO and channel numbers match. */
    for (int channel = TOUCH_MIN_CHAN_ID; channel <= TOUCH_MAX_CHAN_ID; ++channel) {
        if (touch_sensor_channel_io_map[channel] == gpio_num) return channel;
    }
    return -1;
}

static lua_driver_touch_ud_t *lua_driver_touch_get_ud(lua_State *L, int idx)
{
    lua_driver_touch_ud_t *ud =
        (lua_driver_touch_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_TOUCH_METATABLE);
    if (!ud || !ud->handle || !ud->handle->sens_handle || ud->closing) {
        luaL_error(L, "touch: invalid or closed handle");
    }
    return ud;
}

static esp_err_t lua_driver_touch_apply_thresholds(lua_driver_touch_handle_t *handle, int threshold_milli)
{
    for (int i = 0; i < handle->key_count; i++) {
        uint32_t benchmark[TOUCH_SAMPLE_CFG_NUM] = {};
        touch_channel_config_t chan_cfg = LUA_TOUCH_CHAN_CFG_DEFAULT();

#if SOC_TOUCH_SUPPORT_BENCHMARK
        ESP_RETURN_ON_ERROR(touch_channel_read_data(handle->chan_handle[i],
                                                    TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                                    benchmark),
                            TAG, "Failed to read touch benchmark");
#else
        ESP_RETURN_ON_ERROR(touch_channel_read_data(handle->chan_handle[i],
                                                    TOUCH_CHAN_DATA_TYPE_SMOOTH,
                                                    benchmark),
                            TAG, "Failed to read touch smooth data");
#endif

        for (int j = 0; j < TOUCH_SAMPLE_CFG_NUM; j++) {
#if SOC_TOUCH_SENSOR_VERSION == 1
            chan_cfg.abs_active_thresh[j] = (uint32_t)(benchmark[j] * (1000 - threshold_milli) / 1000);
            handle->threshold[i] = chan_cfg.abs_active_thresh[0];
#else
            chan_cfg.active_thresh[j] = (uint32_t)(benchmark[j] * threshold_milli / 1000);
            handle->threshold[i] = chan_cfg.active_thresh[0];
#endif
        }

        ESP_RETURN_ON_ERROR(touch_sensor_reconfig_channel(handle->chan_handle[i], &chan_cfg),
                            TAG, "Failed to update touch threshold");
    }

    return ESP_OK;
}

static esp_err_t lua_driver_touch_initial_scan(lua_driver_touch_handle_t *handle, int threshold_milli)
{
    ESP_RETURN_ON_ERROR(touch_sensor_enable(handle->sens_handle), TAG, "Failed to enable touch sensor");
    handle->enabled = true;

    for (int i = 0; i < LUA_DRIVER_TOUCH_INIT_SCAN_TIMES; i++) {
        ESP_RETURN_ON_ERROR(touch_sensor_trigger_oneshot_scanning(handle->sens_handle, 2000),
                            TAG, "Failed to perform initial touch scan");
    }

    ESP_RETURN_ON_ERROR(touch_sensor_disable(handle->sens_handle), TAG, "Failed to disable touch sensor");
    handle->enabled = false;

    ESP_RETURN_ON_ERROR(lua_driver_touch_apply_thresholds(handle, threshold_milli),
                        TAG, "Failed to apply touch thresholds");

    return ESP_OK;
}

static esp_err_t lua_driver_touch_destroy_handle(lua_driver_touch_handle_t *handle)
{
    if (!handle) return ESP_OK;
    if (handle->scanning) {
        ESP_RETURN_ON_ERROR(touch_sensor_stop_continuous_scanning(handle->sens_handle), TAG, "stop scanning failed");
        handle->scanning = false;
    }
    if (handle->enabled) {
        ESP_RETURN_ON_ERROR(touch_sensor_disable(handle->sens_handle), TAG, "disable sensor failed");
        handle->enabled = false;
    }
    for (int i = 0; i < handle->key_count; i++) {
        if (handle->chan_handle[i]) {
            ESP_RETURN_ON_ERROR(touch_sensor_del_channel(handle->chan_handle[i]), TAG, "delete channel failed");
            handle->chan_handle[i] = NULL;
        }
    }
    if (handle->sens_handle) {
        ESP_RETURN_ON_ERROR(touch_sensor_del_controller(handle->sens_handle), TAG, "delete controller failed");
        handle->sens_handle = NULL;
    }
    free(handle);
    return ESP_OK;
}

static esp_err_t lua_driver_touch_create_handle(const lua_touch_resolved_cfg_t *cfg,
                                                lua_driver_touch_handle_t **out_handle)
{
    touch_sensor_sample_config_t sample_cfg[TOUCH_SAMPLE_CFG_NUM] = {
        LUA_TOUCH_SAMPLE_CFG_DEFAULT()
    };
    touch_sensor_config_t sens_cfg = TOUCH_SENSOR_DEFAULT_BASIC_CONFIG(TOUCH_SAMPLE_CFG_NUM, sample_cfg);
    touch_sensor_filter_config_t filter_cfg = TOUCH_SENSOR_DEFAULT_FILTER_CONFIG();
    touch_channel_config_t chan_cfg = LUA_TOUCH_CHAN_CFG_DEFAULT();
    lua_driver_touch_handle_t *handle = calloc(1, sizeof(lua_driver_touch_handle_t));

    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    *out_handle = handle;
    handle->key_count = cfg->key_count;
    memcpy(handle->gpio_num, cfg->gpio_num, sizeof(handle->gpio_num));

    esp_err_t err = touch_sensor_new_controller(&sens_cfg, &handle->sens_handle);
    if (err != ESP_OK) return err;

    for (int i = 0; i < cfg->key_count; i++) {
        handle->channel_id[i] = lua_driver_touch_gpio_to_channel(cfg->gpio_num[i]);
        if (handle->channel_id[i] < 0) {
            err = ESP_ERR_INVALID_ARG;
            ESP_LOGE(TAG, "GPIO%d is not a valid touch channel", cfg->gpio_num[i]);
            goto fail;
        }

        err = touch_sensor_new_channel(handle->sens_handle, handle->channel_id[i],
                                       &chan_cfg, &handle->chan_handle[i]);
        if (err != ESP_OK) {
            goto fail;
        }
    }

    err = touch_sensor_config_filter(handle->sens_handle, &filter_cfg);
    if (err != ESP_OK) {
        goto fail;
    }

    err = lua_driver_touch_initial_scan(handle, cfg->threshold_milli);
    if (err != ESP_OK) {
        goto fail;
    }

    err = touch_sensor_enable(handle->sens_handle);
    if (err != ESP_OK) {
        goto fail;
    }
    handle->enabled = true;

    err = touch_sensor_start_continuous_scanning(handle->sens_handle);
    if (err != ESP_OK) {
        goto fail;
    }
    handle->scanning = true;

    *out_handle = handle;
    ESP_LOGI(TAG, "Touch sensor initialized with %d channel(s), threshold=%d permille",
             cfg->key_count, cfg->threshold_milli);
    return ESP_OK;

fail:
    return err;
}

static esp_err_t lua_driver_touch_read_one(lua_driver_touch_handle_t *handle, int index,
                                           bool *pressed, uint32_t *smooth,
                                           uint32_t *benchmark, int32_t *delta)
{
    uint32_t smooth_data[TOUCH_SAMPLE_CFG_NUM] = {};
    uint32_t benchmark_data[TOUCH_SAMPLE_CFG_NUM] = {};

    ESP_RETURN_ON_FALSE(handle != NULL && index >= 0 && index < handle->key_count,
                        ESP_ERR_INVALID_ARG, TAG, "Invalid touch key index");

    ESP_RETURN_ON_ERROR(touch_channel_read_data(handle->chan_handle[index],
                                                TOUCH_CHAN_DATA_TYPE_SMOOTH,
                                                smooth_data),
                        TAG, "Failed to read touch smooth data");

#if SOC_TOUCH_SUPPORT_BENCHMARK
    ESP_RETURN_ON_ERROR(touch_channel_read_data(handle->chan_handle[index],
                                                TOUCH_CHAN_DATA_TYPE_BENCHMARK,
                                                benchmark_data),
                        TAG, "Failed to read touch benchmark");
#else
    benchmark_data[0] = handle->threshold[index];
#endif

    *smooth = smooth_data[0];
    *benchmark = benchmark_data[0];

#if SOC_TOUCH_SENSOR_VERSION == 1
    *delta = (int32_t)benchmark_data[0] - (int32_t)smooth_data[0];
    *pressed = smooth_data[0] <= handle->threshold[index];
#else
    *delta = (int32_t)smooth_data[0] - (int32_t)benchmark_data[0];
    *pressed = *delta >= (int32_t)handle->threshold[index];
#endif

    return ESP_OK;
}

static void lua_driver_touch_apply_lua_overrides(lua_State *L, int opts_idx,
                                                 lua_touch_resolved_cfg_t *cfg)
{
    if (opts_idx == 0 || lua_type(L, opts_idx) != LUA_TTABLE) {
        return;
    }

    lua_getfield(L, opts_idx, "gpios");
    if (!lua_isnil(L, -1)) {
        luaL_argcheck(L, lua_istable(L, -1), opts_idx, "gpios must be an array of GPIO numbers");

        size_t gpio_count = lua_rawlen(L, -1);
        luaL_argcheck(L, gpio_count > 0, opts_idx, "gpios must not be empty");
        luaL_argcheck(L, gpio_count <= LUA_DRIVER_TOUCH_MAX_KEYS, opts_idx, "too many touch GPIOs");

        cfg->key_count = 0;
        memset(cfg->gpio_num, 0, sizeof(cfg->gpio_num));

        for (size_t i = 0; i < gpio_count; i++) {
            lua_rawgeti(L, -1, (lua_Integer)i + 1);
            luaL_argcheck(L, lua_isnumber(L, -1), opts_idx, "gpios must contain GPIO numbers");

            lua_Number gpio_value = lua_tonumber(L, -1);
            int gpio_num = (int)gpio_value;
            luaL_argcheck(L, (lua_Number)gpio_num == gpio_value, opts_idx,
                          "gpios must contain integer GPIO numbers");
            luaL_argcheck(L, gpio_num >= 0, opts_idx, "gpios must contain non-negative GPIO numbers");

            for (int j = 0; j < cfg->key_count; j++) {
                luaL_argcheck(L, cfg->gpio_num[j] != gpio_num, opts_idx, "gpios must not contain duplicates");
            }

            cfg->gpio_num[cfg->key_count++] = gpio_num;
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, opts_idx, "threshold_milli");
    if (lua_isnumber(L, -1)) {
        cfg->threshold_milli = (int)lua_tointeger(L, -1);
    }
    lua_pop(L, 1);
}

static esp_err_t lua_driver_touch_destroy(lua_driver_touch_ud_t *ud)
{
    ud->closing = true;
    if (ud->handle) {
        ESP_RETURN_ON_ERROR(lua_driver_touch_destroy_handle(ud->handle), TAG, "close touch failed");
        ud->handle = NULL;
    }
    ESP_RETURN_ON_ERROR(cap_lua_hw_release(&ud->lease), TAG, "release touch claim failed");
    ud->device_name[0] = '\0';
    return ESP_OK;
}

static int lua_driver_touch_gc(lua_State *L)
{
    lua_driver_touch_ud_t *ud = luaL_testudata(L, 1, LUA_DRIVER_TOUCH_METATABLE);
    if (ud) (void)lua_driver_touch_destroy(ud);
    return 0;
}

static int lua_driver_touch_close(lua_State *L)
{
    lua_driver_touch_ud_t *ud = luaL_checkudata(L, 1, LUA_DRIVER_TOUCH_METATABLE);
    esp_err_t err = lua_driver_touch_destroy(ud);
    if (err != ESP_OK) return luaL_error(L, "touch close failed: %s", esp_err_to_name(err));
    return 0;
}

static int lua_driver_touch_name(lua_State *L)
{
    lua_driver_touch_ud_t *ud = lua_driver_touch_get_ud(L, 1);
    lua_pushstring(L, ud->device_name);
    return 1;
}

static void lua_driver_touch_push_key_table(lua_State *L, int index, int channel_id, int gpio_num,
                                            bool pressed, uint32_t smooth,
                                            uint32_t benchmark, int32_t delta,
                                            uint32_t threshold)
{
    lua_newtable(L);
    lua_pushinteger(L, index + 1);
    lua_setfield(L, -2, "index");
    lua_pushinteger(L, channel_id);
    lua_setfield(L, -2, "channel");
    lua_pushinteger(L, gpio_num);
    lua_setfield(L, -2, "gpio");
    lua_pushboolean(L, pressed);
    lua_setfield(L, -2, "pressed");
    lua_pushinteger(L, smooth);
    lua_setfield(L, -2, "smooth");
    lua_pushinteger(L, benchmark);
    lua_setfield(L, -2, "benchmark");
    lua_pushinteger(L, delta);
    lua_setfield(L, -2, "delta");
    lua_pushinteger(L, threshold);
    lua_setfield(L, -2, "threshold");
}

static int lua_driver_touch_read(lua_State *L)
{
    lua_driver_touch_ud_t *ud = lua_driver_touch_get_ud(L, 1);
    int pressed_count = 0;

    lua_newtable(L);
    lua_newtable(L);

    for (int i = 0; i < ud->handle->key_count; i++) {
        bool pressed = false;
        uint32_t smooth = 0;
        uint32_t benchmark = 0;
        int32_t delta = 0;
        esp_err_t err = lua_driver_touch_read_one(ud->handle, i, &pressed, &smooth, &benchmark, &delta);
        if (err != ESP_OK) {
            return luaL_error(L, "touch read failed: %s", esp_err_to_name(err));
        }
        if (pressed) {
            pressed_count++;
        }

        lua_driver_touch_push_key_table(L, i, ud->handle->channel_id[i], ud->handle->gpio_num[i],
                                        pressed, smooth, benchmark, delta, ud->handle->threshold[i]);
        lua_rawseti(L, -2, i + 1);
    }

    lua_setfield(L, -2, "keys");
    lua_pushinteger(L, ud->handle->key_count);
    lua_setfield(L, -2, "count");
    lua_pushboolean(L, pressed_count > 0);
    lua_setfield(L, -2, "any_pressed");
    lua_pushinteger(L, pressed_count);
    lua_setfield(L, -2, "pressed_count");
    return 1;
}

static int lua_driver_touch_is_pressed(lua_State *L)
{
    lua_driver_touch_ud_t *ud = lua_driver_touch_get_ud(L, 1);
    int index = (int)luaL_checkinteger(L, 2);
    bool pressed = false;
    uint32_t smooth = 0;
    uint32_t benchmark = 0;
    int32_t delta = 0;

    luaL_argcheck(L, index >= 1 && index <= ud->handle->key_count, 2, "touch key index out of range");

    esp_err_t err = lua_driver_touch_read_one(ud->handle, index - 1, &pressed, &smooth, &benchmark, &delta);
    if (err != ESP_OK) {
        return luaL_error(L, "touch is_pressed failed: %s", esp_err_to_name(err));
    }

    lua_pushboolean(L, pressed);
    return 1;
}

/* This layout is the touch_keys board YAML contract; verify before reading. */
typedef struct {
    const char *name;
    const char *type;
    const char *chip;
    int8_t key1_gpio_num;
    int8_t key2_gpio_num;
    int8_t threshold_milli;
} lua_touch_board_config_t;

static const lua_touch_board_config_t *lua_driver_touch_board_config(void)
{
    extern const esp_board_device_desc_t g_esp_board_devices[];
    for (const esp_board_device_desc_t *desc = g_esp_board_devices; desc && desc->name; desc = desc->next) {
        if (strcmp(desc->name, "touch_keys") == 0 && desc->cfg && desc->cfg_size == sizeof(lua_touch_board_config_t)) {
            const lua_touch_board_config_t *config = desc->cfg;
            if (config->type && strcmp(config->type, "custom") == 0 && config->chip && strcmp(config->chip, "esp32s3_touch") == 0) return config;
        }
    }
    return NULL;
}

static int lua_driver_touch_new(lua_State *L)
{
    const char *device_name = LUA_DRIVER_TOUCH_DEFAULT_NAME;
    int opts_idx = 0;

    if (lua_isstring(L, 1)) {
        device_name = lua_tostring(L, 1);
        if (lua_istable(L, 2)) {
            opts_idx = 2;
        }
    } else if (lua_istable(L, 1)) {
        opts_idx = 1;
        lua_getfield(L, 1, "device");
        if (lua_isstring(L, -1)) {
            device_name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    if (strlen(device_name) >= LUA_DRIVER_TOUCH_MAX_NAME_LEN) {
        return luaL_error(L, "touch device name too long");
    }

    lua_touch_resolved_cfg_t cfg = {
        .threshold_milli = CONFIG_LUA_DRIVER_TOUCH_DEFAULT_THRESHOLD_MILLI,
    };

    const lua_touch_board_config_t *board = lua_driver_touch_board_config();
    if (board) {
        cfg.gpio_num[0] = board->key1_gpio_num;
        cfg.gpio_num[1] = board->key2_gpio_num;
        cfg.key_count = 2;
        cfg.threshold_milli = board->threshold_milli;
    }
    lua_driver_touch_apply_lua_overrides(L, opts_idx, &cfg);

    if (cfg.key_count <= 0) {
        return luaL_error(L, "touch.new: missing gpios; pass { gpios = { ... } }");
    }
    if (cfg.threshold_milli <= 0) {
        return luaL_error(L, "touch.new: invalid threshold_milli=%d", cfg.threshold_milli);
    }

    lua_driver_touch_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    snprintf(ud->device_name, sizeof(ud->device_name), "%s", device_name);
    luaL_getmetatable(L, LUA_DRIVER_TOUCH_METATABLE);
    lua_setmetatable(L, -2);
    /* Keep variable-size resource lists off the task stack. */
    char (*key_buf)[CLAW_HW_KEY_SIZE] = lua_newuserdata(L, (size_t)cfg.key_count * sizeof(*key_buf));
    const char **keys = lua_newuserdata(L, (size_t)cfg.key_count * sizeof(*keys));
    for (int i = 0; i < cfg.key_count; ++i) {
        keys[i] = claw_hw_key_gpio(key_buf[i], sizeof(key_buf[i]), cfg.gpio_num[i]);
    }
    esp_err_t err = cap_lua_hw_claim(L, &ud->lease, "lua/touch", keys, cfg.key_count);
    lua_pop(L, 2);
    if (err == ESP_OK) err = lua_driver_touch_create_handle(&cfg, &ud->handle);
    if (err != ESP_OK) {
        (void)lua_driver_touch_destroy(ud);
        return luaL_error(L, "touch.new failed: %s", esp_err_to_name(err));
    }
    return 1;
}

int luaopen_touch(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_TOUCH_METATABLE)) {
        lua_pushcfunction(L, lua_driver_touch_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_touch_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_touch_is_pressed);
        lua_setfield(L, -2, "is_pressed");
        lua_pushcfunction(L, lua_driver_touch_name);
        lua_setfield(L, -2, "name");
        lua_pushcfunction(L, lua_driver_touch_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_touch_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_touch_register(void)
{
    return cap_lua_register_module("touch", luaopen_touch);
}
