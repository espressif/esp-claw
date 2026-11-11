/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_ir_device.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cap_lua.h"
#include "cap_lua_hw.h"
#include "esp_log.h"
#include "ir_device.h"
#include "lauxlib.h"
#include "lua_ir_frame.h"
#include "sdkconfig.h"

#define LUA_IR_DEVICE_METATABLE   "ir.device"
#define LUA_IR_RX_GUARD_METATABLE "ir.rx_guard"
#define LUA_IR_DEFAULT_NAME       "ir"
#define LUA_IR_MAX_NAME_LEN       64
#define LUA_IR_DEFAULT_TIMEOUT_MS 5000
#define LUA_IR_DEFAULT_RX_MIN_NS  1250
#define LUA_IR_DEFAULT_RX_MAX_NS  60000000

static const char *TAG = "lua_ir_device";

typedef struct {
    ir_device_handle_t device;
    cap_lua_hw_lease_t lease;
    bool lease_claimed;
    char name[LUA_IR_MAX_NAME_LEN];
} lua_ir_device_ud_t;

typedef struct {
    ir_frame_t *frame;
} lua_ir_rx_guard_t;

static int lua_ir_rx_guard_gc(lua_State *L)
{
    lua_ir_rx_guard_t *guard = luaL_testudata(L, 1, LUA_IR_RX_GUARD_METATABLE);
    if (guard) ir_frame_delete(guard->frame);
    return 0;
}

static lua_ir_rx_guard_t *lua_ir_push_rx_guard(lua_State *L)
{
    lua_ir_rx_guard_t *guard = lua_newuserdata(L, sizeof(*guard));
    guard->frame = NULL;
    luaL_getmetatable(L, LUA_IR_RX_GUARD_METATABLE);
    lua_setmetatable(L, -2);
    return guard;
}

static lua_ir_device_ud_t *lua_ir_get_device(lua_State *L, int index)
{
    lua_ir_device_ud_t *ud = luaL_checkudata(L, index, LUA_IR_DEVICE_METATABLE);
    if (!ud || !ud->device) luaL_error(L, "ir: invalid or closed device");
    return ud;
}

static int lua_ir_table_int(lua_State *L, int index, const char *key, int default_value)
{
    lua_getfield(L, index, key);
    lua_Integer value = luaL_optinteger(L, -1, default_value);
    lua_pop(L, 1);
    if (value < INT32_MIN || value > INT32_MAX) luaL_error(L, "ir: %s is out of range", key);
    return (int)value;
}

static uint32_t lua_ir_table_u32(lua_State *L, int index, const char *key, uint32_t default_value)
{
    lua_getfield(L, index, key);
    lua_Integer value = luaL_optinteger(L, -1, default_value);
    lua_pop(L, 1);
    if (value < 0 || (uint64_t)value > UINT32_MAX) luaL_error(L, "ir: %s is out of range", key);
    return (uint32_t)value;
}

static bool lua_ir_table_bool(lua_State *L, int index, const char *key, bool default_value)
{
    lua_getfield(L, index, key);
    if (!lua_isnil(L, -1) && !lua_isboolean(L, -1)) luaL_error(L, "ir: %s must be a boolean", key);
    bool value = lua_isnil(L, -1) ? default_value : lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

static float lua_ir_table_float(lua_State *L, int index, const char *key, float default_value)
{
    lua_getfield(L, index, key);
    float value = (float)luaL_optnumber(L, -1, default_value);
    lua_pop(L, 1);
    return value;
}

static bool lua_ir_key_allowed(const char *key, const char *const *allowed, size_t allowed_count)
{
    for (size_t i = 0; i < allowed_count; i++) {
        if (strcmp(key, allowed[i]) == 0) return true;
    }
    return false;
}

static void lua_ir_check_table_fields(lua_State *L, int index, const char *context, const char *const *allowed, size_t allowed_count)
{
    index = lua_absindex(L, index);
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TSTRING) luaL_error(L, "ir.%s: field names must be strings", context);
        size_t length;
        const char *key = lua_tolstring(L, -2, &length);
        if (strlen(key) != length || !lua_ir_key_allowed(key, allowed, allowed_count)) luaL_error(L, "ir.%s: unknown field '%s'", context, key);
        lua_pop(L, 1);
    }
}

