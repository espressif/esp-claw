/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "lua_module_ble.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "lauxlib.h"
#include "lua.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define LUA_MODULE_BLE_NAME "ble"

#define LUA_BLE_DEFAULT_NAME      "esp-claw"
#define LUA_BLE_ADV_NAME_MAX      28
#define LUA_BLE_ADV_UUID16_MAX    4
#define LUA_BLE_ADV_UUID32_MAX    4
#define LUA_BLE_ADV_UUID128_MAX   2
#define LUA_BLE_ADV_DATA_MAX      BLE_HS_ADV_MAX_SZ
#define LUA_BLE_ADV_MFG_DATA_MAX  BLE_HS_ADV_MAX_SZ
#define LUA_BLE_GATT_VALUE_MAX    64
#define LUA_BLE_HOST_SYNC_TIMEOUT_MS 15000
#define LUA_BLE_NOTIFY_SUBSCRIPTION_MAX 8
#define LUA_BLE_MAX_CONNECTIONS 3
#define LUA_BLE_EVENT_QUEUE_LEN 32
#define LUA_BLE_PROCESS_EVENTS_MAX 8
#define LUA_BLE_DEFAULT_MTU 64
#define LUA_BLE_MAX_MTU 517
#define LUA_BLE_DEINIT_DISCONNECT_TIMEOUT_MS 3000
#define LUA_BLE_HOST_EXIT_TIMEOUT_MS 5000
#define LUA_BLE_NOTIFY_SEND_RETRY_MAX 1
#define LUA_BLE_ERR_REBOOT_REQUIRED ((esp_err_t)0x6c01)
#define LUA_BLE_UUID_STR_MAX 37
#define LUA_BLE_GATTS_ID_MAX 32
#define LUA_BLE_GATTS_MAX_SERVICES 4
#define LUA_BLE_GATTS_MAX_CHARS 16
#define LUA_BLE_GATTS_MAX_DESCRIPTORS 4
#define LUA_BLE_GATTS_VALUE_MAX 512

/* One slot in the stable connection table exposed to Lua as conn_index. */
typedef struct {
    bool connected;
    uint8_t conn_index;       /* Stable Lua-facing slot; 0..LUA_BLE_MAX_CONNECTIONS-1 */
    uint16_t conn_handle;     /* NimBLE handle used for GAP/GATT operations; changes across reconnects */
    uint8_t peer_addr_type;  /* BLE_ADDR_PUBLIC or BLE_ADDR_RANDOM (see lua_ble_addr_type_name) */
    uint8_t peer_addr[6];    /* Peer identity address at connect time */
    bool encrypted;           /* Link-layer encryption active */
    bool authenticated;       /* MITM-authenticated pairing completed */
    bool bonded;              /* Peer is bonded according to NimBLE security state */
    uint8_t key_size;         /* Negotiated encryption key size in bytes */
    uint16_t mtu;             /* Current ATT MTU for this connection */
} lua_module_ble_connection_t;

/* Per-connection CCCD subscription state tracked for notify/indicate gating. */
typedef struct {
    uint16_t attr_handle;     /* Characteristic value handle being subscribed to */
    uint8_t conn_index;
    bool notify_enabled;      /* Peer enabled notifications on this handle */
    bool indicate_enabled;    /* Peer enabled indications on this handle */
} lua_ble_notify_subscription_t;

/* Snapshot of the last successful ble.adv_start() configuration for restart. */
typedef struct {
    bool configured;          /* True after adv_start parsed and stored this struct */
    char name[LUA_BLE_ADV_NAME_MAX + 1];
    struct ble_gap_adv_params params; /* NimBLE interval, conn/disc mode, channel map */
    uint8_t own_addr_type;    /* BLE_OWN_ADDR_PUBLIC or BLE_OWN_ADDR_RANDOM */
    bool directed;            /* Directed advertising to directed_addr */
    ble_addr_t directed_addr;
    uint8_t adv_data[LUA_BLE_ADV_DATA_MAX];
    uint8_t adv_data_len;
    uint8_t scan_rsp_data[LUA_BLE_ADV_DATA_MAX];
    uint8_t scan_rsp_len;
} lua_ble_adv_config_t;

