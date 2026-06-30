/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "lua_module_ble_hid.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cap_lua.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "lauxlib.h"
#include "lua.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"

#define LUA_MODULE_BLE_HID_NAME "ble_hid"
#define LUA_HID_DEFAULT_NAME "esp-claw-hid"
#define LUA_HID_NAME_MAX 29
#define LUA_HID_MAP_INDEX 0
#define REPORT_ID_CONSUMER 1
#define REPORT_ID_KEYBOARD 2
#define REPORT_ID_MOUSE 3
#define CONSUMER_REPORT_SIZE 1
#define KEYBOARD_REPORT_SIZE 8
#define KEYBOARD_MAX_KEYS 6
#define MOUSE_REPORT_SIZE 5
#define KEY_HOLD_MS 40
#define KEY_RELEASE_MS 20
#define TEXT_GAP_MS 50
#define CONSUMER_PRESS_MS 40
#define CONSUMER_LONG_PRESS_MS 600
#define CONSUMER_RELEASE_GAP_MS 40
#define MOUSE_CLICK_MS 40
#define BLE_HID_HOST_SYNC_TIMEOUT_MS 15000
#define BLE_HID_ADV_UUID16_MAX 4
#define MOD_CTRL (1U << 0)
#define MOD_SHIFT (1U << 1)
#define MOD_ALT (1U << 2)
#define MOD_GUI (1U << 3)
#define MOD_RIGHT_CTRL (1U << 4)
#define MOD_RIGHT_SHIFT (1U << 5)
#define MOD_RIGHT_ALT (1U << 6)
#define MOD_RIGHT_GUI (1U << 7)
#define MOUSE_LEFT (1U << 0)
#define MOUSE_RIGHT (1U << 1)
#define MOUSE_MIDDLE (1U << 2)
#define MOUSE_MASK (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE)
#define HID_USAGE_PAGE(v) 0x05, (v)
#define HID_USAGE(v) 0x09, (v)
#define HID_USAGE16(v) 0x0a, ((v) & 0xff), (((v) >> 8) & 0xff)
#define HID_USAGE_MIN(v) 0x19, (v)
#define HID_USAGE_MAX(v) 0x29, (v)
#define HID_COLLECTION(v) 0xa1, (v)
#define HID_END_COLLECTION 0xc0
#define HID_REPORT_ID(v) 0x85, (v)
#define HID_LOGICAL_MIN(v) 0x15, (v)
#define HID_LOGICAL_MAX(v) 0x25, (v)
#define HID_REPORT_SIZE(v) 0x75, (v)
#define HID_REPORT_COUNT(v) 0x95, (v)
#define HID_INPUT(v) 0x81, (v)

static const char *TAG = "lua_ble_hid";

typedef struct {
    bool initialized;
    bool advertising;
    bool connected;
    bool bonded;
} hid_status_t;

typedef struct {
    bool connected;
    uint16_t conn_handle;
    bool encrypted;
    bool bonded;
} ble_hid_connection_t;

typedef int (*ble_hid_gap_event_handler_t)(struct ble_gap_event *event);

typedef struct {
    int type;
    ble_hid_gap_event_handler_t handler;
} ble_hid_gap_event_handler_entry_t;

typedef struct {
    const char *name;
    uint8_t value;
} name_u8_t;

typedef struct {
    char ch;
    uint8_t modifier;
    uint8_t keycode;
} ascii_key_t;

typedef struct {
    const char *name;
    lua_CFunction fn;
} lua_hid_fn_t;

static SemaphoreHandle_t s_host_sync_sem;
static bool s_stack_inited;
static bool s_host_synced;
static bool s_controller_inited_by_ble_hid;
static bool s_controller_enabled_by_ble_hid;
static bool s_nimble_inited;
static bool s_host_task_started;
static bool s_advertising_requested;
static uint8_t s_own_addr_type;
static char s_adv_name[LUA_HID_NAME_MAX + 1] = LUA_HID_DEFAULT_NAME;
static uint16_t s_adv_appearance;
static uint16_t s_adv_uuid16_list[BLE_HID_ADV_UUID16_MAX];
static size_t s_adv_uuid16_count;
static ble_hid_connection_t s_conn = {
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};
static ble_hs_reset_fn *s_external_reset_cb;
static ble_hs_sync_fn *s_external_sync_cb;
static ble_gatt_register_fn *s_external_gatts_register_cb;
static void *s_external_gatts_register_arg;
static esp_hidd_dev_t *s_hid_dev;
static hid_status_t s_status;
static uint8_t s_mouse_buttons;

