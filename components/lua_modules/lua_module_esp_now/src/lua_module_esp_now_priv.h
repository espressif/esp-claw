/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lua_module_esp_now.h"
#include "lua_module_esp_now_compat.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "lauxlib.h"
#include "lua.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LUA_MODULE_ESP_NOW_NAME "esp_now"

#define LUA_ESPNOW_EVENT_QUEUE_LEN 32
#define LUA_ESPNOW_PROCESS_EVENTS_MAX 8

/* Event kinds delivered to Lua via esp_now.on_event / esp_now.process_events. */
typedef enum {
    LUA_ESPNOW_EVENT_RECV,   /* A unicast/broadcast frame was received. */
    LUA_ESPNOW_EVENT_SEND,   /* A previously issued esp_now.send completed. */
} lua_espnow_event_type_t;

/* Queued event payload; only fields relevant to type are populated. */
typedef struct {
    lua_espnow_event_type_t type;
    uint8_t src_mac[ESP_NOW_ETH_ALEN];  /* RECV: sender MAC */
    uint8_t dst_mac[ESP_NOW_ETH_ALEN];  /* RECV: dest MAC (unicast or broadcast); SEND: peer MAC */
    int rssi;                           /* RECV: signal strength from rx_ctrl */
    uint8_t channel;                    /* RECV: primary channel from rx_ctrl */
    int send_status;                    /* SEND: ESP_NOW_SEND_SUCCESS(0) / ESP_NOW_SEND_FAIL(1) */
    uint8_t *data;                      /* RECV: heap-allocated payload; NULL if empty */
    size_t data_len;
} lua_espnow_event_t;

/* Module-wide runtime state shared by the Lua task and ESP-NOW/WiFi callbacks. */
typedef struct {
    SemaphoreHandle_t mutex;    /* Recursive mutex guarding queue/callback/state */
    QueueHandle_t event_queue;  /* Queue of lua_espnow_event_t pointers */
    int event_callback_ref;     /* LUA_REGISTRYINDEX ref to on_event callback; LUA_NOREF if unset */
    uint32_t event_dropped;     /* Count of events dropped because the queue was full */
    uint32_t recv_count;        /* Total received frames delivered to the queue */
    uint32_t send_count;        /* Total send completions delivered to the queue */
    bool inited;                /* esp_now.init() completed and callbacks registered */
} lua_espnow_runtime_t;

extern const char *TAG;
extern lua_espnow_runtime_t s_espnow_rt;

#define s_event_queue (s_espnow_rt.event_queue)
#define s_event_callback_ref (s_espnow_rt.event_callback_ref)
#define s_event_dropped (s_espnow_rt.event_dropped)

/** @brief Create the runtime recursive mutex if not already allocated. */
esp_err_t lua_espnow_runtime_ensure(void);

/** @brief Take the runtime mutex; no-op if ensure failed. */
void lua_espnow_runtime_lock(void);

/** @brief Release the runtime mutex. */
void lua_espnow_runtime_unlock(void);

/**
 * @brief Push a Lua return for an esp_err_t: true on ESP_OK, else nil + err string.
 * @return Number of Lua values pushed (1 or 2).
 */
int lua_espnow_push_ok_or_err(lua_State *L, esp_err_t err);

/** @brief Allocate a zeroed event struct from SPIRAM-preferred heap. */
lua_espnow_event_t *lua_espnow_event_alloc(void);

/** @brief Free an event struct and its optional payload. */
void lua_espnow_event_free(lua_espnow_event_t *event);

/** @brief Copy payload into event->data. */
esp_err_t lua_espnow_event_set_data(lua_espnow_event_t *event, const uint8_t *data, size_t data_len);

/** @brief Enqueue event and transfer ownership; drop/free if the queue is full. */
void lua_espnow_event_enqueue(lua_espnow_event_t *event);

/** @brief Drain and discard all queued events (under the runtime lock). */
void lua_espnow_events_clear(void);

/** @brief ESP-NOW receive callback (runs in the WiFi task). */
void lua_espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len);

/** @brief ESP-NOW send-complete callback (runs in the WiFi task). */
void lua_espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);

/**
 * @brief Lua API: esp_now.on_event(fn_or_nil).
 * Register or clear the single event callback and clear the queue.
 */
int lua_espnow_on_event(lua_State *L);

/**
 * @brief Lua API: esp_now.process_events(timeout_ms).
 * Dispatch up to LUA_ESPNOW_PROCESS_EVENTS_MAX events to the registered callback.
 */
int lua_espnow_process_events(lua_State *L);

#ifdef __cplusplus
}
#endif
