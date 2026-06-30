/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_ble_priv.h"

#include "cap_lua.h"

const char *TAG = "lua_ble";

lua_ble_runtime_t s_ble_rt = {
    .event_callback_ref = LUA_NOREF,
    .preferred_mtu = LUA_BLE_DEFAULT_MTU,
    .adv_name = LUA_BLE_DEFAULT_NAME,
};

static void lua_ble_push_connection(lua_State *L, const lua_module_ble_connection_t *conn)
{
    lua_newtable(L);
    lua_pushinteger(L, conn->conn_index);
    lua_setfield(L, -2, "conn_index");
    lua_pushboolean(L, conn->connected);
    lua_setfield(L, -2, "connected");
    if (!conn->connected) {
        return;
    }
    lua_pushlstring(L, (const char *)conn->peer_addr, sizeof(conn->peer_addr));
    lua_setfield(L, -2, "peer_addr");
    lua_pushstring(L, lua_ble_addr_type_name(conn->peer_addr_type));
    lua_setfield(L, -2, "peer_addr_type");
    lua_pushboolean(L, conn->encrypted);
    lua_setfield(L, -2, "encrypted");
    lua_pushboolean(L, conn->authenticated);
    lua_setfield(L, -2, "authenticated");
    lua_pushboolean(L, conn->bonded);
    lua_setfield(L, -2, "bonded");
    lua_pushinteger(L, conn->key_size);
    lua_setfield(L, -2, "key_size");
    lua_pushinteger(L, conn->mtu);
    lua_setfield(L, -2, "mtu");
    lua_pushinteger(L, conn->conn_handle);
    lua_setfield(L, -2, "conn_handle");
}

static int lua_ble_stats(lua_State *L)
{
    int ret;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "char");
        bool has_char = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (has_char) {
            return lua_ble_gatts_stats_char(L, 1);
        }
    }

    lua_ble_runtime_lock();
    lua_newtable(L);
    lua_pushlstring(L, (const char *)s_own_addr, sizeof(s_own_addr));
    lua_setfield(L, -2, "mac");
    lua_pushinteger(L, s_preferred_mtu);
    lua_setfield(L, -2, "preferred_mtu");
    lua_pushboolean(L, s_advertising_requested);
    lua_setfield(L, -2, "adv_requested");
    lua_pushboolean(L, s_advertising);
    lua_setfield(L, -2, "adv_active");
    lua_pushinteger(L, s_event_dropped);
    lua_setfield(L, -2, "event_dropped");

    lua_newtable(L);
    for (uint8_t i = 0; i < LUA_BLE_MAX_CONNECTIONS; i++) {
        lua_ble_push_connection(L, &s_conns[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "connections");
    ret = 1;
    lua_ble_runtime_unlock();
    return ret;
}

static void lua_ble_register_adv_type(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, 0x01); lua_setfield(L, -2, "FLAGS");
    lua_pushinteger(L, 0x02); lua_setfield(L, -2, "INCOMPLETE16");
    lua_pushinteger(L, 0x03); lua_setfield(L, -2, "COMPLETE16");
    lua_pushinteger(L, 0x04); lua_setfield(L, -2, "INCOMPLETE32");
    lua_pushinteger(L, 0x05); lua_setfield(L, -2, "COMPLETE32");
    lua_pushinteger(L, 0x06); lua_setfield(L, -2, "INCOMPLETE128");
    lua_pushinteger(L, 0x07); lua_setfield(L, -2, "COMPLETE128");
    lua_pushinteger(L, 0x08); lua_setfield(L, -2, "SHORTENED_LOCAL_NAME");
    lua_pushinteger(L, 0x09); lua_setfield(L, -2, "COMPLETE_LOCAL_NAME");
    lua_pushinteger(L, 0x0A); lua_setfield(L, -2, "TX_POWER");
    lua_pushinteger(L, 0x19); lua_setfield(L, -2, "APPEARANCE");
    lua_pushinteger(L, 0x1A); lua_setfield(L, -2, "ADVERTISING_INTERVAL");
    lua_pushinteger(L, 0x14); lua_setfield(L, -2, "SERVICE_UUIDS_16BIT");
    lua_pushinteger(L, 0x1F); lua_setfield(L, -2, "SERVICE_UUIDS_32BIT");
    lua_pushinteger(L, 0x15); lua_setfield(L, -2, "SERVICE_UUIDS_128BIT");
    lua_pushinteger(L, 0x16); lua_setfield(L, -2, "SERVICE_DATA_16BIT");
    lua_pushinteger(L, 0x20); lua_setfield(L, -2, "SERVICE_DATA_32BIT");
    lua_pushinteger(L, 0x21); lua_setfield(L, -2, "SERVICE_DATA_128BIT");
    lua_pushinteger(L, 0x24); lua_setfield(L, -2, "URI");
    lua_pushinteger(L, 0xFF); lua_setfield(L, -2, "MANUFACTURER_SPECIFIC_DATA");
    lua_setfield(L, -2, "ADV_TYPE");
}

