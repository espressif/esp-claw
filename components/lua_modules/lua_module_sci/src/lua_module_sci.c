/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_sci.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cap_lua.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "lauxlib.h"

#define LUA_MODULE_SCI_NAME              "sci"
#define LUA_MODULE_SCI_METATABLE         "sci.device"
#define LUA_MODULE_SCI_DEFAULT_ADDR      0x21
#define LUA_MODULE_SCI_DEFAULT_FREQ_HZ   100000U
#define LUA_MODULE_SCI_DEFAULT_TIMEOUT   2000U
#define LUA_MODULE_SCI_MAX_ADDR          0x7F
#define LUA_MODULE_SCI_MAX_SKU_LEN       7
#define LUA_MODULE_SCI_MAX_PAYLOAD       1024
#define LUA_MODULE_SCI_I2C_CHUNK         32

#define SCI_STATUS_SUCCESS               0x53
#define SCI_STATUS_FAILED                0x63

#define SCI_ERR_NONE                     0x00
#define SCI_ERR_CMD_INVALID              0x01
#define SCI_ERR_RES_PKT                  0x02
#define SCI_ERR_M_NO_SPACE               0x03
#define SCI_ERR_RES_TIMEOUT              0x04
#define SCI_ERR_ARGS                     0x07

typedef enum {
    SCI_CMD_SET_IF0 = 0x00,
    SCI_CMD_SET_IF1 = 0x01,
    SCI_CMD_SET_IF2 = 0x02,
    SCI_CMD_SET_ADDR = 0x03,
    SCI_CMD_SET_TIME = 0x04,
    SCI_CMD_RECORD_ON = 0x05,
    SCI_CMD_RECORD_OFF = 0x06,
    SCI_CMD_SCREEN_ON = 0x07,
    SCI_CMD_SCREEN_OFF = 0x08,
    SCI_CMD_GET_NAME = 0x09,
    SCI_CMD_GET_VALUE = 0x0A,
    SCI_CMD_GET_UNIT = 0x0B,
    SCI_CMD_GET_SKU = 0x0C,
    SCI_CMD_GET_INFO = 0x0D,
    SCI_CMD_GET_KEY_VALUE0 = 0x0E,
    SCI_CMD_GET_KEY_VALUE1 = 0x0F,
    SCI_CMD_GET_KEY_VALUE2 = 0x10,
    SCI_CMD_GET_KEY_UNIT0 = 0x11,
    SCI_CMD_GET_KEY_UNIT1 = 0x12,
    SCI_CMD_GET_KEY_UNIT2 = 0x13,
    SCI_CMD_RESET = 0x14,
    SCI_CMD_SKU_A = 0x15,
    SCI_CMD_SKU_D = 0x16,
    SCI_CMD_SKU_I2C = 0x17,
    SCI_CMD_SKU_UART = 0x18,
    SCI_CMD_GET_TIMESTAMP = 0x19,
    SCI_CMD_REFRESH_TIME = 0x20,
    SCI_CMD_GET_VERSION = 0x21,
} sci_cmd_t;

typedef struct {
    i2c_bus_handle_t bus;
    i2c_bus_device_handle_t dev;
    uint8_t addr;
    uint32_t timeout_ms;
    bool open;
} lua_module_sci_ud_t;

typedef struct {
    uint8_t status;
    uint8_t cmd;
    uint16_t len;
    uint8_t *data;
} sci_response_t;

static lua_module_sci_ud_t *lua_module_sci_get_ud(lua_State *L, int idx)
{
    lua_module_sci_ud_t *ud = (lua_module_sci_ud_t *)luaL_checkudata(
        L, idx, LUA_MODULE_SCI_METATABLE);
    if (!ud || !ud->open || !ud->dev) {
        luaL_error(L, "sci: invalid or closed device");
    }
    return ud;
}

