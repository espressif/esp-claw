/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_driver_uart.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "hal/uart_types.h"
#include "lauxlib.h"

#define LUA_DRIVER_UART_METATABLE       "uart.port"
#define LUA_DRIVER_UART_RX_BUF_SIZE     1024
#define LUA_DRIVER_UART_TX_BUF_SIZE     0
#define LUA_DRIVER_UART_MAX_READ_LEN    4096
#define LUA_DRIVER_UART_MAX_LINE_LEN    1024

typedef struct {
    uart_port_t port;
    bool installed;
} lua_driver_uart_ud_t;

static lua_driver_uart_ud_t *lua_driver_uart_get_ud(lua_State *L, int idx)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_checkudata(
        L, idx, LUA_DRIVER_UART_METATABLE);
    if (!ud || !ud->installed) {
        luaL_error(L, "uart: invalid or closed port");
    }
    return ud;
}

static TickType_t lua_driver_uart_timeout_ticks(lua_State *L, int idx)
{
    if (lua_isnoneornil(L, idx)) {
        return 0;
    }
    lua_Integer ms = luaL_checkinteger(L, idx);
    if (ms < 0) {
        luaL_error(L, "uart timeout must be >= 0");
    }
    if (ms == 0) {
        return 0;
    }
    return pdMS_TO_TICKS((TickType_t)ms);
}

static uart_word_length_t lua_driver_uart_parse_data_bits(lua_State *L, int bits)
{
    switch (bits) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    case 8:
        return UART_DATA_8_BITS;
    default:
        luaL_error(L, "uart data_bits must be 5, 6, 7, or 8");
        return UART_DATA_8_BITS;
    }
}

static uart_parity_t lua_driver_uart_parse_parity(lua_State *L, const char *s)
{
    if (strcmp(s, "none") == 0) {
        return UART_PARITY_DISABLE;
    }
    if (strcmp(s, "even") == 0) {
        return UART_PARITY_EVEN;
    }
    if (strcmp(s, "odd") == 0) {
        return UART_PARITY_ODD;
    }
    luaL_error(L, "uart parity must be 'none', 'even', or 'odd'");
    return UART_PARITY_DISABLE;
}

static uart_stop_bits_t lua_driver_uart_parse_stop_bits(lua_State *L, int bits)
{
    switch (bits) {
    case 1:
        return UART_STOP_BITS_1;
    case 2:
        return UART_STOP_BITS_2;
    default:
        luaL_error(L, "uart stop_bits must be 1 or 2");
        return UART_STOP_BITS_1;
    }
}