static const uint8_t s_report_map[] = {
    HID_USAGE_PAGE(0x0c), HID_USAGE(0x01), HID_COLLECTION(0x01),
    HID_REPORT_ID(REPORT_ID_CONSUMER), HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0x01),
    HID_USAGE(0xe9), HID_USAGE(0xea), HID_USAGE(0xcd), HID_USAGE(0xb5), HID_USAGE(0xb6), HID_USAGE(0xe2),
    HID_REPORT_SIZE(0x01), HID_REPORT_COUNT(0x06), HID_INPUT(0x02),
    HID_REPORT_SIZE(0x02), HID_REPORT_COUNT(0x01), HID_INPUT(0x03), HID_END_COLLECTION,

    HID_USAGE_PAGE(0x01), HID_USAGE(0x06), HID_COLLECTION(0x01), HID_REPORT_ID(REPORT_ID_KEYBOARD),
    HID_USAGE_PAGE(0x07), HID_USAGE_MIN(0xe0), HID_USAGE_MAX(0xe7),
    HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0x01),
    HID_REPORT_SIZE(0x01), HID_REPORT_COUNT(0x08), HID_INPUT(0x02),
    HID_REPORT_COUNT(0x01), HID_REPORT_SIZE(0x08), HID_INPUT(0x03),
    HID_REPORT_COUNT(KEYBOARD_MAX_KEYS), HID_REPORT_SIZE(0x08),
    HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0x65),
    HID_USAGE_PAGE(0x07), HID_USAGE_MIN(0x00), HID_USAGE_MAX(0x65), HID_INPUT(0x00),
    HID_END_COLLECTION,

    HID_USAGE_PAGE(0x01), HID_USAGE(0x02), HID_COLLECTION(0x01), HID_REPORT_ID(REPORT_ID_MOUSE),
    HID_USAGE(0x01), HID_COLLECTION(0x00),
    HID_USAGE_PAGE(0x09), HID_USAGE_MIN(0x01), HID_USAGE_MAX(0x03),
    HID_LOGICAL_MIN(0x00), HID_LOGICAL_MAX(0x01),
    HID_REPORT_SIZE(0x01), HID_REPORT_COUNT(0x03), HID_INPUT(0x02),
    HID_REPORT_SIZE(0x05), HID_REPORT_COUNT(0x01), HID_INPUT(0x03),
    HID_USAGE_PAGE(0x01), HID_USAGE(0x30), HID_USAGE(0x31), HID_USAGE(0x38),
    HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f),
    HID_REPORT_SIZE(0x08), HID_REPORT_COUNT(0x03), HID_INPUT(0x06),
    HID_USAGE_PAGE(0x0c), HID_USAGE16(0x0238), HID_REPORT_COUNT(0x01), HID_INPUT(0x06),
    HID_END_COLLECTION, HID_END_COLLECTION,
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    { .data = s_report_map, .len = sizeof(s_report_map) },
};

static void ble_hid_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_hid_connection_clear(void)
{
    s_conn.connected = false;
    s_conn.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_conn.encrypted = false;
    s_conn.bonded = false;
    s_status.connected = false;
    s_status.bonded = false;
}

static void ble_hid_connection_update_security(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(conn_handle, &desc);

    if (rc != 0) {
        ESP_LOGW(TAG, "connection security lookup failed conn_handle=%u rc=%d", conn_handle, rc);
        return;
    }

    s_conn.encrypted = desc.sec_state.encrypted;
    s_conn.bonded = desc.sec_state.bonded;
    s_status.bonded = s_conn.bonded;
    ESP_LOGI(TAG, "connection security conn_handle=%u encrypted=%d bonded=%d authenticated=%d key_size=%u",
             conn_handle,
             s_conn.encrypted,
             s_conn.bonded,
             desc.sec_state.authenticated,
             desc.sec_state.key_size);
}

static void ble_hid_connection_set_connected(uint16_t conn_handle)
{
    s_conn.connected = true;
    s_conn.conn_handle = conn_handle;
    s_status.connected = true;
    ble_hid_connection_update_security(conn_handle);
}

static void ble_hid_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset reason=%d", reason);
    if (s_external_reset_cb) {
        s_external_reset_cb(reason);
    }
}

static void ble_hid_on_sync(void)
{
    int rc;

    if (s_external_sync_cb) {
        s_external_sync_cb();
    }

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed rc=%d", rc);
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed rc=%d", rc);
    }

    s_host_synced = true;
    if (s_host_sync_sem) {
        (void)xSemaphoreGive(s_host_sync_sem);
    }
}

static void ble_hid_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    if (s_external_gatts_register_cb) {
        s_external_gatts_register_cb(ctxt, s_external_gatts_register_arg);
    }
    (void)arg;
}

static int ble_hid_handle_gap_connect(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap connect conn_handle=%u status=%d",
             event->connect.conn_handle,
             event->connect.status);
    if (event->connect.status == 0) {
        s_status.advertising = false;
        ble_hid_connection_set_connected(event->connect.conn_handle);
    } else {
        ble_hid_connection_clear();
    }
    return 0;
}

static esp_err_t ble_hid_adv_apply(const char *name, uint16_t appearance,
                                   const uint16_t *uuid16_list, size_t uuid16_count);

static int ble_hid_handle_gap_disconnect(struct ble_gap_event *event)
{
    esp_err_t err;

    ESP_LOGI(TAG, "gap disconnect conn_handle=%u reason=%d",
             event->disconnect.conn.conn_handle,
             event->disconnect.reason);
    ble_hid_connection_clear();
    s_mouse_buttons = 0;
    if (s_advertising_requested) {
        err = ble_hid_adv_apply(s_adv_name, s_adv_appearance, s_adv_uuid16_list, s_adv_uuid16_count);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "restart advertising after disconnect failed: %s", esp_err_to_name(err));
        }
    }
    return 0;
}