static uint8_t lua_module_sci_check_addr(lua_State *L, int idx)
{
    lua_Integer addr = luaL_checkinteger(L, idx);
    if (addr < 0 || addr > LUA_MODULE_SCI_MAX_ADDR) {
        luaL_error(L, "sci address must be in range 0-127");
    }
    return (uint8_t)addr;
}

static uint8_t lua_module_sci_check_port_mask(lua_State *L, int idx, uint8_t def)
{
    lua_Integer mask = luaL_optinteger(L, idx, def);
    if (mask < 1 || mask > 0x07) {
        luaL_error(L, "sci port mask must be 1-7");
    }
    return (uint8_t)mask;
}

static uint8_t lua_module_sci_port_to_cmd(lua_State *L, int port)
{
    switch (port) {
    case 1:
        return SCI_CMD_SET_IF0;
    case 2:
        return SCI_CMD_SET_IF1;
    case 3:
        return SCI_CMD_SET_IF2;
    default:
        luaL_error(L, "sci port must be 1, 2, or 3");
        return SCI_CMD_SET_IF0;
    }
}

static const char *lua_module_sci_port_mode_text(int port, uint8_t mode)
{
    if (port == 1) {
        return mode == 0 ? "ANALOG" : mode == 1 ? "DIGITAL" : "UNKNOWN";
    }
    return mode == 0 ? "I2C" : mode == 1 ? "UART" : "UNKNOWN";
}

static uint32_t lua_module_sci_refresh_ms(uint8_t rate)
{
    static const uint32_t table[] = {
        0, 1000, 3000, 5000, 10000, 30000, 60000, 300000, 600000,
    };
    return rate < (sizeof(table) / sizeof(table[0])) ? table[rate] : 0;
}

static esp_err_t lua_module_sci_write(lua_module_sci_ud_t *ud,
                                      uint8_t cmd,
                                      const uint8_t *args,
                                      uint16_t arg_len)
{
    uint8_t header[3] = {
        cmd,
        (uint8_t)(arg_len & 0xFF),
        (uint8_t)(arg_len >> 8),
    };
    uint16_t total_len = (uint16_t)sizeof(header) + arg_len;
    uint8_t stack_buf[64];
    uint8_t *buf = stack_buf;
    esp_err_t err;

    if (total_len > sizeof(stack_buf)) {
        buf = (uint8_t *)malloc(total_len);
        if (!buf) {
            return ESP_ERR_NO_MEM;
        }
    }

    memcpy(buf, header, sizeof(header));
    if (arg_len > 0 && args) {
        memcpy(buf + sizeof(header), args, arg_len);
    }

    err = i2c_bus_write_bytes(ud->dev, NULL_I2C_MEM_ADDR, total_len, buf);
    if (buf != stack_buf) {
        free(buf);
    }
    return err;
}

