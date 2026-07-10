/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_ble_priv.h"

#include "nvs_flash.h"

esp_err_t lua_ble_runtime_ensure(void)
{
    if (s_ble_rt.mutex == NULL) {
        s_ble_rt.mutex = xSemaphoreCreateRecursiveMutex();
        if (s_ble_rt.mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void lua_ble_runtime_lock(void)
{
    if (lua_ble_runtime_ensure() != ESP_OK) {
        return;
    }
    (void)xSemaphoreTakeRecursive(s_ble_rt.mutex, portMAX_DELAY);
}

void lua_ble_runtime_unlock(void)
{
    if (s_ble_rt.mutex) {
        (void)xSemaphoreGiveRecursive(s_ble_rt.mutex);
    }
}

static const char *lua_ble_err_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return NULL;
    case LUA_BLE_ERR_REBOOT_REQUIRED:
        return "ble_reboot_required";
    case ESP_ERR_INVALID_STATE:
        return "ble_invalid_state";
    case ESP_ERR_NO_MEM:
        return "ble_no_mem";
    case ESP_ERR_TIMEOUT:
        return "ble_timeout";
    case ESP_ERR_NVS_NO_FREE_PAGES:
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return "ble_nvs_needs_recovery";
    case ESP_ERR_INVALID_ARG:
        return "ble_invalid_arg";
    case ESP_ERR_NOT_SUPPORTED:
        return "ble_unsupported";
    case ESP_ERR_INVALID_SIZE:
        return "ble_adv_data_too_long";
    case ESP_ERR_NOT_FOUND:
        return "ble_conn_not_found";
    default:
        return "ble_resource_busy";
    }
}

int lua_ble_push_ok_or_err(lua_State *L, esp_err_t err)
{
    const char *name = lua_ble_err_name(err);

    if (err == ESP_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, name ? name : "ble_resource_busy");
    return 2;
}

int lua_ble_push_err_string(lua_State *L, const char *err)
{
    lua_pushnil(L);
    lua_pushstring(L, err);
    return 2;
}

bool lua_ble_table_has_field(lua_State *L, int index, const char *field)
{
    bool has;

    lua_getfield(L, index, field);
    has = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return has;
}

const char *lua_ble_addr_type_name(uint8_t addr_type)
{
    switch (addr_type) {
    case BLE_ADDR_PUBLIC:
    case BLE_ADDR_PUBLIC_ID:
        return "public";
    case BLE_ADDR_RANDOM:
    case BLE_ADDR_RANDOM_ID:
    default:
        return "random";
    }
}

uint8_t lua_ble_slot_count(void)
{
    uint8_t count = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
    return count > LUA_BLE_MAX_CONNECTIONS ? LUA_BLE_MAX_CONNECTIONS : count;
}

bool lua_ble_has_connection(void)
{
    return lua_ble_connection_count() > 0;
}

int lua_ble_connection_count(void)
{
    uint8_t slots = lua_ble_slot_count();
    int count = 0;

    lua_ble_runtime_lock();
    for (uint8_t i = 0; i < slots; i++) {
        if (s_conns[i].connected) {
            count++;
        }
    }
    lua_ble_runtime_unlock();
    return count;
}

int lua_ble_find_conn_index_by_handle(uint16_t conn_handle)
{
    uint8_t slots = lua_ble_slot_count();
    int found = -1;

    lua_ble_runtime_lock();
    for (uint8_t i = 0; i < slots; i++) {
        if (s_conns[i].connected && s_conns[i].conn_handle == conn_handle) {
            found = i;
            break;
        }
    }
    lua_ble_runtime_unlock();
    return found;
}

static int lua_ble_alloc_conn_index(void)
{
    uint8_t slots = lua_ble_slot_count();

    for (uint8_t i = 0; i < slots; i++) {
        if (!s_conns[i].connected) {
            return i;
        }
    }
    return -1;
}

void lua_ble_connection_init_slots(void)
{
    lua_ble_runtime_lock();
    for (uint8_t i = 0; i < LUA_BLE_MAX_CONNECTIONS; i++) {
        memset(&s_conns[i], 0, sizeof(s_conns[i]));
        s_conns[i].conn_index = i;
        s_conns[i].conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_conns[i].mtu = s_preferred_mtu;
    }
    lua_ble_runtime_unlock();
}

static void lua_ble_notify_subscriptions_clear(void)
{
    memset(s_notify_subscriptions, 0, sizeof(s_notify_subscriptions));
}

void lua_ble_connection_clear(void)
{
    lua_ble_runtime_lock();
    lua_ble_connection_init_slots();
    lua_ble_notify_subscriptions_clear();
    for (uint8_t i = 0; i < LUA_BLE_MAX_CONNECTIONS; i++) {
        lua_ble_gatts_clear_conn_state(i);
    }
    lua_ble_runtime_unlock();
}

void lua_ble_connection_clear_index(uint8_t conn_index)
{
    lua_ble_runtime_lock();
    if (conn_index >= LUA_BLE_MAX_CONNECTIONS) {
        lua_ble_runtime_unlock();
        return;
    }
    memset(&s_conns[conn_index], 0, sizeof(s_conns[conn_index]));
    s_conns[conn_index].conn_index = conn_index;
    s_conns[conn_index].conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_conns[conn_index].mtu = s_preferred_mtu;
    for (size_t i = 0; i < LUA_BLE_NOTIFY_SUBSCRIPTION_MAX; i++) {
        if (s_notify_subscriptions[i].attr_handle != 0 &&
            s_notify_subscriptions[i].conn_index == conn_index) {
            memset(&s_notify_subscriptions[i], 0, sizeof(s_notify_subscriptions[i]));
        }
    }
    lua_ble_gatts_clear_conn_state(conn_index);
    lua_ble_runtime_unlock();
}

void lua_ble_connection_refresh_security(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int conn_index;
    int rc;

    rc = ble_gap_conn_find(conn_handle, &desc);
    if (rc != 0) {
        ESP_LOGW(TAG, "connection security lookup failed conn_handle=%u rc=%d", conn_handle, rc);
        return;
    }

    conn_index = lua_ble_find_conn_index_by_handle(conn_handle);
    if (conn_index < 0) {
        return;
    }

    lua_ble_runtime_lock();
    s_conns[conn_index].encrypted = desc.sec_state.encrypted;
    s_conns[conn_index].authenticated = desc.sec_state.authenticated;
    s_conns[conn_index].bonded = desc.sec_state.bonded;
    s_conns[conn_index].key_size = desc.sec_state.key_size;
    lua_ble_runtime_unlock();
}

void lua_ble_connection_update_security(uint16_t conn_handle, const char *reason)
{
    int conn_index;
    bool encrypted;
    bool authenticated;
    bool bonded;
    uint8_t key_size;

    lua_ble_connection_refresh_security(conn_handle);
    conn_index = lua_ble_find_conn_index_by_handle(conn_handle);
    if (conn_index < 0) {
        return;
    }
    lua_ble_runtime_lock();
    encrypted = s_conns[conn_index].encrypted;
    authenticated = s_conns[conn_index].authenticated;
    bonded = s_conns[conn_index].bonded;
    key_size = s_conns[conn_index].key_size;
    lua_ble_runtime_unlock();

    lua_ble_event_t *event = lua_ble_event_alloc();
    if (!event) {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_SECURITY_CHANGED);
        return;
    }
    event->type = LUA_BLE_EVENT_SECURITY_CHANGED;
    event->conn_index = (uint8_t)conn_index;
    event->conn_handle = conn_handle;
    event->encrypted = encrypted;
    event->authenticated = authenticated;
    event->bonded = bonded;
    event->key_size = key_size;
    strlcpy(event->reason, reason ? reason : "enc_change", sizeof(event->reason));
    lua_ble_event_enqueue(event);
}