static void lua_ir_receive_options(lua_State *L, int index, const ir_device_config_t *device_config, int *timeout_ms, ir_device_receive_config_t *config)
{
    *timeout_ms = LUA_IR_DEFAULT_TIMEOUT_MS;
    config->signal_range_min_ns = device_config->signal_range_min_ns;
    config->signal_range_max_ns = device_config->signal_range_max_ns;
    if (lua_isnoneornil(L, index)) return;
    luaL_checktype(L, index, LUA_TTABLE);
    static const char *const fields[] = { "timeout_ms", "signal_range_min_ns", "signal_range_max_ns" };
    lua_ir_check_table_fields(L, index, "receive", fields, sizeof(fields) / sizeof(fields[0]));
    *timeout_ms = lua_ir_table_int(L, index, "timeout_ms", LUA_IR_DEFAULT_TIMEOUT_MS);
    config->signal_range_min_ns = lua_ir_table_u32(L, index, "signal_range_min_ns", config->signal_range_min_ns);
    config->signal_range_max_ns = lua_ir_table_u32(L, index, "signal_range_max_ns", config->signal_range_max_ns);
    if (*timeout_ms < 0 || config->signal_range_min_ns == 0 || config->signal_range_min_ns >= config->signal_range_max_ns) {
        luaL_error(L, "ir: invalid receive options");
    }
}

static int lua_ir_read_options(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) return LUA_IR_DEFAULT_TIMEOUT_MS;
    luaL_checktype(L, index, LUA_TTABLE);
    static const char *const fields[] = { "timeout_ms" };
    lua_ir_check_table_fields(L, index, "read", fields, sizeof(fields) / sizeof(fields[0]));
    int timeout_ms = lua_ir_table_int(L, index, "timeout_ms", LUA_IR_DEFAULT_TIMEOUT_MS);
    if (timeout_ms < 0) luaL_error(L, "ir: timeout_ms must be nonnegative");
    return timeout_ms;
}

static void lua_ir_start_receive_options(lua_State *L, int index, const ir_device_config_t *device_config, ir_device_receive_config_t *config)
{
    config->signal_range_min_ns = device_config->signal_range_min_ns;
    config->signal_range_max_ns = device_config->signal_range_max_ns;
    if (lua_isnoneornil(L, index)) return;
    luaL_checktype(L, index, LUA_TTABLE);
    static const char *const fields[] = { "signal_range_min_ns", "signal_range_max_ns" };
    lua_ir_check_table_fields(L, index, "start_receive", fields, sizeof(fields) / sizeof(fields[0]));
    config->signal_range_min_ns = lua_ir_table_u32(L, index, "signal_range_min_ns", config->signal_range_min_ns);
    config->signal_range_max_ns = lua_ir_table_u32(L, index, "signal_range_max_ns", config->signal_range_max_ns);
    if (config->signal_range_min_ns == 0 || config->signal_range_min_ns >= config->signal_range_max_ns) {
        luaL_error(L, "ir: invalid receive options");
    }
}

static int lua_ir_push_receive_result(lua_State *L, ir_device_receive_result_t *result, lua_ir_rx_guard_t *guard)
{
    if (result->status == IR_DEVICE_RECEIVE_FRAME) {
        guard->frame = result->frame;
        lua_ir_frame_push(L, result->frame);
        ir_device_receive_result_clear(result);
        guard->frame = NULL;
        lua_remove(L, -2);
        return 1;
    }
    const char *reason = result->status == IR_DEVICE_RECEIVE_TIMEOUT ? "timeout" :
                         result->status == IR_DEVICE_RECEIVE_FRAME_TOO_LONG ? "frame_too_long" : "empty_frame";
    ir_device_receive_result_clear(result);
    lua_pop(L, 1);
    lua_pushnil(L);
    lua_pushstring(L, reason);
    return 2;
}

