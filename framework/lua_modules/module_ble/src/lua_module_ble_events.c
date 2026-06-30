/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_heap_caps.h"
#include <inttypes.h>

#include "lua_module_ble_priv.h"

lua_ble_event_t *lua_ble_event_alloc(void)
{
    return heap_caps_calloc_prefer(1,
                                   sizeof(lua_ble_event_t),
                                   2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lua_ble_event_free(lua_ble_event_t *event)
{
    if (!event) {
        return;
    }
    heap_caps_free(event->data);
    heap_caps_free(event);
}

esp_err_t lua_ble_event_set_data(lua_ble_event_t *event, const uint8_t *data, uint16_t data_len)
{
    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }
    heap_caps_free(event->data);
    event->data = NULL;
    event->data_len = 0;
    if (data_len == 0) {
        return ESP_OK;
    }
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    event->data = heap_caps_malloc_prefer(data_len,
                                          2,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!event->data) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(event->data, data, data_len);
    event->data_len = data_len;
    return ESP_OK;
}

void lua_ble_events_clear(void)
{
    lua_ble_event_t *item;

    lua_ble_runtime_lock();
    if (!s_event_queue) {
        lua_ble_runtime_unlock();
        return;
    }
    while (xQueueReceive(s_event_queue, &item, 0) == pdTRUE) {
        lua_ble_event_free(item);
    }
    lua_ble_runtime_unlock();
}

void lua_ble_event_enqueue(lua_ble_event_t *event)
{
    uint32_t dropped_total = 0;
    bool dropped_event = false;

    if (!event) {
        return;
    }
    lua_ble_runtime_lock();
    if (!s_event_queue) {
        lua_ble_runtime_unlock();
        lua_ble_event_free(event);
        return;
    }
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        s_event_dropped++;
        dropped_total = s_event_dropped;
        dropped_event = true;
    }
    lua_ble_runtime_unlock();
    if (dropped_event) {
        ESP_LOGW(TAG, "BLE event queue full, dropped event type=%d total=%" PRIu32,
                 event->type, dropped_total);
        lua_ble_event_free(event);
    }
}

void lua_ble_event_enqueue_force(lua_ble_event_t *event)
{
    lua_ble_event_t *dropped = NULL;
    int forced_type = 0;
    uint32_t dropped_total = 0;
    bool dropped_oldest = false;

    if (!event) {
        return;
    }
    forced_type = event->type;
    lua_ble_runtime_lock();
    if (!s_event_queue) {
        lua_ble_runtime_unlock();
        lua_ble_event_free(event);
        return;
    }
    if (xQueueSend(s_event_queue, &event, 0) == pdTRUE) {
        lua_ble_runtime_unlock();
        return;
    }

    if (xQueueReceive(s_event_queue, &dropped, 0) == pdTRUE) {
        s_event_dropped++;
        dropped_total = s_event_dropped;
        dropped_oldest = true;
    }
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        lua_ble_runtime_unlock();
        ESP_LOGW(TAG, "BLE event queue full, force enqueue failed event type=%d total=%" PRIu32,
                 event->type, dropped_total);
        lua_ble_event_free(dropped);
        lua_ble_event_free(event);
        return;
    }
    lua_ble_runtime_unlock();
    if (dropped_oldest) {
        ESP_LOGW(TAG, "BLE event queue full, dropped oldest event type=%d for forced event type=%d total=%" PRIu32,
                 dropped ? dropped->type : -1, forced_type, dropped_total);
        lua_ble_event_free(dropped);
    }
}

void lua_ble_event_simple_adv(lua_ble_event_type_t type, const char *reason, int error_code)
{
    lua_ble_event_t *event = lua_ble_event_alloc();

    if (!event) {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d", type);
        return;
    }
    event->type = type;
    event->error_code = error_code;
    if (reason) {
        strlcpy(event->reason, reason, sizeof(event->reason));
    }
    lua_ble_event_enqueue(event);
}

static const char *lua_ble_event_type_name(lua_ble_event_type_t type)
{
    switch (type) {
    case LUA_BLE_EVENT_ADV_STARTED:
        return "adv_started";
    case LUA_BLE_EVENT_ADV_STOPPED:
        return "adv_stopped";
    case LUA_BLE_EVENT_CONNECTED:
        return "connected";
    case LUA_BLE_EVENT_DISCONNECTED:
        return "disconnected";
    case LUA_BLE_EVENT_MTU_CHANGED:
        return "mtu_changed";
    case LUA_BLE_EVENT_SECURITY_CHANGED:
        return "security_changed";
    case LUA_BLE_EVENT_SUBSCRIBE_CHANGED:
        return "subscribe_changed";
    case LUA_BLE_EVENT_GATTS_READ:
        return "gatts_read";
    case LUA_BLE_EVENT_GATTS_WRITE:
        return "gatts_write";
    case LUA_BLE_EVENT_NOTIFY_COMPLETE:
        return "notify_complete";
    case LUA_BLE_EVENT_INDICATE_COMPLETE:
        return "indicate_complete";
    default:
        return "unknown";
    }
}