int lua_ble_connection_set_connected(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int conn_index;
    uint8_t peer_addr_type;
    uint8_t peer_addr[6];

    lua_ble_runtime_lock();
    conn_index = lua_ble_alloc_conn_index();
    if (conn_index < 0) {
        lua_ble_runtime_unlock();
        return -1;
    }

    memset(&s_conns[conn_index], 0, sizeof(s_conns[conn_index]));
    s_conns[conn_index].connected = true;
    s_conns[conn_index].conn_index = (uint8_t)conn_index;
    s_conns[conn_index].conn_handle = conn_handle;
    s_conns[conn_index].mtu = ble_att_mtu(conn_handle);
    if (s_conns[conn_index].mtu == 0) {
        s_conns[conn_index].mtu = s_preferred_mtu;
    }
    if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        s_conns[conn_index].peer_addr_type = desc.peer_id_addr.type;
        memcpy(s_conns[conn_index].peer_addr, desc.peer_id_addr.val, sizeof(s_conns[conn_index].peer_addr));
    }
    lua_ble_connection_refresh_security(conn_handle);

    peer_addr_type = s_conns[conn_index].peer_addr_type;
    memcpy(peer_addr, s_conns[conn_index].peer_addr, sizeof(peer_addr));
    lua_ble_runtime_unlock();

    lua_ble_event_t *event = lua_ble_event_alloc();
    if (!event) {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_CONNECTED);
        return conn_index;
    }
    event->type = LUA_BLE_EVENT_CONNECTED;
    event->conn_index = (uint8_t)conn_index;
    event->conn_handle = conn_handle;
    event->peer_addr_type = peer_addr_type;
    memcpy(event->peer_addr, peer_addr, sizeof(event->peer_addr));
    lua_ble_event_enqueue(event);
    return conn_index;
}