static esp_err_t lua_module_sci_read_exact(lua_module_sci_ud_t *ud, uint8_t *buf, size_t len)
{
    while (len > 0) {
        size_t chunk = len > LUA_MODULE_SCI_I2C_CHUNK ? LUA_MODULE_SCI_I2C_CHUNK : len;
        esp_err_t err = i2c_bus_read_bytes(ud->dev, NULL_I2C_MEM_ADDR, chunk, buf);
        if (err != ESP_OK) {
            return err;
        }
        buf += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static void lua_module_sci_free_response(sci_response_t *resp)
{
    if (resp && resp->data) {
        free(resp->data);
        resp->data = NULL;
    }
}

static esp_err_t lua_module_sci_reset_cmd(lua_module_sci_ud_t *ud, uint8_t cmd)
{
    esp_err_t err = lua_module_sci_write(ud, SCI_CMD_RESET, &cmd, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    return err;
}

static esp_err_t lua_module_sci_read_response(lua_module_sci_ud_t *ud,
                                              uint8_t expected_cmd,
                                              sci_response_t *resp,
                                              uint8_t *error_code)
{
    int64_t deadline_us = esp_timer_get_time() + ((int64_t)ud->timeout_ms * 1000);

    memset(resp, 0, sizeof(*resp));
    if (error_code) {
        *error_code = SCI_ERR_NONE;
    }

    while (esp_timer_get_time() < deadline_us) {
        uint8_t status = 0;
        esp_err_t err = lua_module_sci_read_exact(ud, &status, 1);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (status != SCI_STATUS_SUCCESS && status != SCI_STATUS_FAILED) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint8_t hdr[3] = {0};
        err = lua_module_sci_read_exact(ud, hdr, sizeof(hdr));
        if (err != ESP_OK) {
            return err;
        }
        if (hdr[0] != expected_cmd) {
            lua_module_sci_reset_cmd(ud, expected_cmd);
            if (error_code) {
                *error_code = SCI_ERR_RES_PKT;
            }
            return ESP_ERR_INVALID_RESPONSE;
        }

        resp->status = status;
        resp->cmd = hdr[0];
        resp->len = (uint16_t)hdr[1] | ((uint16_t)hdr[2] << 8);
        if (resp->len > LUA_MODULE_SCI_MAX_PAYLOAD) {
            lua_module_sci_reset_cmd(ud, expected_cmd);
            if (error_code) {
                *error_code = SCI_ERR_RES_PKT;
            }
            return ESP_ERR_INVALID_SIZE;
        }
        if (resp->len > 0) {
            resp->data = (uint8_t *)malloc(resp->len);
            if (!resp->data) {
                if (error_code) {
                    *error_code = SCI_ERR_M_NO_SPACE;
                }
                return ESP_ERR_NO_MEM;
            }
            err = lua_module_sci_read_exact(ud, resp->data, resp->len);
            if (err != ESP_OK) {
                lua_module_sci_free_response(resp);
                return err;
            }
        }

        if (status == SCI_STATUS_FAILED && error_code && resp->len > 0) {
            *error_code = resp->data[0];
        }
        return ESP_OK;
    }

    lua_module_sci_reset_cmd(ud, expected_cmd);
    if (error_code) {
        *error_code = SCI_ERR_RES_TIMEOUT;
    }
    return ESP_ERR_TIMEOUT;
}

static uint8_t lua_module_sci_command_error(lua_module_sci_ud_t *ud,
                                            uint8_t cmd,
                                            const uint8_t *args,
                                            uint16_t arg_len)
{
    sci_response_t resp;
    uint8_t code = SCI_ERR_NONE;
    esp_err_t err = lua_module_sci_write(ud, cmd, args, arg_len);
    if (err != ESP_OK) {
        return SCI_ERR_RES_PKT;
    }
    err = lua_module_sci_read_response(ud, cmd, &resp, &code);
    lua_module_sci_free_response(&resp);
    return err == ESP_OK ? code : code;
}

static int lua_module_sci_push_string_command(lua_State *L,
                                              lua_module_sci_ud_t *ud,
                                              uint8_t cmd,
                                              const uint8_t *args,
                                              uint16_t arg_len)
{
    sci_response_t resp;
    uint8_t code = SCI_ERR_NONE;
    esp_err_t err = lua_module_sci_write(ud, cmd, args, arg_len);
    if (err != ESP_OK) {
        return luaL_error(L, "sci write failed: %s", esp_err_to_name(err));
    }
    err = lua_module_sci_read_response(ud, cmd, &resp, &code);
    if (err != ESP_OK) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci response failed for command 0x%02X: err=%s code=%u",
                          cmd, esp_err_to_name(err), code);
    }
    if (resp.status == SCI_STATUS_FAILED) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci command 0x%02X failed with code %u", cmd, code);
    }
    lua_pushlstring(L, resp.len > 0 ? (const char *)resp.data : "", resp.len);
    lua_module_sci_free_response(&resp);
    return 1;
}