static int ble_hid_handle_gap_enc_change(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap enc_change conn_handle=%u status=%d",
             event->enc_change.conn_handle,
             event->enc_change.status);
    if (event->enc_change.status == 0) {
        ble_hid_connection_update_security(event->enc_change.conn_handle);
    }
    return 0;
}

static int ble_hid_handle_gap_identity_resolved(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap identity_resolved conn_handle=%u", event->identity_resolved.conn_handle);
    ble_hid_connection_update_security(event->identity_resolved.conn_handle);
    return 0;
}

static int ble_hid_handle_gap_repeat_pairing(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap repeat_pairing conn_handle=%u key_size=%u authenticated=%u sc=%u",
             event->repeat_pairing.conn_handle,
             event->repeat_pairing.cur_key_size,
             event->repeat_pairing.cur_authenticated,
             event->repeat_pairing.cur_sc);
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
}

static int ble_hid_handle_gap_mtu(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap mtu conn_handle=%u channel_id=%u mtu=%u",
             event->mtu.conn_handle,
             event->mtu.channel_id,
             event->mtu.value);
    return 0;
}

static int ble_hid_handle_gap_adv_complete(struct ble_gap_event *event)
{
    ESP_LOGI(TAG, "gap adv_complete reason=%d", event->adv_complete.reason);
    s_status.advertising = false;
    return 0;
}

static const ble_hid_gap_event_handler_entry_t s_gap_event_handlers[] = {
    { BLE_GAP_EVENT_CONNECT, ble_hid_handle_gap_connect },
    { BLE_GAP_EVENT_DISCONNECT, ble_hid_handle_gap_disconnect },
    { BLE_GAP_EVENT_ENC_CHANGE, ble_hid_handle_gap_enc_change },
    { BLE_GAP_EVENT_IDENTITY_RESOLVED, ble_hid_handle_gap_identity_resolved },
    { BLE_GAP_EVENT_REPEAT_PAIRING, ble_hid_handle_gap_repeat_pairing },
    { BLE_GAP_EVENT_MTU, ble_hid_handle_gap_mtu },
    { BLE_GAP_EVENT_ADV_COMPLETE, ble_hid_handle_gap_adv_complete },
};

static int ble_hid_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    for (size_t i = 0; i < sizeof(s_gap_event_handlers) / sizeof(s_gap_event_handlers[0]); i++) {
        if (s_gap_event_handlers[i].type == event->type) {
            return s_gap_event_handlers[i].handler(event);
        }
    }
    return 0;
}

static esp_err_t ble_hid_controller_start(void)
{
    esp_err_t err;
    esp_bt_controller_status_t status = esp_bt_controller_get_status();

    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_controller_inited_by_ble_hid = true;
        status = esp_bt_controller_get_status();
    }

    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        return ESP_OK;
    }
    if (status != ESP_BT_CONTROLLER_STATUS_INITED) {
        ESP_LOGE(TAG, "unsupported BLE controller status: %d", status);
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return err;
    }
    s_controller_enabled_by_ble_hid = true;
    return ESP_OK;
}

static void ble_hid_controller_stop(void)
{
    esp_bt_controller_status_t status = esp_bt_controller_get_status();

    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED && s_controller_enabled_by_ble_hid) {
        if (esp_bt_controller_disable() == ESP_OK) {
            s_controller_enabled_by_ble_hid = false;
            status = esp_bt_controller_get_status();
        }
    }

    if (status == ESP_BT_CONTROLLER_STATUS_INITED && s_controller_inited_by_ble_hid) {
        if (esp_bt_controller_deinit() == ESP_OK) {
            s_controller_inited_by_ble_hid = false;
        }
    }
}