void lua_ble_notify_subscription_update(uint8_t conn_index, uint16_t attr_handle, bool notify_enabled,
                                        bool indicate_enabled)
{
    size_t empty_index = LUA_BLE_NOTIFY_SUBSCRIPTION_MAX;

    if (conn_index >= LUA_BLE_MAX_CONNECTIONS || attr_handle == 0) {
        return;
    }

    lua_ble_runtime_lock();
    for (size_t i = 0; i < LUA_BLE_NOTIFY_SUBSCRIPTION_MAX; i++) {
        if (s_notify_subscriptions[i].attr_handle == attr_handle &&
            s_notify_subscriptions[i].conn_index == conn_index) {
            if (!notify_enabled && !indicate_enabled) {
                memset(&s_notify_subscriptions[i], 0, sizeof(s_notify_subscriptions[i]));
                lua_ble_runtime_unlock();
                return;
            }
            s_notify_subscriptions[i].notify_enabled = notify_enabled;
            s_notify_subscriptions[i].indicate_enabled = indicate_enabled;
            lua_ble_runtime_unlock();
            return;
        }
        if (empty_index == LUA_BLE_NOTIFY_SUBSCRIPTION_MAX &&
            s_notify_subscriptions[i].attr_handle == 0) {
            empty_index = i;
        }
    }

    if (!notify_enabled && !indicate_enabled) {
        lua_ble_runtime_unlock();
        return;
    }
    if (empty_index == LUA_BLE_NOTIFY_SUBSCRIPTION_MAX) {
        ESP_LOGW(TAG, "notify subscription table full attr_handle=%u", attr_handle);
        lua_ble_runtime_unlock();
        return;
    }
    s_notify_subscriptions[empty_index].attr_handle = attr_handle;
    s_notify_subscriptions[empty_index].conn_index = conn_index;
    s_notify_subscriptions[empty_index].notify_enabled = notify_enabled;
    s_notify_subscriptions[empty_index].indicate_enabled = indicate_enabled;
    lua_ble_runtime_unlock();
}

bool lua_module_ble_is_notify_subscribed(uint8_t conn_index, uint16_t attr_handle)
{
    if (conn_index >= LUA_BLE_MAX_CONNECTIONS || attr_handle == 0) {
        return false;
    }
    lua_ble_runtime_lock();
    for (size_t i = 0; i < LUA_BLE_NOTIFY_SUBSCRIPTION_MAX; i++) {
        if (s_notify_subscriptions[i].attr_handle == attr_handle &&
            s_notify_subscriptions[i].conn_index == conn_index) {
            bool enabled = s_notify_subscriptions[i].notify_enabled;
            lua_ble_runtime_unlock();
            return enabled;
        }
    }
    lua_ble_runtime_unlock();
    return false;
}

bool lua_module_ble_is_indicate_subscribed(uint8_t conn_index, uint16_t attr_handle)
{
    if (conn_index >= LUA_BLE_MAX_CONNECTIONS || attr_handle == 0) {
        return false;
    }
    lua_ble_runtime_lock();
    for (size_t i = 0; i < LUA_BLE_NOTIFY_SUBSCRIPTION_MAX; i++) {
        if (s_notify_subscriptions[i].attr_handle == attr_handle &&
            s_notify_subscriptions[i].conn_index == conn_index) {
            bool enabled = s_notify_subscriptions[i].indicate_enabled;
            lua_ble_runtime_unlock();
            return enabled;
        }
    }
    lua_ble_runtime_unlock();
    return false;
}