static int lua_module_sci_new(lua_State *L)
{
    lua_Integer port = luaL_checkinteger(L, 1);
    lua_Integer sda = luaL_checkinteger(L, 2);
    lua_Integer scl = luaL_checkinteger(L, 3);
    uint8_t addr = lua_isnoneornil(L, 4) ? LUA_MODULE_SCI_DEFAULT_ADDR :
                   lua_module_sci_check_addr(L, 4);
    uint32_t freq_hz = (uint32_t)luaL_optinteger(L, 5, LUA_MODULE_SCI_DEFAULT_FREQ_HZ);

    if (port < 0 || port > 1) {
        return luaL_error(L, "sci i2c port must be 0 or 1");
    }
    if (freq_hz == 0 || freq_hz > LUA_MODULE_SCI_DEFAULT_FREQ_HZ) {
        return luaL_error(L, "sci i2c freq_hz must be 1-%u", LUA_MODULE_SCI_DEFAULT_FREQ_HZ);
    }

    i2c_config_t cfg = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = (gpio_num_t)sda,
        .scl_io_num = (gpio_num_t)scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = freq_hz,
        .clk_flags = 0,
    };
    i2c_bus_handle_t bus = i2c_bus_create((i2c_port_t)port, &cfg);
    if (!bus) {
        return luaL_error(L, "sci i2c bus create failed");
    }
    i2c_bus_device_handle_t dev = i2c_bus_device_create(bus, addr, freq_hz);
    if (!dev) {
        i2c_bus_delete(&bus);
        return luaL_error(L, "sci i2c device create failed");
    }

    lua_module_sci_ud_t *ud = (lua_module_sci_ud_t *)lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    ud->bus = bus;
    ud->dev = dev;
    ud->addr = addr;
    ud->timeout_ms = LUA_MODULE_SCI_DEFAULT_TIMEOUT;
    ud->open = true;
    luaL_getmetatable(L, LUA_MODULE_SCI_METATABLE);
    lua_setmetatable(L, -2);
    return 1;
}

static int lua_module_sci_close(lua_State *L)
{
    lua_module_sci_ud_t *ud = (lua_module_sci_ud_t *)luaL_checkudata(
        L, 1, LUA_MODULE_SCI_METATABLE);
    if (ud && ud->open) {
        if (ud->dev) {
            i2c_bus_device_delete(&ud->dev);
        }
        if (ud->bus) {
            i2c_bus_delete(&ud->bus);
        }
        ud->open = false;
    }
    return 0;
}

static int lua_module_sci_set_timeout(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    lua_Integer timeout = luaL_checkinteger(L, 2);
    if (timeout <= 0) {
        return luaL_error(L, "sci timeout_ms must be positive");
    }
    ud->timeout_ms = (uint32_t)timeout;
    return 0;
}

static int lua_module_sci_get_version(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    sci_response_t resp;
    uint8_t code = SCI_ERR_NONE;
    esp_err_t err = lua_module_sci_write(ud, SCI_CMD_GET_VERSION, NULL, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "sci write failed: %s", esp_err_to_name(err));
    }
    err = lua_module_sci_read_response(ud, SCI_CMD_GET_VERSION, &resp, &code);
    if (err != ESP_OK) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_version failed: err=%s code=%u",
                          esp_err_to_name(err), code);
    }
    if (resp.status == SCI_STATUS_FAILED || resp.len != 2) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_version failed with code %u", code);
    }
    uint16_t raw = ((uint16_t)resp.data[0] << 8) | resp.data[1];
    char text[16];
    snprintf(text, sizeof(text), "V%d.%d.%d",
             (int)((raw >> 8) & 0xFF),
             (int)((raw >> 4) & 0x0F),
             (int)(raw & 0x0F));
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, raw);
    lua_setfield(L, -2, "raw");
    lua_pushstring(L, text);
    lua_setfield(L, -2, "text");
    lua_module_sci_free_response(&resp);
    return 1;
}