static int lua_driver_uart_new(lua_State *L)
{
    lua_Integer port_num = luaL_checkinteger(L, 1);
    lua_Integer tx = luaL_checkinteger(L, 2);
    lua_Integer rx = luaL_checkinteger(L, 3);
    lua_Integer baud = luaL_checkinteger(L, 4);

    if (port_num < 0 || port_num >= UART_NUM_MAX) {
        return luaL_error(L, "uart port must be in range 0-%d", UART_NUM_MAX - 1);
    }
    if (baud <= 0) {
        return luaL_error(L, "uart baud must be positive");
    }

    uart_word_length_t data_bits = UART_DATA_8_BITS;
    uart_parity_t parity = UART_PARITY_DISABLE;
    uart_stop_bits_t stop_bits = UART_STOP_BITS_1;

    if (!lua_isnoneornil(L, 5)) {
        luaL_checktype(L, 5, LUA_TTABLE);

        lua_getfield(L, 5, "data_bits");
        if (!lua_isnil(L, -1)) {
            data_bits = lua_driver_uart_parse_data_bits(L, (int)luaL_checkinteger(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "parity");
        if (!lua_isnil(L, -1)) {
            parity = lua_driver_uart_parse_parity(L, luaL_checkstring(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, 5, "stop_bits");
        if (!lua_isnil(L, -1)) {
            stop_bits = lua_driver_uart_parse_stop_bits(L, (int)luaL_checkinteger(L, -1));
        }
        lua_pop(L, 1);
    }

    uart_config_t cfg = {
        .baud_rate = (int)baud,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    uart_port_t port = (uart_port_t)port_num;
    esp_err_t err = uart_driver_install(port,
                                        LUA_DRIVER_UART_RX_BUF_SIZE,
                                        LUA_DRIVER_UART_TX_BUF_SIZE,
                                        0, NULL, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "uart_driver_install failed on port %d: %s",
                          (int)port, esp_err_to_name(err));
    }
    err = uart_param_config(port, &cfg);
    if (err != ESP_OK) {
        uart_driver_delete(port);
        return luaL_error(L, "uart_param_config failed: %s", esp_err_to_name(err));
    }
    err = uart_set_pin(port, (int)tx, (int)rx,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        uart_driver_delete(port);
        return luaL_error(L, "uart_set_pin failed: %s", esp_err_to_name(err));
    }

    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)lua_newuserdata(
        L, sizeof(*ud));
    ud->port = port;
    ud->installed = true;
    luaL_getmetatable(L, LUA_DRIVER_UART_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_driver_uart_read(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);
    lua_Integer len = luaL_checkinteger(L, 2);
    TickType_t ticks = lua_driver_uart_timeout_ticks(L, 3);

    if (len <= 0 || len > LUA_DRIVER_UART_MAX_READ_LEN) {
        return luaL_error(L, "uart read length must be 1-%d",
                          LUA_DRIVER_UART_MAX_READ_LEN);
    }

    luaL_Buffer b;
    uint8_t *buf = (uint8_t *)luaL_buffinitsize(L, &b, (size_t)len);
    int got = uart_read_bytes(ud->port, buf, (uint32_t)len, ticks);
    if (got < 0) {
        return luaL_error(L, "uart read failed");
    }
    luaL_pushresultsize(&b, (size_t)got);
    return 1;
}

static int lua_driver_uart_read_line(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);
    lua_Integer max_len = luaL_optinteger(L, 2, LUA_DRIVER_UART_MAX_LINE_LEN);
    TickType_t ticks = lua_driver_uart_timeout_ticks(L, 3);

    if (max_len <= 0 || max_len > LUA_DRIVER_UART_MAX_LINE_LEN) {
        return luaL_error(L, "uart read_line max_len must be 1-%d",
                          LUA_DRIVER_UART_MAX_LINE_LEN);
    }

    TickType_t deadline = (ticks == 0) ? 0 : xTaskGetTickCount() + ticks;
    luaL_Buffer b;
    luaL_buffinit(L, &b);

    for (lua_Integer i = 0; i < max_len; i++) {
        uint8_t byte = 0;
        TickType_t remaining;
        if (ticks == 0) {
            remaining = 0;
        } else {
            TickType_t now = xTaskGetTickCount();
            remaining = (now >= deadline) ? 0 : (deadline - now);
        }
        int got = uart_read_bytes(ud->port, &byte, 1, remaining);
        if (got < 0) {
            return luaL_error(L, "uart read_line failed");
        }
        if (got == 0) {
            break;
        }
        luaL_addchar(&b, (char)byte);
        if (byte == '\n') {
            break;
        }
    }

    luaL_pushresult(&b);
    return 1;
}

static int lua_driver_uart_write(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);

    const uint8_t *data = NULL;
    size_t data_len = 0;

    int type = lua_type(L, 2);
    if (type == LUA_TSTRING) {
        data = (const uint8_t *)lua_tolstring(L, 2, &data_len);
    } else if (type == LUA_TTABLE) {
        lua_Integer n = luaL_len(L, 2);
        if (n < 0 || n > LUA_DRIVER_UART_MAX_READ_LEN) {
            return luaL_error(L, "uart write table length must be 0-%d",
                              LUA_DRIVER_UART_MAX_READ_LEN);
        }
        uint8_t *tmp = (uint8_t *)lua_newuserdata(L, (size_t)(n > 0 ? n : 1));
        for (lua_Integer i = 0; i < n; i++) {
            lua_rawgeti(L, 2, i + 1);
            lua_Integer byte = luaL_checkinteger(L, -1);
            if (byte < 0 || byte > 0xFF) {
                return luaL_error(L, "uart write byte #%d out of range 0-255",
                                  (int)(i + 1));
            }
            tmp[i] = (uint8_t)byte;
            lua_pop(L, 1);
        }
        data = tmp;
        data_len = (size_t)n;
    } else {
        return luaL_error(L, "uart write expects a string or table");
    }

    if (data_len == 0) {
        lua_pushinteger(L, 0);
        return 1;
    }

    int sent = uart_write_bytes(ud->port, (const char *)data, data_len);
    if (sent < 0) {
        return luaL_error(L, "uart write failed");
    }
    lua_pushinteger(L, sent);
    return 1;
}

static int lua_driver_uart_available(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);
    size_t size = 0;
    esp_err_t err = uart_get_buffered_data_len(ud->port, &size);
    if (err != ESP_OK) {
        return luaL_error(L, "uart available failed: %s", esp_err_to_name(err));
    }
    lua_pushinteger(L, (lua_Integer)size);
    return 1;
}

static int lua_driver_uart_flush_input(lua_State *L)
{
    lua_driver_uart_ud_t *ud = lua_driver_uart_get_ud(L, 1);
    esp_err_t err = uart_flush_input(ud->port);
    if (err != ESP_OK) {
        return luaL_error(L, "uart flush_input failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_driver_uart_gc(lua_State *L)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_testudata(
        L, 1, LUA_DRIVER_UART_METATABLE);
    if (ud && ud->installed) {
        uart_driver_delete(ud->port);
        ud->installed = false;
    }
    return 0;
}

static int lua_driver_uart_close(lua_State *L)
{
    lua_driver_uart_ud_t *ud = (lua_driver_uart_ud_t *)luaL_checkudata(
        L, 1, LUA_DRIVER_UART_METATABLE);
    if (ud->installed) {
        esp_err_t err = uart_driver_delete(ud->port);
        ud->installed = false;
        if (err != ESP_OK) {
            return luaL_error(L, "uart close failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

int luaopen_uart(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_DRIVER_UART_METATABLE)) {
        lua_pushcfunction(L, lua_driver_uart_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_driver_uart_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_driver_uart_read_line);
        lua_setfield(L, -2, "read_line");
        lua_pushcfunction(L, lua_driver_uart_write);
        lua_setfield(L, -2, "write");
        lua_pushcfunction(L, lua_driver_uart_available);
        lua_setfield(L, -2, "available");
        lua_pushcfunction(L, lua_driver_uart_flush_input);
        lua_setfield(L, -2, "flush_input");
        lua_pushcfunction(L, lua_driver_uart_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_driver_uart_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_driver_uart_register(void)
{
    return cap_lua_register_module("uart", luaopen_uart);
}