static esp_err_t ble_hid_stack_prepare(void)
{
    esp_err_t err;

    if (s_nimble_inited) {
        return ESP_OK;
    }

    if (s_host_sync_sem == NULL) {
        s_host_sync_sem = xSemaphoreCreateBinary();
        if (s_host_sync_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    err = ble_hid_controller_start();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_nimble_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_nimble_init failed: %s", esp_err_to_name(err));
        ble_hid_controller_stop();
        return err;
    }
    s_nimble_inited = true;
    return ESP_OK;
}

static esp_err_t ble_hid_stack_start(void)
{
    esp_err_t err;

    if (s_stack_inited) {
        return ESP_OK;
    }

    err = ble_hid_stack_prepare();
    if (err != ESP_OK) {
        return err;
    }

    s_external_reset_cb = ble_hs_cfg.reset_cb;
    s_external_sync_cb = ble_hs_cfg.sync_cb;
    s_external_gatts_register_cb = ble_hs_cfg.gatts_register_cb;
    s_external_gatts_register_arg = ble_hs_cfg.gatts_register_arg;
    ble_hs_cfg.reset_cb = ble_hid_on_reset;
    ble_hs_cfg.sync_cb = ble_hid_on_sync;
    ble_hs_cfg.gatts_register_cb = ble_hid_gatts_register_cb;
    ble_hs_cfg.gatts_register_arg = NULL;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ID;

    if (ble_svc_gap_device_name_set(LUA_HID_DEFAULT_NAME) != 0) {
        return ESP_FAIL;
    }

    s_host_synced = false;
    nimble_port_freertos_init(ble_hid_host_task);
    s_host_task_started = true;

    if (xSemaphoreTake(s_host_sync_sem, pdMS_TO_TICKS(BLE_HID_HOST_SYNC_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "NimBLE host sync timeout");
        return ESP_ERR_TIMEOUT;
    }

    s_stack_inited = true;
    return ESP_OK;
}

static esp_err_t ble_hid_stack_stop(void)
{
    esp_err_t err;

    if (s_status.advertising) {
        (void)ble_gap_adv_stop();
        s_status.advertising = false;
    }

    if (s_host_task_started) {
        err = nimble_port_stop();
        if (err == ESP_OK) {
            s_host_task_started = false;
        }
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    if (s_nimble_inited) {
        err = esp_nimble_deinit();
        if (err == ESP_OK) {
            s_nimble_inited = false;
        }
    }

    ble_hid_controller_stop();
    s_stack_inited = false;
    s_host_synced = false;
    s_advertising_requested = false;
    ble_hid_connection_clear();
    return ESP_OK;
}

static esp_err_t ble_hid_adv_apply(const char *name, uint16_t appearance,
                                   const uint16_t *uuid16_list, size_t uuid16_count)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    ble_uuid16_t adv_uuids[BLE_HID_ADV_UUID16_MAX];
    int rc;

    if (!s_stack_inited || !s_host_synced) {
        return ESP_ERR_INVALID_STATE;
    }
    if (uuid16_count > sizeof(adv_uuids) / sizeof(adv_uuids[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ble_gap_adv_active()) {
        (void)ble_gap_adv_stop();
    }

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    if (appearance != 0) {
        fields.appearance = appearance;
        fields.appearance_is_present = 1;
    }
    if (uuid16_count > 0) {
        for (size_t i = 0; i < uuid16_count; i++) {
            adv_uuids[i] = (ble_uuid16_t)BLE_UUID16_INIT(uuid16_list[i]);
        }
        fields.uuids16 = adv_uuids;
        fields.num_uuids16 = uuid16_count;
        fields.uuids16_is_complete = 1;
    }

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed rc=%d", rc);
        return ESP_FAIL;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_hid_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed rc=%d", rc);
        return ESP_FAIL;
    }

    s_status.advertising = true;
    return ESP_OK;
}

static esp_err_t ble_hid_adv_start(const char *name, uint16_t appearance,
                                   const uint16_t *uuid16_list, size_t uuid16_count)
{
    esp_err_t err;

    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(name) > LUA_HID_NAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_status.connected || s_status.advertising) {
        return ESP_OK;
    }
    if (ble_svc_gap_device_name_set(name) != 0) {
        return ESP_FAIL;
    }

    err = ble_hid_adv_apply(name, appearance, uuid16_list, uuid16_count);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(s_adv_name, name, sizeof(s_adv_name));
    s_adv_appearance = appearance;
    if (uuid16_count > 0) {
        memcpy(s_adv_uuid16_list, uuid16_list, uuid16_count * sizeof(s_adv_uuid16_list[0]));
    }
    s_adv_uuid16_count = uuid16_count;
    s_advertising_requested = true;
    return ESP_OK;
}

static esp_err_t ble_hid_adv_stop(void)
{
    int rc;

    if (!s_stack_inited || !s_status.advertising) {
        s_advertising_requested = false;
        s_status.advertising = false;
        return ESP_OK;
    }

    rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_stop failed rc=%d", rc);
        return ESP_FAIL;
    }

    s_advertising_requested = false;
    s_status.advertising = false;
    return ESP_OK;
}

static const name_u8_t s_consumer[] = {
    { "volume_up", 1U << 0 }, { "volume_down", 1U << 1 }, { "play_pause", 1U << 2 },
    { "next_track", 1U << 3 }, { "previous_track", 1U << 4 }, { "mute", 1U << 5 },
};

static const name_u8_t s_keys[] = {
    { "A", 0x04 }, { "B", 0x05 }, { "C", 0x06 }, { "D", 0x07 }, { "E", 0x08 }, { "F", 0x09 },
    { "G", 0x0a }, { "H", 0x0b }, { "I", 0x0c }, { "J", 0x0d }, { "K", 0x0e }, { "L", 0x0f },
    { "M", 0x10 }, { "N", 0x11 }, { "O", 0x12 }, { "P", 0x13 }, { "Q", 0x14 }, { "R", 0x15 },
    { "S", 0x16 }, { "T", 0x17 }, { "U", 0x18 }, { "V", 0x19 }, { "W", 0x1a }, { "X", 0x1b },
    { "Y", 0x1c }, { "Z", 0x1d }, { "1", 0x1e }, { "2", 0x1f }, { "3", 0x20 }, { "4", 0x21 },
    { "5", 0x22 }, { "6", 0x23 }, { "7", 0x24 }, { "8", 0x25 }, { "9", 0x26 }, { "0", 0x27 },
    { "ENTER", 0x28 }, { "ESC", 0x29 }, { "ESCAPE", 0x29 }, { "BACKSPACE", 0x2a }, { "TAB", 0x2b },
    { "SPACE", 0x2c }, { "MINUS", 0x2d }, { "-", 0x2d }, { "EQUAL", 0x2e }, { "=", 0x2e },
    { "LEFT_BRACKET", 0x2f }, { "[", 0x2f }, { "RIGHT_BRACKET", 0x30 }, { "]", 0x30 },
    { "BACKSLASH", 0x31 }, { "\\", 0x31 }, { "SEMICOLON", 0x33 }, { ";", 0x33 },
    { "QUOTE", 0x34 }, { "'", 0x34 }, { "GRAVE", 0x35 }, { "`", 0x35 }, { "COMMA", 0x36 },
    { ",", 0x36 }, { "PERIOD", 0x37 }, { ".", 0x37 }, { "SLASH", 0x38 }, { "/", 0x38 },
    { "CAPS_LOCK", 0x39 }, { "F1", 0x3a }, { "F2", 0x3b }, { "F3", 0x3c }, { "F4", 0x3d },
    { "F5", 0x3e }, { "F6", 0x3f }, { "F7", 0x40 }, { "F8", 0x41 }, { "F9", 0x42 },
    { "F10", 0x43 }, { "F11", 0x44 }, { "F12", 0x45 }, { "PRINT_SCREEN", 0x46 },
    { "SCROLL_LOCK", 0x47 }, { "PAUSE", 0x48 }, { "INSERT", 0x49 }, { "HOME", 0x4a },
    { "PAGE_UP", 0x4b }, { "DELETE", 0x4c }, { "END", 0x4d }, { "PAGE_DOWN", 0x4e },
    { "RIGHT", 0x4f }, { "LEFT", 0x50 }, { "DOWN", 0x51 }, { "UP", 0x52 },
};

static const name_u8_t s_modifiers[] = {
    { "CTRL", MOD_CTRL }, { "CONTROL", MOD_CTRL }, { "SHIFT", MOD_SHIFT }, { "ALT", MOD_ALT },
    { "OPTION", MOD_ALT }, { "GUI", MOD_GUI }, { "COMMAND", MOD_GUI }, { "CMD", MOD_GUI },
    { "META", MOD_GUI }, { "RIGHT_CTRL", MOD_RIGHT_CTRL }, { "RIGHT_SHIFT", MOD_RIGHT_SHIFT },
    { "RIGHT_ALT", MOD_RIGHT_ALT }, { "RIGHT_GUI", MOD_RIGHT_GUI },
};

static const name_u8_t s_mouse_button_map[] = {
    { "left", MOUSE_LEFT }, { "right", MOUSE_RIGHT }, { "middle", MOUSE_MIDDLE },
};

static const ascii_key_t s_ascii[] = {
    { '\n', 0, 0x28 }, { '\t', 0, 0x2b }, { ' ', 0, 0x2c },
    { '!', MOD_SHIFT, 0x1e }, { '@', MOD_SHIFT, 0x1f }, { '#', MOD_SHIFT, 0x20 },
    { '$', MOD_SHIFT, 0x21 }, { '%', MOD_SHIFT, 0x22 }, { '^', MOD_SHIFT, 0x23 },
    { '&', MOD_SHIFT, 0x24 }, { '*', MOD_SHIFT, 0x25 }, { '(', MOD_SHIFT, 0x26 },
    { ')', MOD_SHIFT, 0x27 }, { '-', 0, 0x2d }, { '_', MOD_SHIFT, 0x2d },
    { '=', 0, 0x2e }, { '+', MOD_SHIFT, 0x2e }, { '[', 0, 0x2f }, { '{', MOD_SHIFT, 0x2f },
    { ']', 0, 0x30 }, { '}', MOD_SHIFT, 0x30 }, { '\\', 0, 0x31 }, { '|', MOD_SHIFT, 0x31 },
    { ';', 0, 0x33 }, { ':', MOD_SHIFT, 0x33 }, { '\'', 0, 0x34 }, { '"', MOD_SHIFT, 0x34 },
    { '`', 0, 0x35 }, { '~', MOD_SHIFT, 0x35 }, { ',', 0, 0x36 }, { '<', MOD_SHIFT, 0x36 },
    { '.', 0, 0x37 }, { '>', MOD_SHIFT, 0x37 }, { '/', 0, 0x38 }, { '?', MOD_SHIFT, 0x38 },
};

static bool lookup(const name_u8_t *items, size_t count, const char *name, uint8_t *out)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(items[i].name, name) == 0) {
            *out = items[i].value;
            return true;
        }
    }
    return false;
}