static int lua_module_sci_get_address(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    sci_response_t resp;
    uint8_t code = SCI_ERR_NONE;
    esp_err_t err = lua_module_sci_write(ud, SCI_CMD_SET_ADDR, NULL, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "sci write failed: %s", esp_err_to_name(err));
    }
    err = lua_module_sci_read_response(ud, SCI_CMD_SET_ADDR, &resp, &code);
    if (err != ESP_OK) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_address failed: err=%s code=%u",
                          esp_err_to_name(err), code);
    }
    if (resp.status == SCI_STATUS_FAILED || resp.len < 1) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_address failed with code %u", code);
    }
    lua_pushinteger(L, resp.data[0]);
    lua_module_sci_free_response(&resp);
    return 1;
}

static int lua_module_sci_set_address(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    uint8_t addr = lua_module_sci_check_addr(L, 2);
    uint8_t code = lua_module_sci_command_error(ud, SCI_CMD_SET_ADDR, &addr, 1);
    if (code == SCI_ERR_NONE) {
        i2c_bus_device_handle_t new_dev = i2c_bus_device_create(ud->bus, addr, 0);
        if (!new_dev) {
            return luaL_error(L, "sci i2c device recreate failed after address change");
        }
        i2c_bus_device_delete(&ud->dev);
        ud->dev = new_dev;
        ud->addr = addr;
    }
    lua_pushinteger(L, code);
    return 1;
}

static int lua_module_sci_set_port(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    const char *sku = luaL_checkstring(L, 3);
    size_t len = strlen(sku);
    if (len > LUA_MODULE_SCI_MAX_SKU_LEN) {
        lua_pushinteger(L, SCI_ERR_ARGS);
        return 1;
    }
    lua_pushinteger(L, lua_module_sci_command_error(ud,
                                                    lua_module_sci_port_to_cmd(L, port),
                                                    (const uint8_t *)sku,
                                                    (uint16_t)len));
    return 1;
}

static int lua_module_sci_get_port(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    int port = (int)luaL_checkinteger(L, 2);
    uint8_t cmd = lua_module_sci_port_to_cmd(L, port);
    sci_response_t resp;
    uint8_t code = SCI_ERR_NONE;
    esp_err_t err = lua_module_sci_write(ud, cmd, NULL, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "sci write failed: %s", esp_err_to_name(err));
    }
    err = lua_module_sci_read_response(ud, cmd, &resp, &code);
    if (err != ESP_OK) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_port failed: err=%s code=%u",
                          esp_err_to_name(err), code);
    }
    if (resp.status == SCI_STATUS_FAILED || resp.len < 1) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_port failed with code %u", code);
    }

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, resp.data[0]);
    lua_setfield(L, -2, "mode");
    lua_pushstring(L, lua_module_sci_port_mode_text(port, resp.data[0]));
    lua_setfield(L, -2, "mode_text");
    lua_pushlstring(L, (const char *)resp.data + 1, resp.len - 1);
    lua_setfield(L, -2, "sku");
    lua_module_sci_free_response(&resp);
    return 1;
}

static int lua_module_sci_set_refresh_rate(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    lua_Integer rate = luaL_checkinteger(L, 2);
    if (rate < 0 || rate > 8) {
        return luaL_error(L, "sci refresh rate must be 0-8");
    }
    uint8_t arg = (uint8_t)rate;
    lua_pushinteger(L, lua_module_sci_command_error(ud, SCI_CMD_REFRESH_TIME, &arg, 1));
    return 1;
}

static int lua_module_sci_get_refresh_rate(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    sci_response_t resp;
    uint8_t code = SCI_ERR_NONE;
    esp_err_t err = lua_module_sci_write(ud, SCI_CMD_REFRESH_TIME, NULL, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "sci write failed: %s", esp_err_to_name(err));
    }
    err = lua_module_sci_read_response(ud, SCI_CMD_REFRESH_TIME, &resp, &code);
    if (err != ESP_OK) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_refresh_rate failed: err=%s code=%u",
                          esp_err_to_name(err), code);
    }
    if (resp.status == SCI_STATUS_FAILED || resp.len < 1) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_refresh_rate failed with code %u", code);
    }
    uint8_t rate = resp.data[0];
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, rate);
    lua_setfield(L, -2, "rate");
    lua_pushinteger(L, lua_module_sci_refresh_ms(rate));
    lua_setfield(L, -2, "ms");
    lua_module_sci_free_response(&resp);
    return 1;
}