/* Policy when NimBLE reports repeat pairing for an already-bonded peer. */
typedef enum {
    LUA_BLE_SMP_REPEAT_PAIRING_IGNORE,       /* Reject the new pairing attempt */
    LUA_BLE_SMP_REPEAT_PAIRING_DELETE_RETRY, /* Delete old bond and retry pairing */
} lua_ble_smp_repeat_pairing_t;

/* SMP options set via ble.smp_config() before ble.init(); applied to ble_hs_cfg. */
typedef struct {
    bool bonding;
    bool secure_connections;  /* LE Secure Connections (SC) */
    bool mitm;                /* Require MITM protection during pairing */
    uint8_t io_cap;           /* BLE_SM_IO_CAP_*; must be non-NO_IO when mitm=true */
    uint8_t key_dist;         /* BLE_SM_PAIR_KEY_DIST_* bitmask for local/remote keys */
    lua_ble_smp_repeat_pairing_t repeat_pairing;
    bool require_bond_persist; /* Fail init if CONFIG_BT_NIMBLE_NVS_PERSIST is disabled */
} lua_ble_smp_config_t;

/* Module-wide runtime state shared by the Lua task and NimBLE callbacks. */
typedef struct {
    SemaphoreHandle_t mutex;           /* Recursive mutex for selected shared runtime state */
    SemaphoreHandle_t host_sync_sem;   /* Signaled by lua_ble_on_sync after host ready */
    SemaphoreHandle_t host_exit_sem;   /* Signaled when nimble_port_run() returns */
    SemaphoreHandle_t disconnect_sem;  /* Signaled on each GAP disconnect during deinit wait */
    QueueHandle_t event_queue;         /* Fixed-length queue of lua_ble_event_t pointers */
    int event_callback_ref;            /* LUA_REGISTRYINDEX ref to Lua on_event callback; LUA_NOREF if unset */
    uint32_t event_dropped;            /* Count of events dropped because queue was full */
    bool stack_inited;                 /* ble.init() completed successfully */
    bool host_synced;                  /* NimBLE sync callback ran; identity addr available */
    bool controller_inited_by_ble;       /* This module called esp_bt_controller_init */
    bool controller_enabled_by_ble;    /* This module called esp_bt_controller_enable */
    bool nimble_inited;                /* esp_nimble_init succeeded */
    bool host_task_started;            /* nimble_port_freertos_init started host task */
    bool advertising;                  /* Cached advertising-active state updated by start/stop/GAP callbacks */
    bool advertising_requested;        /* User wants adv running (survives temporary stop) */
    bool advertising_paused_for_connection_full; /* Adv paused because conn slots are full */
    bool draining;                     /* deinit in progress; used to mark teardown-related disconnect events */
    bool stack_unrecoverable;          /* Host stop timed out; reboot required before using BLE again */
    uint8_t own_addr_type;
    uint8_t own_addr[6];               /* Local identity address after host sync */
    uint16_t preferred_mtu;            /* ble_att_set_preferred_mtu target */
    lua_module_ble_connection_t conns[LUA_BLE_MAX_CONNECTIONS];
    lua_ble_notify_subscription_t notify_subscriptions[LUA_BLE_NOTIFY_SUBSCRIPTION_MAX];
    char adv_name[LUA_BLE_ADV_NAME_MAX + 1]; /* Last GAP device name / default adv name */
    lua_ble_adv_config_t adv_config;
} lua_ble_runtime_t;

