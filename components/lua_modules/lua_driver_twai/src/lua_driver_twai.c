/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_twai.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/twai.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"

#define LUA_DRIVER_TWAI_METATABLE            "twai.bus"
#define LUA_DRIVER_TWAI_DEFAULT_BITRATE      500000
#define LUA_DRIVER_TWAI_DEFAULT_TX_QUEUE_LEN 8
#define LUA_DRIVER_TWAI_DEFAULT_RX_QUEUE_LEN 20
#define LUA_DRIVER_TWAI_MAX_DATA_LEN         8

typedef struct {
    bool installed;
    int tx_gpio;
    int rx_gpio;
    int bitrate;
    twai_mode_t mode;
} lua_driver_twai_ud_t;

static int lua_driver_twai_table_opt_int(lua_State *L, int idx, const char *key, int def)
{
    lua_getfield(L, idx, key);
    int value = lua_isnil(L, -1) ? def : (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    return value;
}

static bool lua_driver_twai_table_opt_bool(lua_State *L, int idx, const char *key, bool def)
{
    lua_getfield(L, idx, key);
    bool value = lua_isnil(L, -1) ? def : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static const char *lua_driver_twai_table_opt_string(lua_State *L, int idx,
                                                     const char *key, const char *def)
{
    lua_getfield(L, idx, key);
    const char *value = lua_isnil(L, -1) ? def : luaL_checkstring(L, -1);
    lua_pop(L, 1);
    return value;
}

static lua_driver_twai_ud_t *lua_driver_twai_get_ud(lua_State *L, int idx)
{
    lua_driver_twai_ud_t *ud =
        (lua_driver_twai_ud_t *)luaL_checkudata(L, idx, LUA_DRIVER_TWAI_METATABLE);
    if (!ud || !ud->installed) {
        luaL_error(L, "twai: invalid or closed bus");
    }
    return ud;
}

static TickType_t lua_driver_twai_timeout_ticks(lua_State *L, int idx, int default_ms)
{
    lua_Integer timeout_ms = luaL_optinteger(L, idx, default_ms);
    if (timeout_ms < 0 || timeout_ms > INT32_MAX) {
        luaL_error(L, "twai timeout must be in range 0-%d ms", INT32_MAX);
    }
    return timeout_ms == 0 ? 0 : pdMS_TO_TICKS((TickType_t)timeout_ms);
}

static twai_mode_t lua_driver_twai_parse_mode(lua_State *L, const char *mode)
{
    if (strcmp(mode, "normal") == 0) {
        return TWAI_MODE_NORMAL;
    }
    if (strcmp(mode, "no_ack") == 0) {
        return TWAI_MODE_NO_ACK;
    }
    if (strcmp(mode, "listen_only") == 0) {
        return TWAI_MODE_LISTEN_ONLY;
    }
    luaL_error(L, "twai mode must be 'normal', 'no_ack', or 'listen_only'");
    return TWAI_MODE_NORMAL;
}

static const char *lua_driver_twai_mode_name(twai_mode_t mode)
{
    switch (mode) {
    case TWAI_MODE_NO_ACK:
        return "no_ack";
    case TWAI_MODE_LISTEN_ONLY:
        return "listen_only";
    case TWAI_MODE_NORMAL:
    default:
        return "normal";
    }
}

static twai_timing_config_t lua_driver_twai_timing(lua_State *L, int bitrate)
{
    switch (bitrate) {
    case 25000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_25KBITS();
    case 50000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_50KBITS();
    case 100000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_100KBITS();
    case 125000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
    case 250000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
    case 500000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
    case 800000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_800KBITS();
    case 1000000:
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
    default:
        luaL_error(L, "twai bitrate must be 25000, 50000, 100000, 125000, "
                   "250000, 500000, 800000, or 1000000");
        return (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
    }
}

static const char *lua_driver_twai_state_name(twai_state_t state)
{
    switch (state) {
    case TWAI_STATE_STOPPED:
        return "stopped";
    case TWAI_STATE_RUNNING:
        return "running";
    case TWAI_STATE_BUS_OFF:
        return "bus_off";
    case TWAI_STATE_RECOVERING:
        return "recovering";
    default:
        return "unknown";
    }
}

static esp_err_t lua_driver_twai_release(lua_driver_twai_ud_t *ud)
{
    esp_err_t err;

    if (!ud || !ud->installed) {
        return ESP_OK;
    }

    err = twai_stop();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = twai_driver_uninstall();
    if (err == ESP_OK) {
        ud->installed = false;
    }
    return err;
}

static int lua_driver_twai_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    int tx_gpio = lua_driver_twai_table_opt_int(L, 1, "tx_gpio", -1);
    int rx_gpio = lua_driver_twai_table_opt_int(L, 1, "rx_gpio", -1);
    int bitrate = lua_driver_twai_table_opt_int(L, 1, "bitrate",
                                                LUA_DRIVER_TWAI_DEFAULT_BITRATE);
    int tx_queue_len = lua_driver_twai_table_opt_int(
        L, 1, "tx_queue_len", LUA_DRIVER_TWAI_DEFAULT_TX_QUEUE_LEN);
    int rx_queue_len = lua_driver_twai_table_opt_int(
        L, 1, "rx_queue_len", LUA_DRIVER_TWAI_DEFAULT_RX_QUEUE_LEN);
    const char *mode_name = lua_driver_twai_table_opt_string(L, 1, "mode", "normal");
    twai_mode_t mode = lua_driver_twai_parse_mode(L, mode_name);
    twai_timing_config_t timing = lua_driver_twai_timing(L, bitrate);

    if (tx_gpio < 0 || rx_gpio < 0) {
        return luaL_error(L, "twai tx_gpio and rx_gpio are required");
    }
    if (tx_queue_len < 0 || rx_queue_len <= 0) {
        return luaL_error(L, "twai tx_queue_len must be >= 0 and rx_queue_len must be > 0");
    }
    if (mode != TWAI_MODE_LISTEN_ONLY && tx_queue_len == 0) {
        return luaL_error(L, "twai tx_queue_len must be > 0 outside listen_only mode");
    }

    lua_driver_twai_ud_t *ud =
        (lua_driver_twai_ud_t *)lua_newuserdata(L, sizeof(*ud));
    *ud = (lua_driver_twai_ud_t) {
        .tx_gpio = tx_gpio,
        .rx_gpio = rx_gpio,
        .bitrate = bitrate,
        .mode = mode,
    };
    luaL_getmetatable(L, LUA_DRIVER_TWAI_METATABLE);
    lua_setmetatable(L, -2);

    twai_general_config_t general = TWAI_GENERAL_CONFIG_DEFAULT(
        (gpio_num_t)tx_gpio, (gpio_num_t)rx_gpio, mode);
    general.tx_queue_len = (uint32_t)tx_queue_len;
    general.rx_queue_len = (uint32_t)rx_queue_len;
    twai_filter_config_t filter = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&general, &timing, &filter);
    if (err != ESP_OK) {
        return luaL_error(L, "twai_driver_install failed: %s", esp_err_to_name(err));
    }
    ud->installed = true;

    err = twai_start();
    if (err != ESP_OK) {
        (void)twai_driver_uninstall();
        ud->installed = false;
        return luaL_error(L, "twai_start failed: %s", esp_err_to_name(err));
    }
    return 1;
}

static size_t lua_driver_twai_read_data(lua_State *L, int idx, uint8_t *data)
{
    int type = lua_type(L, idx);
    if (type == LUA_TSTRING) {
        size_t len = 0;
        const char *bytes = lua_tolstring(L, idx, &len);
        if (len > LUA_DRIVER_TWAI_MAX_DATA_LEN) {
            luaL_error(L, "twai data string must contain at most 8 bytes");
        }
        memcpy(data, bytes, len);
        return len;
    }
    if (type == LUA_TTABLE) {
        lua_Integer len = luaL_len(L, idx);
        if (len < 0 || len > LUA_DRIVER_TWAI_MAX_DATA_LEN) {
            luaL_error(L, "twai data table must contain at most 8 bytes");
        }
        for (lua_Integer i = 0; i < len; i++) {
            lua_rawgeti(L, idx, i + 1);
            lua_Integer value = luaL_checkinteger(L, -1);
            lua_pop(L, 1);
            if (value < 0 || value > UINT8_MAX) {
                luaL_error(L, "twai data byte #%d must be in range 0-255", (int)(i + 1));
            }
            data[i] = (uint8_t)value;
        }
        return (size_t)len;
    }
    luaL_error(L, "twai data must be a string or a table of bytes");
    return 0;
}

static int lua_driver_twai_send(lua_State *L)
{
    (void)lua_driver_twai_get_ud(L, 1);
    lua_Integer identifier = luaL_checkinteger(L, 2);
    twai_message_t message = { 0 };
    size_t data_len = lua_driver_twai_read_data(L, 3, message.data);
    int opts_idx = 4;
    TickType_t timeout_ticks = 0;

    if (!lua_isnoneornil(L, opts_idx)) {
        luaL_checktype(L, opts_idx, LUA_TTABLE);
        message.extd = lua_driver_twai_table_opt_bool(L, opts_idx, "extended", false);
        message.rtr = lua_driver_twai_table_opt_bool(L, opts_idx, "rtr", false);
        message.ss = lua_driver_twai_table_opt_bool(L, opts_idx, "single_shot", false);
        message.self = lua_driver_twai_table_opt_bool(L, opts_idx, "self_reception", false);
        lua_getfield(L, opts_idx, "timeout_ms");
        timeout_ticks = lua_driver_twai_timeout_ticks(L, -1, 0);
        lua_pop(L, 1);
    }

    lua_Integer max_identifier = message.extd ? 0x1FFFFFFF : 0x7FF;
    if (identifier < 0 || identifier > max_identifier) {
        return luaL_error(L, "twai identifier is out of range for a %s frame",
                          message.extd ? "29-bit" : "11-bit");
    }

    message.identifier = (uint32_t)identifier;
    message.data_length_code = (uint8_t)data_len;
    esp_err_t err = twai_transmit(&message, timeout_ticks);
    if (err == ESP_ERR_TIMEOUT) {
        lua_pushnil(L);
        lua_pushstring(L, "timeout");
        return 2;
    }
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_driver_twai_receive(lua_State *L)
{
    (void)lua_driver_twai_get_ud(L, 1);
    TickType_t timeout_ticks = lua_driver_twai_timeout_ticks(L, 2, 0);
    twai_message_t message = { 0 };
    esp_err_t err = twai_receive(&message, timeout_ticks);

    if (err == ESP_ERR_TIMEOUT) {
        lua_pushnil(L);
        lua_pushstring(L, "timeout");
        return 2;
    }
    if (err != ESP_OK) {
        lua_pushnil(L);
        lua_pushstring(L, esp_err_to_name(err));
        return 2;
    }

    lua_newtable(L);
    lua_pushinteger(L, message.identifier);
    lua_setfield(L, -2, "id");
    lua_pushinteger(L, message.data_length_code);
    lua_setfield(L, -2, "dlc");
    lua_pushboolean(L, message.extd);
    lua_setfield(L, -2, "extended");
    lua_pushboolean(L, message.rtr);
    lua_setfield(L, -2, "rtr");
    lua_pushboolean(L, message.self);
    lua_setfield(L, -2, "self_reception");

    lua_newtable(L);
    for (uint8_t i = 0; i < message.data_length_code && i < LUA_DRIVER_TWAI_MAX_DATA_LEN; i++) {
        lua_pushinteger(L, message.data[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "data");
    return 1;
}

static int lua_driver_twai_status(lua_State *L)
{
    lua_driver_twai_ud_t *ud = lua_driver_twai_get_ud(L, 1);
    twai_status_info_t status = { 0 };
    esp_err_t err = twai_get_status_info(&status);
    if (err != ESP_OK) {
        return luaL_error(L, "twai_get_status_info failed: %s", esp_err_to_name(err));
    }

    lua_newtable(L);
    lua_pushstring(L, lua_driver_twai_state_name(status.state));
    lua_setfield(L, -2, "state");
    lua_pushinteger(L, ud->tx_gpio);
    lua_setfield(L, -2, "tx_gpio");
    lua_pushinteger(L, ud->rx_gpio);
    lua_setfield(L, -2, "rx_gpio");
    lua_pushinteger(L, ud->bitrate);
    lua_setfield(L, -2, "bitrate");
    lua_pushstring(L, lua_driver_twai_mode_name(ud->mode));
    lua_setfield(L, -2, "mode");
#define LUA_DRIVER_TWAI_SET_STATUS_FIELD(field) \
    do { \
        lua_pushinteger(L, status.field); \
        lua_setfield(L, -2, #field); \
    } while (0)
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(msgs_to_tx);
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(msgs_to_rx);
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(tx_error_counter);
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(rx_error_counter);
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(tx_failed_count);
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(rx_missed_count);
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(rx_overrun_count);
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(arb_lost_count);
    LUA_DRIVER_TWAI_SET_STATUS_FIELD(bus_error_count);
#undef LUA_DRIVER_TWAI_SET_STATUS_FIELD
    return 1;
}

static int lua_driver_twai_clear_rx(lua_State *L)
{
    (void)lua_driver_twai_get_ud(L, 1);
    esp_err_t err = twai_clear_receive_queue();
    if (err != ESP_OK) {
        return luaL_error(L, "twai_clear_receive_queue failed: %s", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_driver_twai_recover(lua_State *L)
{
    (void)lua_driver_twai_get_ud(L, 1);
    TickType_t timeout_ticks = lua_driver_twai_timeout_ticks(L, 2, 1000);
    TickType_t start_ticks = xTaskGetTickCount();
    twai_status_info_t status = { 0 };
    esp_err_t err = twai_get_status_info(&status);
    if (err != ESP_OK) {
        return luaL_error(L, "twai_get_status_info failed: %s", esp_err_to_name(err));
    }

    if (status.state == TWAI_STATE_RUNNING) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (status.state == TWAI_STATE_STOPPED) {
        err = twai_start();
        if (err != ESP_OK) {
            return luaL_error(L, "twai_start failed: %s", esp_err_to_name(err));
        }
        lua_pushboolean(L, 1);
        return 1;
    }
    if (status.state == TWAI_STATE_BUS_OFF) {
        err = twai_initiate_recovery();
        if (err != ESP_OK) {
            return luaL_error(L, "twai_initiate_recovery failed: %s", esp_err_to_name(err));
        }
        err = twai_get_status_info(&status);
        if (err != ESP_OK) {
            return luaL_error(L, "twai_get_status_info failed: %s", esp_err_to_name(err));
        }
    }

    while (status.state == TWAI_STATE_RECOVERING) {
        if (timeout_ticks == 0 || xTaskGetTickCount() - start_ticks >= timeout_ticks) {
            lua_pushnil(L);
            lua_pushstring(L, "timeout");
            return 2;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        err = twai_get_status_info(&status);
        if (err != ESP_OK) {
            return luaL_error(L, "twai_get_status_info failed: %s", esp_err_to_name(err));
        }
    }

    if (status.state != TWAI_STATE_STOPPED) {
        lua_pushnil(L);
        lua_pushstring(L, lua_driver_twai_state_name(status.state));
        return 2;
    }
    err = twai_start();
    if (err != ESP_OK) {
        return luaL_error(L, "twai_start failed: %s", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_driver_twai_close(lua_State *L)
{
    lua_driver_twai_ud_t *ud =
        (lua_driver_twai_ud_t *)luaL_checkudata(L, 1, LUA_DRIVER_TWAI_METATABLE);
    esp_err_t err = lua_driver_twai_release(ud);
    if (err != ESP_OK) {
        return luaL_error(L, "twai close failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_twai_gc(lua_State *L)
{
    lua_driver_twai_ud_t *ud =
        (lua_driver_twai_ud_t *)luaL_testudata(L, 1, LUA_DRIVER_TWAI_METATABLE);
    (void)lua_driver_twai_release(ud);
    return 0;
}

int luaopen_twai(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_TWAI_METATABLE)) {
        lua_pushcfunction(L, lua_driver_twai_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_twai_send);
        lua_setfield(L, -2, "send");
        lua_pushcfunction(L, lua_driver_twai_receive);
        lua_setfield(L, -2, "receive");
        lua_pushcfunction(L, lua_driver_twai_status);
        lua_setfield(L, -2, "status");
        lua_pushcfunction(L, lua_driver_twai_clear_rx);
        lua_setfield(L, -2, "clear_rx");
        lua_pushcfunction(L, lua_driver_twai_recover);
        lua_setfield(L, -2, "recover");
        lua_pushcfunction(L, lua_driver_twai_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_twai_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_twai_register(void)
{
    return cap_lua_register_module("twai", luaopen_twai);
}