static bool ascii_lookup(char ch, uint8_t *modifier, uint8_t *keycode)
{
    if (ch >= 'a' && ch <= 'z') {
        *modifier = 0;
        *keycode = (uint8_t)(0x04 + (ch - 'a'));
        return true;
    }
    if (ch >= 'A' && ch <= 'Z') {
        *modifier = MOD_SHIFT;
        *keycode = (uint8_t)(0x04 + (ch - 'A'));
        return true;
    }
    if (ch >= '1' && ch <= '9') {
        *modifier = 0;
        *keycode = (uint8_t)(0x1e + (ch - '1'));
        return true;
    }
    if (ch == '0') {
        *modifier = 0;
        *keycode = 0x27;
        return true;
    }
    for (size_t i = 0; i < sizeof(s_ascii) / sizeof(s_ascii[0]); i++) {
        if (s_ascii[i].ch == ch) {
            *modifier = s_ascii[i].modifier;
            *keycode = s_ascii[i].keycode;
            return true;
        }
    }
    return false;
}

static int8_t clamp_i8(lua_Number value)
{
    if (value > 127.0) {
        return 127;
    }
    if (value < -127.0) {
        return -127;
    }
    return value >= 0.0 ? (int8_t)(value + 0.5) : (int8_t)(value - 0.5);
}