/* Event types delivered to Lua via ble.on_event / ble.process_events. */
typedef enum {
    LUA_BLE_EVENT_ADV_STARTED,
    LUA_BLE_EVENT_ADV_STOPPED,
    LUA_BLE_EVENT_CONNECTED,
    LUA_BLE_EVENT_DISCONNECTED,
    LUA_BLE_EVENT_MTU_CHANGED,
    LUA_BLE_EVENT_SECURITY_CHANGED,
    LUA_BLE_EVENT_SUBSCRIBE_CHANGED,
    LUA_BLE_EVENT_GATTS_READ,
    LUA_BLE_EVENT_GATTS_WRITE,
    LUA_BLE_EVENT_NOTIFY_COMPLETE,
    LUA_BLE_EVENT_INDICATE_COMPLETE,
} lua_ble_event_type_t;

/* Queued event payload; only fields relevant to type are populated. */
typedef struct {
    lua_ble_event_type_t type;
    uint8_t conn_index;
    uint16_t conn_handle;
    uint16_t mtu;                       /* LUA_BLE_EVENT_MTU_CHANGED */
    uint8_t peer_addr_type;             /* LUA_BLE_EVENT_CONNECTED */
    uint8_t peer_addr[6];
    bool encrypted;                     /* LUA_BLE_EVENT_SECURITY_CHANGED */
    bool authenticated;
    bool bonded;
    uint8_t key_size;
    bool notify;                        /* LUA_BLE_EVENT_SUBSCRIBE_CHANGED */
    bool indicate;
    int error_code;                     /* HCI or host error when applicable */
    char reason[24];                    /* Human-readable stop/disconnect/security reason */
    char status[8];                     /* "ok" or "error" for notify/indicate complete */
    char service_id[LUA_BLE_GATTS_ID_MAX + 1];       /* Lua profile id if defined */
    char characteristic_id[LUA_BLE_GATTS_ID_MAX + 1];
    char uuid_service[LUA_BLE_UUID_STR_MAX];
    char uuid_characteristic[LUA_BLE_UUID_STR_MAX];
    char uuid_descriptor[LUA_BLE_UUID_STR_MAX];      /* Set for descriptor read/write events */
    uint8_t *data;                       /* Optional GATT write payload; valid when data_len > 0 */
    uint16_t data_len;
    uint16_t offset;                    /* ATT read/write offset */
} lua_ble_event_t;

extern const char *TAG;

extern lua_ble_runtime_t s_ble_rt;

#define s_host_sync_sem (s_ble_rt.host_sync_sem)
#define s_host_exit_sem (s_ble_rt.host_exit_sem)
#define s_disconnect_sem (s_ble_rt.disconnect_sem)
#define s_event_queue (s_ble_rt.event_queue)
#define s_event_callback_ref (s_ble_rt.event_callback_ref)
#define s_event_dropped (s_ble_rt.event_dropped)
#define s_stack_inited (s_ble_rt.stack_inited)
#define s_host_synced (s_ble_rt.host_synced)
#define s_controller_inited_by_ble (s_ble_rt.controller_inited_by_ble)
#define s_controller_enabled_by_ble (s_ble_rt.controller_enabled_by_ble)
#define s_nimble_inited (s_ble_rt.nimble_inited)
#define s_host_task_started (s_ble_rt.host_task_started)
#define s_advertising (s_ble_rt.advertising)
#define s_advertising_requested (s_ble_rt.advertising_requested)
#define s_advertising_paused_for_connection_full (s_ble_rt.advertising_paused_for_connection_full)
#define s_draining (s_ble_rt.draining)
#define s_stack_unrecoverable (s_ble_rt.stack_unrecoverable)
#define s_own_addr_type (s_ble_rt.own_addr_type)
#define s_own_addr (s_ble_rt.own_addr)
#define s_preferred_mtu (s_ble_rt.preferred_mtu)
#define s_conns (s_ble_rt.conns)
#define s_notify_subscriptions (s_ble_rt.notify_subscriptions)
#define s_adv_name (s_ble_rt.adv_name)
#define s_adv_config (s_ble_rt.adv_config)

