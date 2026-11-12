/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_uart.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cap_lua.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lauxlib.h"

#define LUA_MODULE_UART_METATABLE "uart.handle"
#define LUA_MODULE_UART_RX_BUFFER_SIZE 1024
#define LUA_MODULE_UART_MAX_READ_SIZE 4096

typedef struct {
    uart_port_t port;
    bool open;
} lua_module_uart_ud_t;

static lua_module_uart_ud_t *lua_module_uart_check_open(lua_State *L, int index)
{
    lua_module_uart_ud_t *ud =
        (lua_module_uart_ud_t *)luaL_checkudata(L, index, LUA_MODULE_UART_METATABLE);
    if (!ud->open) {
        luaL_error(L, "uart: handle is closed");
    }
    return ud;
}

static uint32_t lua_module_uart_check_timeout(lua_State *L, int index, uint32_t default_value)
{
    lua_Integer value = luaL_optinteger(L, index, (lua_Integer)default_value);
    if (value < 0 || value > UINT32_MAX) {
        luaL_error(L, "uart: timeout must be between 0 and %u ms", UINT32_MAX);
    }
    return (uint32_t)value;
}

static size_t lua_module_uart_check_read_size(lua_State *L, int index)
{
    lua_Integer value = luaL_checkinteger(L, index);
    if (value <= 0 || value > LUA_MODULE_UART_MAX_READ_SIZE) {
        luaL_error(L, "uart: read size must be between 1 and %d bytes",
                   LUA_MODULE_UART_MAX_READ_SIZE);
    }
    return (size_t)value;
}

static int lua_module_uart_close(lua_State *L)
{
    lua_module_uart_ud_t *ud =
        (lua_module_uart_ud_t *)luaL_checkudata(L, 1, LUA_MODULE_UART_METATABLE);
    if (!ud->open) {
        return 0;
    }

    esp_err_t err = uart_driver_delete(ud->port);
    if (err != ESP_OK) {
        return luaL_error(L, "uart close failed: %s", esp_err_to_name(err));
    }
    ud->open = false;
    return 0;
}

static int lua_module_uart_gc(lua_State *L)
{
    lua_module_uart_ud_t *ud =
        (lua_module_uart_ud_t *)luaL_testudata(L, 1, LUA_MODULE_UART_METATABLE);
    if (ud && ud->open) {
        (void)uart_driver_delete(ud->port);
        ud->open = false;
    }
    return 0;
}

static int lua_module_uart_write(lua_State *L)
{
    lua_module_uart_ud_t *ud = lua_module_uart_check_open(L, 1);
    size_t length = 0;
    const char *data = luaL_checklstring(L, 2, &length);
    if (length > INT_MAX) {
        return luaL_error(L, "uart write data is too large");
    }

    int written = uart_write_bytes(ud->port, data, length);
    if (written < 0) {
        return luaL_error(L, "uart write failed");
    }

    lua_pushinteger(L, written);
    return 1;
}

static int lua_module_uart_available(lua_State *L)
{
    lua_module_uart_ud_t *ud = lua_module_uart_check_open(L, 1);
    size_t available = 0;
    esp_err_t err = uart_get_buffered_data_len(ud->port, &available);
    if (err != ESP_OK) {
        return luaL_error(L, "uart available failed: %s", esp_err_to_name(err));
    }

    lua_pushinteger(L, (lua_Integer)available);
    return 1;
}