static int lua_module_sci_simple_error_cmd(lua_State *L, uint8_t cmd)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    lua_pushinteger(L, lua_module_sci_command_error(ud, cmd, NULL, 0));
    return 1;
}

static int lua_module_sci_enable_record(lua_State *L) { return lua_module_sci_simple_error_cmd(L, SCI_CMD_RECORD_ON); }
static int lua_module_sci_disable_record(lua_State *L) { return lua_module_sci_simple_error_cmd(L, SCI_CMD_RECORD_OFF); }
static int lua_module_sci_oled_on(lua_State *L) { return lua_module_sci_simple_error_cmd(L, SCI_CMD_SCREEN_ON); }
static int lua_module_sci_oled_off(lua_State *L) { return lua_module_sci_simple_error_cmd(L, SCI_CMD_SCREEN_OFF); }

static int lua_module_sci_get_information(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    uint8_t args[2] = {
        lua_module_sci_check_port_mask(L, 2, 0x07),
        (uint8_t)lua_toboolean(L, 3),
    };
    return lua_module_sci_push_string_command(L, ud, SCI_CMD_GET_INFO, args, sizeof(args));
}

static int lua_module_sci_get_field(lua_State *L, uint8_t cmd)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    uint8_t port_mask = lua_module_sci_check_port_mask(L, 2, 0x07);
    return lua_module_sci_push_string_command(L, ud, cmd, &port_mask, 1);
}

static int lua_module_sci_get_sku(lua_State *L) { return lua_module_sci_get_field(L, SCI_CMD_GET_SKU); }
static int lua_module_sci_get_keys(lua_State *L) { return lua_module_sci_get_field(L, SCI_CMD_GET_NAME); }
static int lua_module_sci_get_values(lua_State *L) { return lua_module_sci_get_field(L, SCI_CMD_GET_VALUE); }
static int lua_module_sci_get_units(lua_State *L) { return lua_module_sci_get_field(L, SCI_CMD_GET_UNIT); }

static int lua_module_sci_key_lookup(lua_State *L, uint8_t base_cmd)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    const char *key = luaL_checkstring(L, 2);
    size_t key_len = strlen(key);
    bool has_port = !lua_isnoneornil(L, 3);
    bool has_sku = !lua_isnoneornil(L, 4);
    uint8_t port = has_port ? lua_module_sci_check_port_mask(L, 3, 0x07) : 0x07;
    const char *sku = has_sku ? luaL_checkstring(L, 4) : NULL;
    size_t sku_len = sku ? strlen(sku) : 0;
    uint8_t cmd = base_cmd;
    uint8_t stack_buf[96];
    uint8_t *args = stack_buf;
    size_t len = key_len;

    if (sku_len > LUA_MODULE_SCI_MAX_SKU_LEN) {
        return luaL_error(L, "sci sku length must be <= %d", LUA_MODULE_SCI_MAX_SKU_LEN);
    }
    if (has_sku) {
        cmd = base_cmd + 2;
        len = 1 + LUA_MODULE_SCI_MAX_SKU_LEN + key_len;
    } else if (has_port) {
        cmd = base_cmd + 1;
        len = 1 + key_len;
    }
    if (len > UINT16_MAX) {
        return luaL_error(L, "sci key lookup argument too long");
    }
    if (len > sizeof(stack_buf)) {
        args = (uint8_t *)malloc(len);
        if (!args) {
            return luaL_error(L, "sci key lookup out of memory");
        }
    }

    if (has_sku) {
        args[0] = port;
        memset(args + 1, 0, LUA_MODULE_SCI_MAX_SKU_LEN);
        memcpy(args + 1, sku, sku_len);
        memcpy(args + 1 + LUA_MODULE_SCI_MAX_SKU_LEN, key, key_len);
    } else if (has_port) {
        args[0] = port;
        memcpy(args + 1, key, key_len);
    } else {
        memcpy(args, key, key_len);
    }

    int ret = lua_module_sci_push_string_command(L, ud, cmd, args, (uint16_t)len);
    if (args != stack_buf) {
        free(args);
    }
    return ret;
}