/** @brief Create the runtime recursive mutex if not already allocated. */
esp_err_t lua_ble_runtime_ensure(void);

/** @brief Take s_ble_rt.mutex; no-op if ensure failed. */
void lua_ble_runtime_lock(void);

/** @brief Release s_ble_rt.mutex. */
void lua_ble_runtime_unlock(void);

/**
 * @brief Push Lua return value for an esp_err_t: true on ESP_OK, else nil + err string.
 * @return Number of Lua stack values pushed (1 or 2).
 */
int lua_ble_push_ok_or_err(lua_State *L, esp_err_t err);

/**
 * @brief Push nil and a stable BLE error string for Lua callers.
 * @return 2 (nil, err).
 */
int lua_ble_push_err_string(lua_State *L, const char *err);

/** @brief True if table at index has a non-nil field. */
bool lua_ble_table_has_field(lua_State *L, int index, const char *field);

/** @brief Map NimBLE address type to "public" or "random" for Lua events. */
const char *lua_ble_addr_type_name(uint8_t addr_type);

/** @brief Effective connection slot count (min of Kconfig and LUA_BLE_MAX_CONNECTIONS). */
uint8_t lua_ble_slot_count(void);

/** @brief True if at least one connection slot is connected. */
bool lua_ble_has_connection(void);

/** @brief Count connected slots. */
int lua_ble_connection_count(void);

/** @brief Find conn_index for a NimBLE conn_handle, or -1. */
int lua_ble_find_conn_index_by_handle(uint16_t conn_handle);

/** @brief Reset all connection slots to disconnected defaults. */
void lua_ble_connection_init_slots(void);

/** @brief Clear all connections and notify subscription records. */
void lua_ble_connection_clear(void);

/** @brief Clear one connection slot and its notify subscriptions. */
void lua_ble_connection_clear_index(uint8_t conn_index);

/** @brief Refresh encrypted/authenticated/bonded/key_size from ble_gap_conn_find. */
void lua_ble_connection_refresh_security(uint16_t conn_handle);

/** @brief Refresh security state and enqueue LUA_BLE_EVENT_SECURITY_CHANGED. */
void lua_ble_connection_update_security(uint16_t conn_handle, const char *reason);

/**
 * @brief Allocate a connection slot for conn_handle and enqueue LUA_BLE_EVENT_CONNECTED.
 * @return conn_index on success, -1 if no free slot.
 */
int lua_ble_connection_set_connected(uint16_t conn_handle);

/** @brief Update or remove a notify/indicate subscription record for attr_handle. */
void lua_ble_notify_subscription_update(uint8_t conn_index, uint16_t attr_handle, bool notify_enabled,
                                        bool indicate_enabled);

/** @brief True if peer has notifications enabled on attr_handle for conn_index. */
bool lua_module_ble_is_notify_subscribed(uint8_t conn_index, uint16_t attr_handle);

/** @brief True if peer has indications enabled on attr_handle for conn_index. */
bool lua_module_ble_is_indicate_subscribed(uint8_t conn_index, uint16_t attr_handle);

/** @brief Drain and discard all queued events (under runtime lock). */
void lua_ble_events_clear(void);

/** @brief Invoke the registered Lua callback for all queued events (non-blocking receive). */
void lua_ble_events_drain(lua_State *L);

/** @brief Allocate an event struct from SPIRAM-preferred heap. */
lua_ble_event_t *lua_ble_event_alloc(void);

/** @brief Free an event struct and its optional payload from lua_ble_event_alloc. */
void lua_ble_event_free(lua_ble_event_t *event);

/** @brief Copy payload into event->data. */
esp_err_t lua_ble_event_set_data(lua_ble_event_t *event, const uint8_t *data, uint16_t data_len);

/** @brief Enqueue event and transfer ownership; drop/free if queue full. */
void lua_ble_event_enqueue(lua_ble_event_t *event);