static void hidd_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)data;

    (void)arg;
    (void)base;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_CONNECT_EVENT:
        s_status.connected = param ? esp_hidd_dev_connected(param->connect.dev) : true;
        ESP_LOGI(TAG, "HID connected");
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        s_status.connected = false;
        s_mouse_buttons = 0;
        ESP_LOGI(TAG, "HID disconnected");
        break;
    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG, "HID protocol mode=%u", param ? param->protocol_mode.protocol_mode : 0);
        break;
    default:
        break;
    }
}

static const char *hid_err_reason(esp_err_t err)
{
    if (err == ESP_OK) {
        return NULL;
    }
    if (!s_status.initialized || !s_hid_dev) {
        return "HID not initialized";
    }
    if (!esp_hidd_dev_connected(s_hid_dev)) {
        return "not connected";
    }
    return esp_err_to_name(err);
}

static int push_result(lua_State *L, esp_err_t err)
{
    if (err == ESP_OK) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, hid_err_reason(err));
    return 2;
}

static esp_err_t send_report(size_t report_id, uint8_t *data, size_t len)
{
    if (!s_hid_dev || !s_status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_hidd_dev_input_set(s_hid_dev, LUA_HID_MAP_INDEX, report_id, data, len);
}

static esp_err_t send_consumer(uint8_t report)
{
    uint8_t data[CONSUMER_REPORT_SIZE] = { report };
    return send_report(REPORT_ID_CONSUMER, data, sizeof(data));
}

static esp_err_t send_keyboard(uint8_t modifier, const uint8_t *keys, size_t key_count)
{
    uint8_t data[KEYBOARD_REPORT_SIZE] = { 0 };

    if (key_count > KEYBOARD_MAX_KEYS || (key_count > 0 && !keys)) {
        return ESP_ERR_INVALID_ARG;
    }
    data[0] = modifier;
    for (size_t i = 0; i < key_count; i++) {
        data[2 + i] = keys[i];
    }
    return send_report(REPORT_ID_KEYBOARD, data, sizeof(data));
}

static esp_err_t send_mouse(int8_t x, int8_t y, int8_t wheel, int8_t pan)
{
    uint8_t data[MOUSE_REPORT_SIZE] = {
        s_mouse_buttons & MOUSE_MASK,
        (uint8_t)x,
        (uint8_t)y,
        (uint8_t)wheel,
        (uint8_t)pan,
    };
    return send_report(REPORT_ID_MOUSE, data, sizeof(data));
}

static esp_err_t keyboard_press_release(uint8_t modifier, const uint8_t *keys, size_t key_count)
{
    esp_err_t err = send_keyboard(modifier, keys, key_count);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_HOLD_MS));
    err = send_keyboard(0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_RELEASE_MS));
    return ESP_OK;
}

static esp_err_t release_all(bool notify)
{
    esp_err_t err;

    s_mouse_buttons = 0;
    if (!notify || !s_hid_dev || !esp_hidd_dev_connected(s_hid_dev)) {
        return ESP_OK;
    }
    err = send_consumer(0);
    if (err != ESP_OK) {
        return err;
    }
    err = send_keyboard(0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    return send_mouse(0, 0, 0, 0);
}

static esp_err_t init_hid_device(const char *name)
{
    esp_hid_device_config_t config = {
        .vendor_id = 0x303a,
        .product_id = 0x4001,
        .version = 0x0100,
        .device_name = name,
        .manufacturer_name = "Espressif",
        .serial_number = "esp-claw",
        .report_maps = s_report_maps,
        .report_maps_len = sizeof(s_report_maps) / sizeof(s_report_maps[0]),
    };
    esp_err_t err;

    if (s_hid_dev) {
        return ESP_OK;
    }
    err = ble_hid_stack_prepare();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_hidd_dev_init(&config, ESP_HID_TRANSPORT_BLE, hidd_event_handler, &s_hid_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidd_dev_init failed: %s", esp_err_to_name(err));
        s_hid_dev = NULL;
        return err;
    }
    err = ble_hid_stack_start();
    if (err != ESP_OK) {
        (void)esp_hidd_dev_deinit(s_hid_dev);
        s_hid_dev = NULL;
        return err;
    }
    s_status.initialized = true;
    s_status.connected = esp_hidd_dev_connected(s_hid_dev);
    return ESP_OK;
}

static int lua_ble_hid_init(lua_State *L)
{
    const char *name = LUA_HID_DEFAULT_NAME;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "name");
        if (lua_type(L, -1) == LUA_TSTRING) {
            name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }
    if (strlen(name) > LUA_HID_NAME_MAX) {
        return luaL_error(L, "name length must be <= %d", LUA_HID_NAME_MAX);
    }
    return push_result(L, init_hid_device(name));
}