static void lua_ble_register_perm(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, 0x08); lua_setfield(L, -2, "READ");
    lua_pushinteger(L, 0x10); lua_setfield(L, -2, "WRITE");
    lua_pushinteger(L, 0x20); lua_setfield(L, -2, "INDICATE");
    lua_pushinteger(L, 0x40); lua_setfield(L, -2, "NOTIFY");
    lua_pushinteger(L, 0x80); lua_setfield(L, -2, "WRITE_NO_RSP");
    lua_setfield(L, -2, "PERM");
}

static void lua_ble_register_addr_mode(lua_State *L)
{
    lua_newtable(L);
    lua_pushstring(L, "public"); lua_setfield(L, -2, "PUBLIC");
    lua_pushstring(L, "random"); lua_setfield(L, -2, "RANDOM");
    lua_setfield(L, -2, "ADDR_MODE");
}

int luaopen_ble(lua_State *L)
{
    if (lua_ble_runtime_ensure() != ESP_OK) {
        return luaL_error(L, "ble runtime init failed");
    }
    lua_newtable(L);
    lua_pushcfunction(L, lua_ble_init);
    lua_setfield(L, -2, "init");
    lua_pushcfunction(L, lua_ble_deinit);
    lua_setfield(L, -2, "deinit");
    lua_pushcfunction(L, lua_ble_set_name);
    lua_setfield(L, -2, "set_name");
    lua_pushcfunction(L, lua_ble_set_max_mtu);
    lua_setfield(L, -2, "set_max_mtu");
    lua_pushcfunction(L, lua_ble_adv_start);
    lua_setfield(L, -2, "adv_start");
    lua_pushcfunction(L, lua_ble_adv_stop);
    lua_setfield(L, -2, "adv_stop");
    lua_pushcfunction(L, lua_ble_disconnect);
    lua_setfield(L, -2, "disconnect");
    lua_pushcfunction(L, lua_ble_on_event);
    lua_setfield(L, -2, "on_event");
    lua_pushcfunction(L, lua_ble_process_events);
    lua_setfield(L, -2, "process_events");
    lua_pushcfunction(L, lua_ble_stats);
    lua_setfield(L, -2, "stats");
    lua_pushcfunction(L, lua_ble_gatts_define);
    lua_setfield(L, -2, "gatts_define");
    lua_pushcfunction(L, lua_ble_gatts_set_value);
    lua_setfield(L, -2, "gatts_set_value");
    lua_pushcfunction(L, lua_ble_notify);
    lua_setfield(L, -2, "notify");
    lua_pushcfunction(L, lua_ble_indicate);
    lua_setfield(L, -2, "indicate");
    lua_pushcfunction(L, lua_ble_smp_config);
    lua_setfield(L, -2, "smp_config");
    lua_ble_register_adv_type(L);
    lua_ble_register_perm(L);
    lua_ble_register_addr_mode(L);
    return 1;
}

esp_err_t lua_module_ble_register(void)
{
    return cap_lua_register_module(LUA_MODULE_BLE_NAME, luaopen_ble);
}