static int lua_ir_device_send(lua_State *L)
{
    lua_ir_device_ud_t *ud = lua_ir_get_device(L, 1);
    const ir_device_config_t *config = ir_device_get_config(ud->device);
    if (config->tx_gpio < 0) return luaL_error(L, "ir: TX GPIO not configured");
    ir_frame_t *frame = lua_ir_frame_check(L, 2);
    int timeout_ms = LUA_IR_DEFAULT_TIMEOUT_MS;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        static const char *const fields[] = { "timeout_ms" };
        lua_ir_check_table_fields(L, 3, "send", fields, sizeof(fields) / sizeof(fields[0]));
        timeout_ms = lua_ir_table_int(L, 3, "timeout_ms", LUA_IR_DEFAULT_TIMEOUT_MS);
        if (timeout_ms < 0) return luaL_error(L, "ir: timeout_ms must be nonnegative");
    }
    esp_err_t err = ir_device_send(ud->device, frame, timeout_ms);
    if (err == ESP_ERR_TIMEOUT) return luaL_error(L, "ir: transmit wait failed: %s", esp_err_to_name(err));
    if (err != ESP_OK) return luaL_error(L, "ir: transmit failed: %s", esp_err_to_name(err));
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ir_device_receive(lua_State *L)
{
    lua_ir_device_ud_t *ud = lua_ir_get_device(L, 1);
    const ir_device_config_t *device_config = ir_device_get_config(ud->device);
    if (device_config->rx_gpio < 0) return luaL_error(L, "ir: RX GPIO not configured");
    int timeout_ms;
    ir_device_receive_config_t config;
    lua_ir_receive_options(L, 2, device_config, &timeout_ms, &config);
    lua_ir_rx_guard_t *guard = lua_ir_push_rx_guard(L);
    esp_err_t err = ir_device_start_receive(ud->device, &config);
    if (err != ESP_OK) return luaL_error(L, "ir: RX start failed: %s", esp_err_to_name(err));
    ir_device_receive_result_t result = {};
    err = ir_device_read(ud->device, timeout_ms, true, &result);
    if (err != ESP_OK) return luaL_error(L, "ir: RX cancel failed: %s", esp_err_to_name(err));
    return lua_ir_push_receive_result(L, &result, guard);
}