static int lua_ble_hid_deinit(lua_State *L)
{
    esp_err_t err = ESP_OK;

    (void)release_all(true);
    (void)ble_hid_stack_stop();
    if (s_hid_dev) {
        err = esp_hidd_dev_deinit(s_hid_dev);
        s_hid_dev = NULL;
    }
    s_status.initialized = false;
    s_status.advertising = false;
    s_status.connected = false;
    s_status.bonded = false;
    return push_result(L, err);
}

static int lua_ble_hid_start(lua_State *L)
{
    const uint16_t uuid16 = 0x1812;
    const char *name = LUA_HID_DEFAULT_NAME;
    esp_err_t err;

    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "name");
        if (lua_type(L, -1) == LUA_TSTRING) {
            name = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }
    if (strlen(name) > LUA_HID_NAME_MAX) {
        return luaL_error(L, "name length must be <= %d", LUA_HID_NAME_MAX);
    }
    if (!s_status.initialized) {
        return luaL_error(L, "ble_hid.init() must succeed before ble_hid.start()");
    }
    err = ble_hid_adv_start(name, ESP_HID_APPEARANCE_MOUSE, &uuid16, 1);
    if (err == ESP_OK) {
        s_status.advertising = true;
    }
    return push_result(L, err);
}

static int lua_ble_hid_stop(lua_State *L)
{
    esp_err_t err = ble_hid_adv_stop();
    if (err == ESP_OK) {
        s_status.advertising = false;
    }
    return push_result(L, err);
}