static int lua_module_sci_get_value(lua_State *L)
{
    return lua_module_sci_key_lookup(L, SCI_CMD_GET_KEY_VALUE0);
}

static int lua_module_sci_get_unit(lua_State *L)
{
    return lua_module_sci_key_lookup(L, SCI_CMD_GET_KEY_UNIT0);
}

static int lua_module_sci_get_supported_skus(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    const char *kind = luaL_checkstring(L, 2);
    uint8_t cmd;
    if (strcmp(kind, "analog") == 0) {
        cmd = SCI_CMD_SKU_A;
    } else if (strcmp(kind, "digital") == 0) {
        cmd = SCI_CMD_SKU_D;
    } else if (strcmp(kind, "i2c") == 0) {
        cmd = SCI_CMD_SKU_I2C;
    } else if (strcmp(kind, "uart") == 0) {
        cmd = SCI_CMD_SKU_UART;
    } else {
        return luaL_error(L, "sci sku kind must be analog, digital, i2c, or uart");
    }
    return lua_module_sci_push_string_command(L, ud, cmd, NULL, 0);
}

static int lua_module_sci_get_timestamp(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    return lua_module_sci_push_string_command(L, ud, SCI_CMD_GET_TIMESTAMP, NULL, 0);
}

static int lua_module_sci_get_rtc(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    sci_response_t resp;
    uint8_t code = SCI_ERR_NONE;
    esp_err_t err = lua_module_sci_write(ud, SCI_CMD_SET_TIME, NULL, 0);
    if (err != ESP_OK) {
        return luaL_error(L, "sci write failed: %s", esp_err_to_name(err));
    }
    err = lua_module_sci_read_response(ud, SCI_CMD_SET_TIME, &resp, &code);
    if (err != ESP_OK) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_rtc failed: err=%s code=%u",
                          esp_err_to_name(err), code);
    }
    if (resp.status == SCI_STATUS_FAILED || resp.len != 8) {
        lua_module_sci_free_response(&resp);
        return luaL_error(L, "sci get_rtc failed with code %u", code);
    }
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, resp.data[0]); lua_setfield(L, -2, "second");
    lua_pushinteger(L, resp.data[1]); lua_setfield(L, -2, "minute");
    lua_pushinteger(L, resp.data[2]); lua_setfield(L, -2, "hour");
    lua_pushinteger(L, resp.data[3]); lua_setfield(L, -2, "day");
    lua_pushinteger(L, resp.data[4]); lua_setfield(L, -2, "week");
    lua_pushinteger(L, resp.data[5]); lua_setfield(L, -2, "month");
    lua_pushinteger(L, (uint16_t)resp.data[6] | ((uint16_t)resp.data[7] << 8));
    lua_setfield(L, -2, "year");
    lua_module_sci_free_response(&resp);
    return 1;
}

static int lua_module_sci_set_rtc(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const char *fields[] = { "second", "minute", "hour", "day", "week", "month", "year" };
    int values[7] = {0};
    for (int i = 0; i < 7; i++) {
        lua_getfield(L, 2, fields[i]);
        values[i] = (int)luaL_checkinteger(L, -1);
        lua_pop(L, 1);
    }
    if (values[6] < 0 || values[6] > 65535) {
        return luaL_error(L, "sci rtc year must be 0-65535");
    }
    uint8_t args[8] = {
        (uint8_t)values[0],
        (uint8_t)values[1],
        (uint8_t)values[2],
        (uint8_t)values[3],
        (uint8_t)values[4],
        (uint8_t)values[5],
        (uint8_t)(values[6] & 0xFF),
        (uint8_t)(values[6] >> 8),
    };
    lua_pushinteger(L, lua_module_sci_command_error(ud, SCI_CMD_SET_TIME, args, sizeof(args)));
    return 1;
}

