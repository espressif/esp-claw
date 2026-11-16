/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>

#include "esp_heap_caps.h"

#include "lua_module_esp_now_priv.h"

lua_espnow_event_t *lua_espnow_event_alloc(void)
{
    return heap_caps_calloc_prefer(1,
                                   sizeof(lua_espnow_event_t),
                                   2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lua_espnow_event_free(lua_espnow_event_t *event)
{
    if (!event) {
        return;
    }
    heap_caps_free(event->data);
    heap_caps_free(event);
}

esp_err_t lua_espnow_event_set_data(lua_espnow_event_t *event, const uint8_t *data, size_t data_len)
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

void lua_espnow_events_clear(void)
{
    lua_espnow_event_t *item;

    lua_espnow_runtime_lock();
    if (!s_event_queue) {
        lua_espnow_runtime_unlock();
        return;
    }
    while (xQueueReceive(s_event_queue, &item, 0) == pdTRUE) {
        lua_espnow_event_free(item);
    }
    lua_espnow_runtime_unlock();
}

void lua_espnow_event_enqueue(lua_espnow_event_t *event)
{
    uint32_t dropped_total = 0;
    bool dropped_event = false;

    if (!event) {
        return;
    }
    lua_espnow_runtime_lock();
    if (!s_event_queue) {
        lua_espnow_runtime_unlock();
        lua_espnow_event_free(event);
        return;
    }
    if (xQueueSend(s_event_queue, &event, 0) != pdTRUE) {
        s_event_dropped++;
        dropped_total = s_event_dropped;
        dropped_event = true;
    }
    lua_espnow_runtime_unlock();
    if (dropped_event) {
        ESP_LOGW(TAG, "ESP-NOW event queue full, dropped event type=%d total=%" PRIu32,
                 event->type, dropped_total);
        lua_espnow_event_free(event);
    }
}

void lua_espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    lua_espnow_event_t *event;

    if (!info || !info->src_addr || len < 0) {
        return;
    }

    event = lua_espnow_event_alloc();
    if (!event) {
        ESP_LOGW(TAG, "ESP-NOW recv event alloc failed len=%d", len);
        return;
    }
    event->type = LUA_ESPNOW_EVENT_RECV;
    memcpy(event->src_mac, info->src_addr, ESP_NOW_ETH_ALEN);
    if (info->des_addr) {
        memcpy(event->dst_mac, info->des_addr, ESP_NOW_ETH_ALEN);
    }
    if (info->rx_ctrl) {
        event->rssi = info->rx_ctrl->rssi;
        event->channel = info->rx_ctrl->channel;
    }
    if (len > 0 && lua_espnow_event_set_data(event, data, (size_t)len) != ESP_OK) {
        ESP_LOGW(TAG, "ESP-NOW recv payload copy failed len=%d", len);
        lua_espnow_event_free(event);
        return;
    }

    lua_espnow_runtime_lock();
    s_espnow_rt.recv_count++;
    lua_espnow_runtime_unlock();
    lua_espnow_event_enqueue(event);
}

void lua_espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    lua_espnow_event_t *event = lua_espnow_event_alloc();

    if (!event) {
        ESP_LOGW(TAG, "ESP-NOW send event alloc failed");
        return;
    }
    event->type = LUA_ESPNOW_EVENT_SEND;
    event->send_status = (int)status;
    (void)lua_espnow_send_info_dest(tx_info, event->dst_mac);

    lua_espnow_runtime_lock();
    s_espnow_rt.send_count++;
    lua_espnow_runtime_unlock();
    lua_espnow_event_enqueue(event);
}

static void lua_espnow_event_push_lua(lua_State *L, const lua_espnow_event_t *event)
{
    lua_newtable(L);
    if (event->type == LUA_ESPNOW_EVENT_RECV) {
        lua_pushstring(L, "recv");
        lua_setfield(L, -2, "type");
        lua_pushlstring(L, (const char *)event->src_mac, sizeof(event->src_mac));
        lua_setfield(L, -2, "src_mac");
        lua_pushlstring(L, (const char *)event->dst_mac, sizeof(event->dst_mac));
        lua_setfield(L, -2, "dst_mac");
        lua_pushlstring(L, event->data ? (const char *)event->data : "", event->data_len);
        lua_setfield(L, -2, "data");
        lua_pushinteger(L, event->rssi);
        lua_setfield(L, -2, "rssi");
        lua_pushinteger(L, event->channel);
        lua_setfield(L, -2, "channel");
    } else {
        lua_pushstring(L, "send");
        lua_setfield(L, -2, "type");
        lua_pushlstring(L, (const char *)event->dst_mac, sizeof(event->dst_mac));
        lua_setfield(L, -2, "peer_mac");
        lua_pushinteger(L, event->send_status);
        lua_setfield(L, -2, "status");
        lua_pushboolean(L, event->send_status == ESP_NOW_SEND_SUCCESS);
        lua_setfield(L, -2, "success");
    }
}

static bool lua_espnow_event_receive(lua_espnow_event_t **event, TickType_t wait)
{
    QueueHandle_t queue = s_event_queue;

    if (!queue || s_event_callback_ref == LUA_NOREF) {
        return false;
    }
    return xQueueReceive(queue, event, wait) == pdTRUE;
}

static bool lua_espnow_event_push_callback(lua_State *L)
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
        lua_espnow_events_clear();
        return false;
    }
    return true;
}

int lua_espnow_on_event(lua_State *L)
{
    if (lua_isnil(L, 1)) {
        if (s_event_callback_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, s_event_callback_ref);
            s_event_callback_ref = LUA_NOREF;
        }
        lua_espnow_events_clear();
        lua_pushboolean(L, 1);
        return 1;
    }

    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    if (s_event_callback_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_event_callback_ref);
    }
    s_event_callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_espnow_events_clear();
    lua_pushboolean(L, 1);
    return 1;
}

int lua_espnow_process_events(lua_State *L)
{
    int timeout_ms = lua_isnoneornil(L, 1) ? 0 : (int)luaL_checkinteger(L, 1);
    lua_espnow_event_t *event;
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

    while (processed < LUA_ESPNOW_PROCESS_EVENTS_MAX) {
        TickType_t wait = (processed == 0) ? first_wait : 0;

        if (!lua_espnow_event_receive(&event, wait)) {
            break;
        }
        if (!lua_espnow_event_push_callback(L)) {
            lua_espnow_event_free(event);
            break;
        }
        lua_espnow_event_push_lua(L, event);
        lua_espnow_event_free(event);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            const char *msg = lua_tostring(L, -1);
            ESP_LOGE(TAG, "ESP-NOW event callback error: %s", msg ? msg : "(nil)");
            lua_pop(L, 1);
        }
        processed++;
    }

    lua_pushinteger(L, processed);
    return 1;
}