static int lua_ble_hid_media(lua_State *L)
{
    const char *key = luaL_checkstring(L, 1);
    const char *gesture = luaL_optstring(L, 2, "single");
    uint8_t report = 0;
    uint8_t repeats = 1;
    uint32_t press_ms = CONSUMER_PRESS_MS;
    esp_err_t err;

    if (!lookup(s_consumer, sizeof(s_consumer) / sizeof(s_consumer[0]), key, &report)) {
        return luaL_error(L, "unsupported media key: %s", key);
    }
    if (strcmp(gesture, "double") == 0) {
        repeats = 2;
    } else if (strcmp(gesture, "long") == 0) {
        press_ms = CONSUMER_LONG_PRESS_MS;
    } else if (strcmp(gesture, "single") != 0) {
        return luaL_error(L, "unsupported media gesture: %s", gesture);
    }
    for (uint8_t i = 0; i < repeats; i++) {
        err = send_consumer(report);
        if (err != ESP_OK) {
            return push_result(L, err);
        }
        vTaskDelay(pdMS_TO_TICKS(press_ms));
        err = send_consumer(0);
        if (err != ESP_OK) {
            return push_result(L, err);
        }
        if (i + 1 < repeats) {
            vTaskDelay(pdMS_TO_TICKS(CONSUMER_RELEASE_GAP_MS));
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_key(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    uint8_t key = 0;

    if (!lookup(s_keys, sizeof(s_keys) / sizeof(s_keys[0]), name, &key)) {
        return luaL_error(L, "unsupported keyboard key: %s", name);
    }
    return push_result(L, keyboard_press_release(0, &key, 1));
}

static int lua_ble_hid_combo(lua_State *L)
{
    int argc = lua_gettop(L);
    uint8_t modifier = 0;
    uint8_t keys[KEYBOARD_MAX_KEYS] = { 0 };
    size_t key_count = 0;

    if (argc <= 0) {
        return luaL_error(L, "ble_hid.combo requires at least one key or modifier");
    }
    for (int i = 1; i <= argc; i++) {
        const char *name = luaL_checkstring(L, i);
        uint8_t value = 0;
        if (lookup(s_modifiers, sizeof(s_modifiers) / sizeof(s_modifiers[0]), name, &value)) {
            modifier |= value;
        } else if (lookup(s_keys, sizeof(s_keys) / sizeof(s_keys[0]), name, &value)) {
            if (key_count >= KEYBOARD_MAX_KEYS) {
                return luaL_error(L, "keyboard combo supports at most %d non-modifier keys", KEYBOARD_MAX_KEYS);
            }
            keys[key_count++] = value;
        } else {
            return luaL_error(L, "unsupported keyboard key or modifier: %s", name);
        }
    }
    return push_result(L, keyboard_press_release(modifier, keys, key_count));
}

static int lua_ble_hid_text(lua_State *L)
{
    size_t len = 0;
    const char *text = luaL_checklstring(L, 1, &len);

    for (size_t i = 0; i < len; i++) {
        uint8_t modifier = 0;
        uint8_t key = 0;
        if (!ascii_lookup(text[i], &modifier, &key)) {
            return luaL_error(L, "unsupported text character at byte %u", (unsigned int)i + 1);
        }
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t modifier = 0;
        uint8_t key = 0;
        esp_err_t err;
        (void)ascii_lookup(text[i], &modifier, &key);
        err = keyboard_press_release(modifier, &key, 1);
        if (err != ESP_OK) {
            return push_result(L, err);
        }
        vTaskDelay(pdMS_TO_TICKS(TEXT_GAP_MS));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_ble_hid_mouse_move(lua_State *L)
{
    int8_t x = clamp_i8(luaL_checknumber(L, 1));
    int8_t y = clamp_i8(luaL_checknumber(L, 2));
    int8_t wheel = clamp_i8(luaL_optnumber(L, 3, 0));
    int8_t pan = clamp_i8(luaL_optnumber(L, 4, 0));
    return push_result(L, send_mouse(x, y, wheel, pan));
}

static int lua_ble_hid_mouse_scroll(lua_State *L)
{
    int8_t wheel = clamp_i8(luaL_checknumber(L, 1));
    int8_t pan = clamp_i8(luaL_optnumber(L, 2, 0));
    return push_result(L, send_mouse(0, 0, wheel, pan));
}

static int lua_ble_hid_mouse_button(lua_State *L)
{
    const char *button = luaL_checkstring(L, 1);
    const char *gesture = luaL_optstring(L, 2, "click");
    uint8_t mask = 0;
    esp_err_t err;

    if (!lookup(s_mouse_button_map, sizeof(s_mouse_button_map) / sizeof(s_mouse_button_map[0]), button, &mask)) {
        return luaL_error(L, "unsupported mouse button: %s", button);
    }
    if (strcmp(gesture, "down") == 0) {
        s_mouse_buttons |= mask;
        return push_result(L, send_mouse(0, 0, 0, 0));
    }
    if (strcmp(gesture, "up") == 0) {
        s_mouse_buttons &= (uint8_t)~mask;
        return push_result(L, send_mouse(0, 0, 0, 0));
    }
    if (strcmp(gesture, "click") != 0) {
        return luaL_error(L, "unsupported mouse gesture: %s", gesture);
    }
    s_mouse_buttons |= mask;
    err = send_mouse(0, 0, 0, 0);
    if (err != ESP_OK) {
        return push_result(L, err);
    }
    vTaskDelay(pdMS_TO_TICKS(MOUSE_CLICK_MS));
    s_mouse_buttons &= (uint8_t)~mask;
    return push_result(L, send_mouse(0, 0, 0, 0));
}

static int lua_ble_hid_release_all(lua_State *L)
{
    return push_result(L, release_all(true));
}

static int lua_ble_hid_status(lua_State *L)
{
    s_status.connected = s_hid_dev ? esp_hidd_dev_connected(s_hid_dev) : false;
    s_status.bonded = s_conn.bonded;
    lua_newtable(L);
    lua_pushboolean(L, s_status.initialized);
    lua_setfield(L, -2, "initialized");
    lua_pushboolean(L, s_status.advertising);
    lua_setfield(L, -2, "advertising");
    lua_pushboolean(L, s_status.connected);
    lua_setfield(L, -2, "connected");
    lua_pushboolean(L, s_status.bonded);
    lua_setfield(L, -2, "bonded");
    return 1;
}

static int lua_ble_hid_describe(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, REPORT_ID_CONSUMER);
    lua_setfield(L, -2, "consumer_report_id");
    lua_pushinteger(L, REPORT_ID_KEYBOARD);
    lua_setfield(L, -2, "keyboard_report_id");
    lua_pushinteger(L, REPORT_ID_MOUSE);
    lua_setfield(L, -2, "mouse_report_id");
    lua_pushstring(L, "basic ASCII simulation on standard US keyboard layout");
    lua_setfield(L, -2, "text_scope");
    lua_pushboolean(L, false);
    lua_setfield(L, -2, "text_unicode");
    return 1;
}

static void set_fns(lua_State *L, const lua_hid_fn_t *fns, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        lua_pushcfunction(L, fns[i].fn);
        lua_setfield(L, -2, fns[i].name);
    }
}

int luaopen_ble_hid(lua_State *L)
{
    static const lua_hid_fn_t fns[] = {
        { "init", lua_ble_hid_init }, { "deinit", lua_ble_hid_deinit }, { "start", lua_ble_hid_start },
        { "stop", lua_ble_hid_stop }, { "status", lua_ble_hid_status }, { "media", lua_ble_hid_media },
        { "key", lua_ble_hid_key }, { "combo", lua_ble_hid_combo }, { "text", lua_ble_hid_text },
        { "mouse_move", lua_ble_hid_mouse_move }, { "mouse_scroll", lua_ble_hid_mouse_scroll },
        { "mouse_button", lua_ble_hid_mouse_button }, { "release_all", lua_ble_hid_release_all },
        { "describe", lua_ble_hid_describe },
    };

    lua_newtable(L);
    set_fns(L, fns, sizeof(fns) / sizeof(fns[0]));
    return 1;
}

esp_err_t lua_module_ble_hid_register(void)
{
    return cap_lua_register_module(LUA_MODULE_BLE_HID_NAME, luaopen_ble_hid);
}