static int lua_module_sci_reset(lua_State *L)
{
    lua_module_sci_ud_t *ud = lua_module_sci_get_ud(L, 1);
    lua_Integer cmd = luaL_optinteger(L, 2, SCI_CMD_RESET);
    if (cmd < 0 || cmd > SCI_CMD_RESET) {
        return luaL_error(L, "sci reset command must be 0-0x14");
    }
    esp_err_t err = lua_module_sci_reset_cmd(ud, (uint8_t)cmd);
    if (err != ESP_OK) {
        return luaL_error(L, "sci reset failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static const luaL_Reg lua_module_sci_methods[] = {
    { "close", lua_module_sci_close },
    { "set_timeout", lua_module_sci_set_timeout },
    { "get_version", lua_module_sci_get_version },
    { "get_address", lua_module_sci_get_address },
    { "set_address", lua_module_sci_set_address },
    { "set_port", lua_module_sci_set_port },
    { "get_port", lua_module_sci_get_port },
    { "set_refresh_rate", lua_module_sci_set_refresh_rate },
    { "get_refresh_rate", lua_module_sci_get_refresh_rate },
    { "enable_record", lua_module_sci_enable_record },
    { "disable_record", lua_module_sci_disable_record },
    { "oled_on", lua_module_sci_oled_on },
    { "oled_off", lua_module_sci_oled_off },
    { "get_information", lua_module_sci_get_information },
    { "get_sku", lua_module_sci_get_sku },
    { "get_keys", lua_module_sci_get_keys },
    { "get_values", lua_module_sci_get_values },
    { "get_units", lua_module_sci_get_units },
    { "get_value", lua_module_sci_get_value },
    { "get_unit", lua_module_sci_get_unit },
    { "get_supported_skus", lua_module_sci_get_supported_skus },
    { "get_timestamp", lua_module_sci_get_timestamp },
    { "get_rtc", lua_module_sci_get_rtc },
    { "set_rtc", lua_module_sci_set_rtc },
    { "reset", lua_module_sci_reset },
    { "__gc", lua_module_sci_close },
    { NULL, NULL },
};

static void lua_module_sci_set_const(lua_State *L, const char *name, lua_Integer value)
{
    lua_pushinteger(L, value);
    lua_setfield(L, -2, name);
}

int luaopen_sci(lua_State *L)
{
    if (luaL_newmetatable(L, LUA_MODULE_SCI_METATABLE)) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        luaL_setfuncs(L, lua_module_sci_methods, 0);
    }
    lua_pop(L, 1);

    lua_createtable(L, 0, 18);
    lua_pushcfunction(L, lua_module_sci_new);
    lua_setfield(L, -2, "new");
    lua_module_sci_set_const(L, "DEFAULT_ADDR", LUA_MODULE_SCI_DEFAULT_ADDR);
    lua_module_sci_set_const(L, "PORT1", 0x01);
    lua_module_sci_set_const(L, "PORT2", 0x02);
    lua_module_sci_set_const(L, "PORT3", 0x04);
    lua_module_sci_set_const(L, "ALL", 0x07);
    lua_module_sci_set_const(L, "REFRESH_MS", 0);
    lua_module_sci_set_const(L, "REFRESH_1S", 1);
    lua_module_sci_set_const(L, "REFRESH_3S", 2);
    lua_module_sci_set_const(L, "REFRESH_5S", 3);
    lua_module_sci_set_const(L, "REFRESH_10S", 4);
    lua_module_sci_set_const(L, "REFRESH_30S", 5);
    lua_module_sci_set_const(L, "REFRESH_1MIN", 6);
    lua_module_sci_set_const(L, "REFRESH_5MIN", 7);
    lua_module_sci_set_const(L, "REFRESH_10MIN", 8);
    return 1;
}

esp_err_t lua_module_sci_register(void)
{
    return cap_lua_register_module(LUA_MODULE_SCI_NAME, luaopen_sci);
}