static void lua_ble_event_push_gatt_fields(lua_State *L, const lua_ble_event_t *event)
{
    lua_pushinteger(L, event->conn_index);
    lua_setfield(L, -2, "conn_index");
    if (event->service_id[0]) {
        lua_pushstring(L, event->service_id);
        lua_setfield(L, -2, "service_id");
    }
    if (event->characteristic_id[0]) {
        lua_pushstring(L, event->characteristic_id);
        lua_setfield(L, -2, "characteristic_id");
    }
    if (event->uuid_service[0]) {
        lua_pushstring(L, event->uuid_service);
        lua_setfield(L, -2, "uuid_service");
    }
    if (event->uuid_characteristic[0]) {
        lua_pushstring(L, event->uuid_characteristic);
        lua_setfield(L, -2, "uuid_characteristic");
    }
    if (event->uuid_descriptor[0]) {
        lua_pushstring(L, event->uuid_descriptor);
        lua_setfield(L, -2, "uuid_descriptor");
    }
}

static void lua_ble_event_push_common_fields(lua_State *L, const lua_ble_event_t *event)
{
    lua_pushstring(L, lua_ble_event_type_name(event->type));
    lua_setfield(L, -2, "type");

    if (event->reason[0]) {
        lua_pushstring(L, event->reason);
        lua_setfield(L, -2, "reason");
    }
    if (event->error_code != 0) {
        lua_pushinteger(L, event->error_code);
        lua_setfield(L, -2, "error_code");
    }
}

static void lua_ble_event_push_conn_index(lua_State *L, const lua_ble_event_t *event)
{
    lua_pushinteger(L, event->conn_index);
    lua_setfield(L, -2, "conn_index");
}

static void lua_ble_event_push_connected(lua_State *L, const lua_ble_event_t *event)
{
    lua_ble_event_push_conn_index(L, event);
    lua_pushinteger(L, event->conn_handle);
    lua_setfield(L, -2, "conn_handle");
    lua_pushlstring(L, (const char *)event->peer_addr, sizeof(event->peer_addr));
    lua_setfield(L, -2, "peer_addr");
    lua_pushstring(L, lua_ble_addr_type_name(event->peer_addr_type));
    lua_setfield(L, -2, "peer_addr_type");
}

static void lua_ble_event_push_mtu(lua_State *L, const lua_ble_event_t *event)
{
    lua_ble_event_push_conn_index(L, event);
    lua_pushinteger(L, event->mtu);
    lua_setfield(L, -2, "mtu");
}

static void lua_ble_event_push_security(lua_State *L, const lua_ble_event_t *event)
{
    lua_ble_event_push_conn_index(L, event);
    lua_pushboolean(L, event->encrypted);
    lua_setfield(L, -2, "encrypted");
    lua_pushboolean(L, event->authenticated);
    lua_setfield(L, -2, "authenticated");
    lua_pushboolean(L, event->bonded);
    lua_setfield(L, -2, "bonded");
    lua_pushinteger(L, event->key_size);
    lua_setfield(L, -2, "key_size");
}

static void lua_ble_event_push_subscribe(lua_State *L, const lua_ble_event_t *event)
{
    lua_ble_event_push_gatt_fields(L, event);
    lua_pushboolean(L, event->notify);
    lua_setfield(L, -2, "notify");
    lua_pushboolean(L, event->indicate);
    lua_setfield(L, -2, "indicate");
}

static void lua_ble_event_push_gatts_read(lua_State *L, const lua_ble_event_t *event)
{
    lua_ble_event_push_gatt_fields(L, event);
    lua_pushinteger(L, event->offset);
    lua_setfield(L, -2, "offset");
}

static void lua_ble_event_push_gatts_write(lua_State *L, const lua_ble_event_t *event)
{
    lua_ble_event_push_gatts_read(L, event);
    lua_pushlstring(L, event->data ? (const char *)event->data : "", event->data_len);
    lua_setfield(L, -2, "data");
}