/** @brief Enqueue event and transfer ownership, dropping/freeing oldest queued event if necessary. */
void lua_ble_event_enqueue_force(lua_ble_event_t *event);

/** @brief Allocate, fill, and enqueue a minimal adv-related event. */
void lua_ble_event_simple_adv(lua_ble_event_type_t type, const char *reason, int error_code);

/**
 * @brief Lua API: ble.on_event(fn_or_nil).
 * Register or clear the single event callback and clear the queue.
 */
int lua_ble_on_event(lua_State *L);

/**
 * @brief Lua API: ble.process_events(timeout_ms).
 * Dispatch up to LUA_BLE_PROCESS_EVENTS_MAX events to the registered callback.
 */
int lua_ble_process_events(lua_State *L);

/** @brief Lua API: ble.init() - start controller and NimBLE host. */
int lua_ble_init(lua_State *L);

/** @brief Lua API: ble.deinit() - drain events, disconnect, stop host. */
int lua_ble_deinit(lua_State *L);

/** @brief Lua API: ble.set_name(name). */
int lua_ble_set_name(lua_State *L);

/** @brief Lua API: ble.set_max_mtu(mtu). */
int lua_ble_set_max_mtu(lua_State *L);

/** @brief Lua API: ble.adv_start(opts). */
int lua_ble_adv_start(lua_State *L);

/** @brief Lua API: ble.adv_stop(). */
int lua_ble_adv_stop(lua_State *L);

/** @brief Lua API: ble.disconnect(opts). */
int lua_ble_disconnect(lua_State *L);

/** @brief NimBLE host reset callback. */
void lua_ble_on_reset(int reason);

/** @brief NimBLE host sync callback; resolves identity address and signals host_sync_sem. */
void lua_ble_on_sync(void);

/** @brief Lua API: ble.smp_config(opts) - must be called before ble.init(). */
int lua_ble_smp_config(lua_State *L);

/** @brief Return init-blocking SMP config error string, or NULL if init may proceed. */
const char *lua_ble_smp_init_error(void);

/** @brief Apply s_smp_config to ble_hs_cfg and ble_store_config_init. */
void lua_ble_smp_apply_default_config(void);

/** @brief True when MITM and a non-NO_IO capability allow authenticated GATT access. */
bool lua_ble_smp_allows_authenticated_access(void);

/** @brief GAP repeat-pairing handler; may delete bond and return RETRY. */
int lua_ble_smp_handle_repeat_pairing(const struct ble_gap_repeat_pairing *repeat_pairing);

/** @brief Default passkey/numcmp injection for Phase 1 (no Lua UI). */
void lua_ble_smp_handle_passkey_action(uint16_t conn_handle, const struct ble_gap_passkey_params *params);

/** @brief Reset dynamic GATT profile runtime state. */
void lua_ble_gatts_reset(void);

/** @brief Clear per-connection GATT runtime state such as pending indications. */
void lua_ble_gatts_clear_conn_state(uint8_t conn_index);

/** @brief Lua API: ble.gatts_define(profile). */
int lua_ble_gatts_define(lua_State *L);

/** @brief Lua API: ble.gatts_set_value(opts). */
int lua_ble_gatts_set_value(lua_State *L);

/** @brief Lua API: ble.notify(opts). */
int lua_ble_notify(lua_State *L);

/** @brief Lua API: ble.indicate(opts). */
int lua_ble_indicate(lua_State *L);

/** @brief Lua API: ble.stats({ char = ... }) - push characteristic cached value. */
int lua_ble_gatts_stats_char(lua_State *L, int opts_index);

/** @brief Fill GATT service/characteristic id and UUID fields in event from attr_handle. */
bool lua_ble_gatts_fill_event_by_handle(lua_ble_event_t *event, uint16_t attr_handle);

/** @brief GAP notify_tx handler; enqueue final notify/indicate completion events. */
void lua_ble_gatts_on_notify_tx(uint16_t conn_handle, uint16_t attr_handle, bool indication, int status);