static int lua_ir_device_start_receive(lua_State *L)
{
    lua_ir_device_ud_t *ud = lua_ir_get_device(L, 1);
    const ir_device_config_t *device_config = ir_device_get_config(ud->device);
    if (device_config->rx_gpio < 0) return luaL_error(L, "ir: RX GPIO not configured");
    ir_device_receive_config_t config;
    lua_ir_start_receive_options(L, 2, device_config, &config);
    esp_err_t err = ir_device_start_receive(ud->device, &config);
    if (err != ESP_OK) return luaL_error(L, "ir: RX start failed: %s", esp_err_to_name(err));
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ir_device_read(lua_State *L)
{
    lua_ir_device_ud_t *ud = lua_ir_get_device(L, 1);
    if (!ir_device_is_receiving(ud->device)) return luaL_error(L, "ir: receiver is not started");
    lua_ir_rx_guard_t *guard = lua_ir_push_rx_guard(L);
    ir_device_receive_result_t result = {};
    esp_err_t err = ir_device_read(ud->device, lua_ir_read_options(L, 2), false, &result);
    if (err != ESP_OK) return luaL_error(L, "ir: RX read failed: %s", esp_err_to_name(err));
    return lua_ir_push_receive_result(L, &result, guard);
}

static int lua_ir_device_stop_receive(lua_State *L)
{
    lua_ir_device_ud_t *ud = lua_ir_get_device(L, 1);
    esp_err_t err = ir_device_stop_receive(ud->device);
    if (err != ESP_OK) return luaL_error(L, "ir: RX stop failed: %s", esp_err_to_name(err));
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ir_device_info(lua_State *L)
{
    lua_ir_device_ud_t *ud = lua_ir_get_device(L, 1);
    const ir_device_config_t *config = ir_device_get_config(ud->device);
    lua_newtable(L);
#define PUSH_INT_FIELD(name, value) do { lua_pushinteger(L, (value)); lua_setfield(L, -2, (name)); } while (0)
    lua_pushstring(L, ud->name); lua_setfield(L, -2, "name");
    PUSH_INT_FIELD("tx_gpio", config->tx_gpio);
    PUSH_INT_FIELD("rx_gpio", config->rx_gpio);
    PUSH_INT_FIELD("ctrl_gpio", config->ctrl_gpio);
    PUSH_INT_FIELD("carrier_hz", config->carrier_hz);
    PUSH_INT_FIELD("tx_resolution_hz", config->tx_resolution_hz);
    PUSH_INT_FIELD("rx_resolution_hz", config->rx_resolution_hz);
    PUSH_INT_FIELD("rx_max_symbols", config->rx_max_symbols);
    lua_pushboolean(L, config->tx_invert); lua_setfield(L, -2, "tx_invert");
    lua_pushboolean(L, config->rx_invert); lua_setfield(L, -2, "rx_invert");
#undef PUSH_INT_FIELD
    return 1;
}

static esp_err_t lua_ir_close_device(lua_ir_device_ud_t *ud)
{
    if (!ud) return ESP_OK;
    if (ud->device) {
        esp_err_t err = ir_device_delete(ud->device);
        if (err != ESP_OK) return err;
        ud->device = NULL;
    }
    if (ud->lease_claimed) {
        esp_err_t err = cap_lua_hw_release(&ud->lease);
        if (err != ESP_OK) return err;
        ud->lease_claimed = false;
    }
    ud->name[0] = '\0';
    return ESP_OK;
}

static int lua_ir_device_close(lua_State *L)
{
    lua_ir_device_ud_t *ud = luaL_checkudata(L, 1, LUA_IR_DEVICE_METATABLE);
    esp_err_t err = lua_ir_close_device(ud);
    if (err != ESP_OK) return luaL_error(L, "ir: close failed: %s", esp_err_to_name(err));
    return 0;
}

static int lua_ir_device_gc(lua_State *L)
{
    lua_ir_device_ud_t *ud = luaL_testudata(L, 1, LUA_IR_DEVICE_METATABLE);
    if (ud) {
        esp_err_t err = lua_ir_close_device(ud);
        if (err != ESP_OK) ESP_LOGE(TAG, "IR device cleanup failed: %s", esp_err_to_name(err));
    }
    return 0;
}

static int lua_ir_device_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    static const char *const fields[] = {
        "name", "tx_gpio", "rx_gpio", "ctrl_gpio", "ctrl_active_level", "tx_invert", "rx_invert", "carrier_hz",
        "carrier_duty", "tx_resolution_hz", "rx_resolution_hz", "rx_max_symbols", "signal_range_min_ns", "signal_range_max_ns",
    };
    lua_ir_check_table_fields(L, 1, "device", fields, sizeof(fields) / sizeof(fields[0]));
    ir_device_config_t config = {
        .tx_gpio = lua_ir_table_int(L, 1, "tx_gpio", -1),
        .rx_gpio = lua_ir_table_int(L, 1, "rx_gpio", -1),
        .ctrl_gpio = lua_ir_table_int(L, 1, "ctrl_gpio", -1),
        .ctrl_active_level = lua_ir_table_int(L, 1, "ctrl_active_level", 0),
        .tx_invert = lua_ir_table_bool(L, 1, "tx_invert", false),
        .rx_invert = lua_ir_table_bool(L, 1, "rx_invert", true),
        .carrier_hz = lua_ir_table_u32(L, 1, "carrier_hz", CONFIG_LUA_MODULE_IR_DEFAULT_CARRIER_HZ),
        .carrier_duty = lua_ir_table_float(L, 1, "carrier_duty", 0.33f),
        .tx_resolution_hz = lua_ir_table_u32(L, 1, "tx_resolution_hz", CONFIG_LUA_MODULE_IR_DEFAULT_TX_RESOLUTION_HZ),
        .rx_resolution_hz = lua_ir_table_u32(L, 1, "rx_resolution_hz", CONFIG_LUA_MODULE_IR_DEFAULT_RX_RESOLUTION_HZ),
        .rx_max_symbols = lua_ir_table_int(L, 1, "rx_max_symbols", CONFIG_LUA_MODULE_IR_RX_MAX_SYMBOLS),
        .signal_range_min_ns = lua_ir_table_u32(L, 1, "signal_range_min_ns", LUA_IR_DEFAULT_RX_MIN_NS),
        .signal_range_max_ns = lua_ir_table_u32(L, 1, "signal_range_max_ns", LUA_IR_DEFAULT_RX_MAX_NS),
    };
    lua_getfield(L, 1, "name");
    const char *name = luaL_optstring(L, -1, LUA_IR_DEFAULT_NAME);
    if (strlen(name) >= LUA_IR_MAX_NAME_LEN) return luaL_error(L, "ir.device: name is too long");
    char name_copy[LUA_IR_MAX_NAME_LEN];
    snprintf(name_copy, sizeof(name_copy), "%s", name);
    lua_pop(L, 1);

    lua_ir_device_ud_t *ud = lua_newuserdata(L, sizeof(*ud));
    memset(ud, 0, sizeof(*ud));
    luaL_getmetatable(L, LUA_IR_DEVICE_METATABLE);
    lua_setmetatable(L, -2);
    snprintf(ud->name, sizeof(ud->name), "%s", name_copy);

    char key_buffers[3][CLAW_HW_KEY_SIZE];
    const char *keys[3];
    size_t key_count = 0;
    int gpios[3] = { config.tx_gpio, config.rx_gpio, config.ctrl_gpio };
    for (size_t i = 0; i < 3; i++) {
        if (gpios[i] < 0) continue;
        keys[key_count] = claw_hw_key_gpio(key_buffers[key_count], CLAW_HW_KEY_SIZE, gpios[i]);
        key_count++;
    }
    esp_err_t err = ir_device_create(&config, &ud->device);
    if (err != ESP_OK) return luaL_error(L, "ir.device: invalid configuration: %s", esp_err_to_name(err));
    const char *owner = cap_lua_current_owner_tag();
    err = cap_lua_hw_claim(L, &ud->lease, owner ? owner : "lua/ir", keys, key_count);
    if (err != ESP_OK) {
        esp_err_t delete_err = ir_device_delete(ud->device);
        if (delete_err != ESP_OK) ESP_LOGE(TAG, "IR device cleanup failed: %s", esp_err_to_name(delete_err));
        ud->device = NULL;
        return luaL_error(L, "ir.device: resource claim failed: %s", esp_err_to_name(err));
    }
    ud->lease_claimed = true;
    ESP_LOGI(TAG, "IR device created: tx=%d rx=%d ctrl=%d", config.tx_gpio, config.rx_gpio, config.ctrl_gpio);
    return 1;
}

void lua_ir_device_register(lua_State *L)
{
    luaL_checktype(L, -1, LUA_TTABLE);
    if (luaL_newmetatable(L, LUA_IR_RX_GUARD_METATABLE)) {
        lua_pushcfunction(L, lua_ir_rx_guard_gc);
        lua_setfield(L, -2, "__gc");
    }
    lua_pop(L, 1);
    if (luaL_newmetatable(L, LUA_IR_DEVICE_METATABLE)) {
        lua_pushcfunction(L, lua_ir_device_gc); lua_setfield(L, -2, "__gc");
        lua_pushvalue(L, -1); lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, lua_ir_device_send); lua_setfield(L, -2, "send");
        lua_pushcfunction(L, lua_ir_device_receive); lua_setfield(L, -2, "receive");
        lua_pushcfunction(L, lua_ir_device_start_receive); lua_setfield(L, -2, "start_receive");
        lua_pushcfunction(L, lua_ir_device_read); lua_setfield(L, -2, "read");
        lua_pushcfunction(L, lua_ir_device_stop_receive); lua_setfield(L, -2, "stop_receive");
        lua_pushcfunction(L, lua_ir_device_info); lua_setfield(L, -2, "info");
        lua_pushcfunction(L, lua_ir_device_close); lua_setfield(L, -2, "close");
    }
    lua_pop(L, 1);
    lua_pushcfunction(L, lua_ir_device_new);
    lua_setfield(L, -2, "device");
}