static int lua_module_uart_flush_input(lua_State *L)
{
    lua_module_uart_ud_t *ud = lua_module_uart_check_open(L, 1);
    esp_err_t err = uart_flush_input(ud->port);
    if (err != ESP_OK) {
        return luaL_error(L, "uart flush_input failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_module_uart_wait_tx_done(lua_State *L)
{
    lua_module_uart_ud_t *ud = lua_module_uart_check_open(L, 1);
    uint32_t timeout_ms = lua_module_uart_check_timeout(L, 2, 1000);
    esp_err_t err = uart_wait_tx_done(ud->port, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        return luaL_error(L, "uart wait_tx_done failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_module_uart_read(lua_State *L)
{
    lua_module_uart_ud_t *ud = lua_module_uart_check_open(L, 1);
    size_t max_length = lua_module_uart_check_read_size(L, 2);
    uint32_t timeout_ms = lua_module_uart_check_timeout(L, 3, 0);
    uint8_t *buffer = malloc(max_length);
    if (!buffer) {
        return luaL_error(L, "uart read failed: out of memory");
    }

    int length = uart_read_bytes(ud->port, buffer, max_length, pdMS_TO_TICKS(timeout_ms));
    if (length < 0) {
        free(buffer);
        return luaL_error(L, "uart read failed");
    }
    if (length == 0) {
        free(buffer);
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *)buffer, (size_t)length);
    free(buffer);
    return 1;
}

static int lua_module_uart_read_line(lua_State *L)
{
    lua_module_uart_ud_t *ud = lua_module_uart_check_open(L, 1);
    size_t max_length = lua_module_uart_check_read_size(L, 2);
    uint32_t timeout_ms = lua_module_uart_check_timeout(L, 3, 0);
    uint8_t *buffer = malloc(max_length);
    if (!buffer) {
        return luaL_error(L, "uart read_line failed: out of memory");
    }

    size_t length = 0;
    TickType_t start = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    while (length < max_length) {
        TickType_t wait_ticks = 0;
        if (timeout_ticks > 0) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= timeout_ticks) {
                break;
            }
            wait_ticks = timeout_ticks - elapsed;
        }

        uint8_t byte = 0;
        int received = uart_read_bytes(ud->port, &byte, 1, wait_ticks);
        if (received <= 0) {
            break;
        }
        buffer[length++] = byte;
        if (byte == '\n') {
            break;
        }
    }

    if (length == 0) {
        free(buffer);
        lua_pushnil(L);
        return 1;
    }

    lua_pushlstring(L, (const char *)buffer, length);
    free(buffer);
    return 1;
}

static int lua_module_uart_new(lua_State *L)
{
    lua_Integer port_value = luaL_checkinteger(L, 1);
    lua_Integer tx_pin_value = luaL_checkinteger(L, 2);
    lua_Integer rx_pin_value = luaL_checkinteger(L, 3);
    lua_Integer baud_rate_value = luaL_checkinteger(L, 4);

    if (port_value < UART_NUM_0 || port_value >= UART_NUM_MAX) {
        return luaL_error(L, "uart.new: invalid UART port: %d", (int)port_value);
    }
    if (!GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)tx_pin_value)) {
        return luaL_error(L, "uart.new: invalid TX GPIO: %d", (int)tx_pin_value);
    }
    if (!GPIO_IS_VALID_GPIO((gpio_num_t)rx_pin_value)) {
        return luaL_error(L, "uart.new: invalid RX GPIO: %d", (int)rx_pin_value);
    }
    if (baud_rate_value <= 0 || baud_rate_value > INT_MAX) {
        return luaL_error(L, "uart.new: invalid baud rate: %d", (int)baud_rate_value);
    }

    uart_port_t port = (uart_port_t)port_value;
    esp_err_t err = uart_driver_install(port, LUA_MODULE_UART_RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "uart.new: driver install failed on UART%d: %s",
                          (int)port, esp_err_to_name(err));
    }

    const uart_config_t config = {
        .baud_rate = (int)baud_rate_value,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(port, &config);
    if (err == ESP_OK) {
        err = uart_set_pin(port, (int)tx_pin_value, (int)rx_pin_value,
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        (void)uart_driver_delete(port);
        return luaL_error(L, "uart.new: configuration failed on UART%d: %s",
                          (int)port, esp_err_to_name(err));
    }

    lua_module_uart_ud_t *ud =
        (lua_module_uart_ud_t *)lua_newuserdata(L, sizeof(*ud));
    *ud = (lua_module_uart_ud_t) {
        .port = port,
        .open = true,
    };
    luaL_getmetatable(L, LUA_MODULE_UART_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

int luaopen_uart(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_UART_METATABLE)) {
        lua_pushcfunction(L, lua_module_uart_gc);
        lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_module_uart_write);
        lua_setfield(L, -2, "write");
        lua_pushcfunction(L, lua_module_uart_read);
        lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_module_uart_read_line);
        lua_setfield(L, -2, "read_line");
        lua_pushcfunction(L, lua_module_uart_available);
        lua_setfield(L, -2, "available");
        lua_pushcfunction(L, lua_module_uart_flush_input);
        lua_setfield(L, -2, "flush_input");
        lua_pushcfunction(L, lua_module_uart_wait_tx_done);
        lua_setfield(L, -2, "wait_tx_done");
        lua_pushcfunction(L, lua_module_uart_close);
        lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    lua_pushcfunction(L, lua_module_uart_new);
    lua_setfield(L, -2, "new");
    return 1;
}

esp_err_t lua_module_uart_register(void)
{
    return cap_lua_register_module("uart", luaopen_uart);
}