static void lua_ble_event_push_tx_complete(lua_State *L, const lua_ble_event_t *event)
{
    lua_ble_event_push_gatt_fields(L, event);
    lua_pushstring(L, event->status[0] ? event->status : "ok");
    lua_setfield(L, -2, "status");
}

static void lua_ble_event_push_specific_fields(lua_State *L, const lua_ble_event_t *event)
{
    switch (event->type) {
    case LUA_BLE_EVENT_CONNECTED:
        lua_ble_event_push_connected(L, event);
        break;
    case LUA_BLE_EVENT_DISCONNECTED:
        lua_ble_event_push_conn_index(L, event);
        break;
    case LUA_BLE_EVENT_MTU_CHANGED:
        lua_ble_event_push_mtu(L, event);
        break;
    case LUA_BLE_EVENT_SECURITY_CHANGED:
        lua_ble_event_push_security(L, event);
        break;
    case LUA_BLE_EVENT_SUBSCRIBE_CHANGED:
        lua_ble_event_push_subscribe(L, event);
        break;
    case LUA_BLE_EVENT_GATTS_READ:
        lua_ble_event_push_gatts_read(L, event);
        break;
    case LUA_BLE_EVENT_GATTS_WRITE:
        lua_ble_event_push_gatts_write(L, event);
        break;
    case LUA_BLE_EVENT_NOTIFY_COMPLETE:
    case LUA_BLE_EVENT_INDICATE_COMPLETE:
        lua_ble_event_push_tx_complete(L, event);
        break;
    default:
        break;
    }
}

static void lua_ble_event_push_lua(lua_State *L, const lua_ble_event_t *event)
{
    lua_newtable(L);
    lua_ble_event_push_common_fields(L, event);
    lua_ble_event_push_specific_fields(L, event);
}

static bool lua_ble_event_receive(lua_ble_event_t **event, TickType_t wait)
{
    QueueHandle_t queue = s_event_queue;

    if (!queue || s_event_callback_ref == LUA_NOREF) {
        return false;
    }
    return xQueueReceive(queue, event, wait) == pdTRUE;
}

static bool lua_ble_event_push_callback(lua_State *L)
{
    int callback_ref = s_event_callback_ref;

    if (callback_ref == LUA_NOREF) {
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, callback_ref);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        if (s_event_callback_ref == callback_ref) {
            s_event_callback_ref = LUA_NOREF;
        }
        lua_ble_events_clear();
        return false;
    }
    return true;
}

int lua_ble_on_event(lua_State *L)
{
    if (lua_isnil(L, 1)) {
        if (s_event_callback_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, s_event_callback_ref);
            s_event_callback_ref = LUA_NOREF;
        }
        lua_ble_events_clear();
        lua_pushboolean(L, 1);
        return 1;
    }

    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    if (s_event_callback_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_event_callback_ref);
    }
    s_event_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_ble_events_clear();
    lua_pushboolean(L, 1);
    return 1;
}

void lua_ble_events_drain(lua_State *L)
{
    lua_ble_event_t *event;

    while (lua_ble_event_receive(&event, 0)) {
        if (!lua_ble_event_push_callback(L)) {
            lua_ble_event_free(event);
            return;
        }
        lua_ble_event_push_lua(L, event);
        lua_ble_event_free(event);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            ESP_LOGE(TAG, "BLE deinit event callback error: %s", msg ? msg : "(nil)");
            lua_pop(L, 1);
        }
    }
}

int lua_ble_process_events(lua_State *L)
{
    int timeout_ms = lua_isnoneornil(L, 1) ? 0 : (int)luaL_checkinteger(L, 1);
    lua_ble_event_t *event;
    int processed = 0;
    TickType_t first_wait;

    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    if (!s_event_queue || s_event_callback_ref == LUA_NOREF) {
        lua_pushinteger(L, 0);
        return 1;
    }

    first_wait = (timeout_ms > 0) ? pdMS_TO_TICKS(timeout_ms) : 0;

    while (processed < LUA_BLE_PROCESS_EVENTS_MAX) {
        TickType_t wait = (processed == 0) ? first_wait : 0;

        if (!lua_ble_event_receive(&event, wait)) {
            break;
        }
        if (!lua_ble_event_push_callback(L)) {
            lua_ble_event_free(event);
            break;
        }
        lua_ble_event_push_lua(L, event);
        lua_ble_event_free(event);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            ESP_LOGE(TAG, "BLE event callback error: %s", msg ? msg : "(nil)");
            lua_pop(L, 1);
        }
        processed++;
    }

    lua_pushinteger(L, processed);
    return 1;
}

