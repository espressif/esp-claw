/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_ble_priv.h"

void ble_store_config_init(void);

static lua_ble_smp_config_t s_smp_config = {
    .bonding = true,
    .secure_connections = true,
    .mitm = false,
    .io_cap = BLE_SM_IO_CAP_NO_IO,
    .key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID,
    .repeat_pairing = LUA_BLE_SMP_REPEAT_PAIRING_DELETE_RETRY,
    .require_bond_persist = false,
};

static bool lua_ble_smp_get_bool_field(lua_State *L, int table_index, const char *field, bool default_value)
{
    bool value = default_value;

    table_index = lua_absindex(L, table_index);
    lua_getfield(L, table_index, field);
    if (!lua_isnil(L, -1)) {
        luaL_checktype(L, -1, LUA_TBOOLEAN);
        value = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    return value;
}

static int lua_ble_smp_parse_io_cap(lua_State *L, const char *io_cap, uint8_t *out)
{
    if (strcmp(io_cap, "no_io") == 0) {
        *out = BLE_SM_IO_CAP_NO_IO;
    } else {
        return lua_ble_push_err_string(L, "ble_smp_unsupported_io");
    }
    return 0;
}

static int lua_ble_smp_parse_key_dist(lua_State *L, int opts_index, uint8_t *out)
{
    uint8_t key_dist = 0;

    opts_index = lua_absindex(L, opts_index);
    lua_getfield(L, opts_index, "key_dist");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }
    luaL_checktype(L, -1, LUA_TTABLE);
    if (lua_ble_smp_get_bool_field(L, lua_gettop(L), "enc", true)) {
        key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    }
    if (lua_ble_smp_get_bool_field(L, lua_gettop(L), "id", true)) {
        key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
    }
    lua_pop(L, 1);
    *out = key_dist;
    return 0;
}

int lua_ble_smp_config(lua_State *L)
{
    lua_ble_smp_config_t cfg = s_smp_config;
    int rc;

    if (s_stack_inited || s_nimble_inited || s_host_task_started || s_host_synced) {
        return lua_ble_push_err_string(L, "ble_invalid_state");
    }

    luaL_checktype(L, 1, LUA_TTABLE);
    cfg.bonding = lua_ble_smp_get_bool_field(L, 1, "bonding", cfg.bonding);
    cfg.secure_connections = lua_ble_smp_get_bool_field(L, 1, "secure_connections", cfg.secure_connections);
    cfg.mitm = lua_ble_smp_get_bool_field(L, 1, "mitm", cfg.mitm);
    cfg.require_bond_persist = lua_ble_smp_get_bool_field(L, 1, "require_bond_persist", cfg.require_bond_persist);

    lua_getfield(L, 1, "io_cap");
    if (!lua_isnil(L, -1)) {
        rc = lua_ble_smp_parse_io_cap(L, luaL_checkstring(L, -1), &cfg.io_cap);
        if (rc != 0) {
            return rc;
        }
    }
    lua_pop(L, 1);

    lua_getfield(L, 1, "repeat_pairing");
    if (!lua_isnil(L, -1)) {
        const char *repeat_pairing = luaL_checkstring(L, -1);
        if (strcmp(repeat_pairing, "ignore") == 0) {
            cfg.repeat_pairing = LUA_BLE_SMP_REPEAT_PAIRING_IGNORE;
        } else if (strcmp(repeat_pairing, "delete_retry") == 0) {
            cfg.repeat_pairing = LUA_BLE_SMP_REPEAT_PAIRING_DELETE_RETRY;
        } else {
            return luaL_error(L, "repeat_pairing must be ignore or delete_retry");
        }
    }
    lua_pop(L, 1);

    rc = lua_ble_smp_parse_key_dist(L, 1, &cfg.key_dist);
    if (rc != 0) {
        return rc;
    }

    if (cfg.mitm && cfg.io_cap == BLE_SM_IO_CAP_NO_IO) {
        return lua_ble_push_err_string(L, "ble_smp_invalid_config");
    }

    s_smp_config = cfg;
    lua_pushboolean(L, 1);
    return 1;
}

const char *lua_ble_smp_init_error(void)
{
#ifndef CONFIG_BT_NIMBLE_NVS_PERSIST
    if (s_smp_config.require_bond_persist) {
        return "ble_smp_nvs_persist_disabled";
    }
#endif
    return NULL;
}

void lua_ble_smp_apply_default_config(void)
{
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = s_smp_config.io_cap;
    ble_hs_cfg.sm_bonding = s_smp_config.bonding ? 1 : 0;
    ble_hs_cfg.sm_sc = s_smp_config.secure_connections ? 1 : 0;
    ble_hs_cfg.sm_mitm = s_smp_config.mitm ? 1 : 0;
    ble_hs_cfg.sm_our_key_dist = s_smp_config.key_dist;
    ble_hs_cfg.sm_their_key_dist = s_smp_config.key_dist;
    ble_store_config_init();
}

bool lua_ble_smp_allows_authenticated_access(void)
{
    return false;
}

int lua_ble_smp_handle_repeat_pairing(const struct ble_gap_repeat_pairing *repeat_pairing)
{
    struct ble_gap_conn_desc desc;
    int rc;

    if (!repeat_pairing) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    if (s_smp_config.repeat_pairing == LUA_BLE_SMP_REPEAT_PAIRING_IGNORE) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

    rc = ble_gap_conn_find(repeat_pairing->conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "repeat_pairing conn lookup failed conn_handle=%u rc=%d",
                 repeat_pairing->conn_handle, rc);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    rc = ble_store_util_delete_peer(&desc.peer_id_addr);
    if (rc != 0) {
        ESP_LOGW(TAG, "delete peer bond failed conn_handle=%u rc=%d", repeat_pairing->conn_handle, rc);
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    return BLE_GAP_REPEAT_PAIRING_RETRY;
}

void lua_ble_smp_handle_passkey_action(uint16_t conn_handle, const struct ble_gap_passkey_params *params)
{
    struct ble_sm_io io = { 0 };
    int rc;

    if (!params) {
        return;
    }

    switch (params->action) {
    case BLE_SM_IOACT_NUMCMP:
        io.action = params->action;
        io.numcmp_accept = 0;
        break;
    case BLE_SM_IOACT_INPUT:
    case BLE_SM_IOACT_DISP:
    case BLE_SM_IOACT_STATIC:
    case BLE_SM_IOACT_OOB:
    case BLE_SM_IOACT_OOB_SC:
    case BLE_SM_IOACT_NONE:
    default:
        ESP_LOGW(TAG, "rejecting unsupported passkey action conn_handle=%u action=%u",
                 conn_handle, params->action);
        return;
    }

    rc = ble_sm_inject_io(conn_handle, &io);
    if (rc != 0) {
        ESP_LOGW(TAG, "passkey default injection failed conn_handle=%u action=%u rc=%d",
                 conn_handle, params->action, rc);
    }
}
