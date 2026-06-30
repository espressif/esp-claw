/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lua_module_ble_priv.h"

#include "esp_timer.h"
#include "nvs_flash.h"

static void lua_ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    if (s_host_exit_sem) {
        (void)xSemaphoreGive(s_host_exit_sem);
    }
    nimble_port_freertos_deinit();
}

static int lua_ble_cccd_reserve_access_cb(uint16_t conn_handle,
                                          uint16_t attr_handle,
                                          struct ble_gatt_access_ctxt *ctxt,
                                          void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_UNLIKELY;
}

static esp_err_t lua_ble_stack_start(void);
static esp_err_t lua_ble_stack_stop(void);
static esp_err_t lua_ble_stack_prepare(void);

typedef struct {
    uint8_t conn_index;
    uint16_t conn_handle;
} lua_ble_conn_snapshot_t;

typedef struct {
    int active_connections;
    int active_after_wait;
    bool stack_inited;
    bool advertising;
    bool advertising_requested;
    bool wait_timed_out;
} lua_ble_deinit_ctx_t;

static uint8_t lua_ble_snapshot_connections(lua_ble_conn_snapshot_t *out, uint8_t max)
{
    uint8_t count = 0;
    uint8_t slots = lua_ble_slot_count();

    if (!out || max == 0) {
        return 0;
    }

    lua_ble_runtime_lock();
    for (uint8_t i = 0; i < slots && count < max; i++) {
        if (s_conns[i].connected) {
            out[count].conn_index = i;
            out[count].conn_handle = s_conns[i].conn_handle;
            count++;
        }
    }
    lua_ble_runtime_unlock();
    return count;
}

static void lua_ble_deinit_force_disconnect_event(uint8_t conn_index, uint16_t conn_handle, const char *detail)
{
    lua_ble_event_t *event = lua_ble_event_alloc();

    if (!event) {
        ESP_LOGW(TAG, "BLE event alloc failed type=%d", LUA_BLE_EVENT_DISCONNECTED);
        return;
    }

    event->type = LUA_BLE_EVENT_DISCONNECTED;
    event->conn_index = conn_index;
    event->conn_handle = conn_handle;
    strlcpy(event->reason, "deinit", sizeof(event->reason));
    ESP_LOGW(TAG, "[deinit]: forcing disconnect event conn_index=%u conn_handle=%u detail=%s",
             conn_index, conn_handle, detail ? detail : "unknown");
    lua_ble_event_enqueue_force(event);
    /* The real GAP disconnect may arrive later; clearing the slot here prevents duplicate Lua events. */
    lua_ble_connection_clear_index(conn_index);
    if (s_disconnect_sem) {
        (void)xSemaphoreGive(s_disconnect_sem);
    }
}

static void lua_ble_deinit_enter(lua_ble_deinit_ctx_t *ctx)
{
    uint8_t slots = lua_ble_slot_count();

    memset(ctx, 0, sizeof(*ctx));
    lua_ble_runtime_lock();
    for (uint8_t i = 0; i < slots; i++) {
        if (s_conns[i].connected) {
            ctx->active_connections++;
        }
    }
    ctx->stack_inited = s_stack_inited;
    ctx->advertising = s_advertising;
    ctx->advertising_requested = s_advertising_requested;
    s_draining = true;
    lua_ble_runtime_unlock();

    ESP_LOGI(TAG, "[deinit]: enter draining active_connections=%d adv_active=%u adv_requested=%u",
             ctx->active_connections, ctx->advertising, ctx->advertising_requested);
}

static void lua_ble_deinit_stop_advertising(const lua_ble_deinit_ctx_t *ctx)
{
    if (ctx->advertising && ctx->advertising_requested) {
        lua_ble_event_simple_adv(LUA_BLE_EVENT_ADV_STOPPED, "deinit", 0);
    }
    if (ctx->stack_inited && ctx->advertising) {
        (void)ble_gap_adv_stop();
    }
}

static void lua_ble_deinit_terminate_connections(void)
{
    lua_ble_conn_snapshot_t conns[LUA_BLE_MAX_CONNECTIONS];
    uint8_t count = lua_ble_snapshot_connections(conns, LUA_BLE_MAX_CONNECTIONS);

    for (uint8_t i = 0; i < count; i++) {
        int rc = ble_gap_terminate(conns[i].conn_handle, BLE_ERR_CONN_TERM_LOCAL);
        if (rc != 0) {
            ESP_LOGW(TAG, "[deinit]: terminate failed conn_index=%u conn_handle=%u rc=%d",
                     conns[i].conn_index, conns[i].conn_handle, rc);
            lua_ble_deinit_force_disconnect_event(conns[i].conn_index, conns[i].conn_handle, "terminate_failed");
        }
    }
}

static void lua_ble_deinit_wait_disconnects(lua_ble_deinit_ctx_t *ctx)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)LUA_BLE_DEINIT_DISCONNECT_TIMEOUT_MS * 1000;

    while (lua_ble_connection_count() > 0) {
        int64_t now = esp_timer_get_time();
        if (now >= deadline) {
            ctx->wait_timed_out = true;
            break;
        }
        TickType_t wait = pdMS_TO_TICKS(100);
        if (s_disconnect_sem) {
            (void)xSemaphoreTake(s_disconnect_sem, wait);
        } else {
            vTaskDelay(wait);
        }
    }
    ctx->active_after_wait = lua_ble_connection_count();
    ESP_LOGI(TAG, "[deinit]: disconnect wait active_before=%d active_after=%d timed_out=%u",
             ctx->active_connections, ctx->active_after_wait, ctx->wait_timed_out);
}

static void lua_ble_deinit_force_remaining_disconnects(const lua_ble_deinit_ctx_t *ctx)
{
    lua_ble_conn_snapshot_t conns[LUA_BLE_MAX_CONNECTIONS];
    uint8_t count;

    if (!ctx->wait_timed_out) {
        return;
    }

    ESP_LOGW(TAG, "[deinit]: disconnect wait timed out; forcing host stop with active connections");
    count = lua_ble_snapshot_connections(conns, LUA_BLE_MAX_CONNECTIONS);
    for (uint8_t i = 0; i < count; i++) {
        lua_ble_deinit_force_disconnect_event(conns[i].conn_index, conns[i].conn_handle, "timeout");
    }
}

int lua_ble_init(lua_State *L)
{
    const char *err = lua_ble_smp_init_error();

    if (s_stack_unrecoverable) {
        return lua_ble_push_err_string(L, "ble_reboot_required");
    }
    if (err) {
        return lua_ble_push_err_string(L, err);
    }
    ESP_LOGI(TAG, "[init]: start");
    return lua_ble_push_ok_or_err(L, lua_ble_stack_start());
}

int lua_ble_deinit(lua_State *L)
{
    lua_ble_deinit_ctx_t ctx;
    bool stack_active;

    if (s_stack_unrecoverable) {
        return lua_ble_push_err_string(L, "ble_reboot_required");
    }
    lua_ble_runtime_lock();
    if (s_draining) {
        lua_ble_runtime_unlock();
        return lua_ble_push_err_string(L, "ble_invalid_state");
    }
    stack_active = s_stack_inited || s_nimble_inited ||
                   s_host_task_started || s_controller_inited_by_ble ||
                   s_controller_enabled_by_ble;
    if (!stack_active) {
        lua_ble_runtime_unlock();
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_ble_runtime_unlock();
    lua_ble_deinit_enter(&ctx);
    lua_ble_deinit_stop_advertising(&ctx);
    lua_ble_deinit_terminate_connections();
    lua_ble_deinit_wait_disconnects(&ctx);
    lua_ble_deinit_force_remaining_disconnects(&ctx);
    lua_ble_events_drain(L);
    ESP_LOGI(TAG, "[deinit]: stop");
    return lua_ble_push_ok_or_err(L, lua_ble_stack_stop());
}

static esp_err_t lua_ble_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS needs recovery before BLE init: %s", esp_err_to_name(err));
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    return err;
}

static const char *lua_ble_controller_status_name(esp_bt_controller_status_t status)
{
    switch (status) {
    case ESP_BT_CONTROLLER_STATUS_IDLE:
        return "IDLE";
    case ESP_BT_CONTROLLER_STATUS_INITED:
        return "INITED";
    case ESP_BT_CONTROLLER_STATUS_ENABLED:
        return "ENABLED";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t lua_ble_controller_start(void)
{
    esp_err_t err;
    esp_bt_controller_status_t status = esp_bt_controller_get_status();

    ESP_LOGI(TAG, "BLE controller status before init: %s", lua_ble_controller_status_name(status));

    if (status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK) {
            return err;
        }
        s_controller_inited_by_ble = true;
    } else if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        return ESP_OK;
    } else if (status != ESP_BT_CONTROLLER_STATUS_INITED) {
        return ESP_ERR_INVALID_STATE;
    }

    status = esp_bt_controller_get_status();
    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        return ESP_OK;
    }
    if (status != ESP_BT_CONTROLLER_STATUS_INITED) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        return err;
    }
    s_controller_enabled_by_ble = true;
    return ESP_OK;
}

static void lua_ble_controller_stop(void)
{
    esp_err_t err;
    esp_bt_controller_status_t status = esp_bt_controller_get_status();

    if (status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        if (!s_controller_enabled_by_ble) {
            return;
        }
        err = esp_bt_controller_disable();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_bt_controller_disable returned %s", esp_err_to_name(err));
            return;
        }
        s_controller_enabled_by_ble = false;
        status = esp_bt_controller_get_status();
    }

    if (status == ESP_BT_CONTROLLER_STATUS_INITED) {
        if (!s_controller_inited_by_ble) {
            return;
        }
        err = esp_bt_controller_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_bt_controller_deinit returned %s", esp_err_to_name(err));
            return;
        }
        s_controller_inited_by_ble = false;
    }
}

static esp_err_t lua_ble_stack_prepare(void)
{
    esp_err_t err;

    err = lua_ble_runtime_ensure();
    if (err != ESP_OK) {
        return err;
    }
    if (s_nimble_inited) {
        return ESP_OK;
    }

    err = lua_ble_nvs_init();
    if (err != ESP_OK) {
        return err;
    }
    if (s_host_sync_sem == NULL) {
        s_host_sync_sem = xSemaphoreCreateBinary();
        if (s_host_sync_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_host_exit_sem == NULL) {
        s_host_exit_sem = xSemaphoreCreateBinary();
        if (s_host_exit_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_disconnect_sem == NULL) {
        s_disconnect_sem = xSemaphoreCreateBinary();
        if (s_disconnect_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_event_queue == NULL) {
        s_event_queue = xQueueCreate(LUA_BLE_EVENT_QUEUE_LEN, sizeof(lua_ble_event_t *));
        if (s_event_queue == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    err = lua_ble_controller_start();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_nimble_init();
    if (err != ESP_OK) {
        lua_ble_controller_stop();
        return err;
    }
    s_nimble_inited = true;
    return ESP_OK;
}

static void lua_ble_stack_delete_runtime_objects(void)
{
    if (s_event_queue) {
        lua_ble_events_clear();
        vQueueDelete(s_event_queue);
        s_event_queue = NULL;
    }
    if (s_disconnect_sem) {
        vSemaphoreDelete(s_disconnect_sem);
        s_disconnect_sem = NULL;
    }
    if (s_host_exit_sem) {
        vSemaphoreDelete(s_host_exit_sem);
        s_host_exit_sem = NULL;
    }
    if (s_host_sync_sem) {
        vSemaphoreDelete(s_host_sync_sem);
        s_host_sync_sem = NULL;
    }
}

static void lua_ble_stack_reserve_cccd_cfg(void)
{
    static const ble_uuid16_t s_cccd_reserve_chr_uuid = BLE_UUID16_INIT(0xfff1);
    static const ble_uuid16_t s_cccd_reserve_svc_uuid = BLE_UUID16_INIT(0xfff0);

    static struct ble_gatt_chr_def s_cccd_reserve_chrs[LUA_BLE_GATTS_MAX_CHARS + 1];
    static struct ble_gatt_svc_def s_cccd_reserve_svcs[2];
    int rc;

    for (int i = 0; i < LUA_BLE_GATTS_MAX_CHARS; i++) {
        s_cccd_reserve_chrs[i] = (struct ble_gatt_chr_def){
            .uuid       = &s_cccd_reserve_chr_uuid.u,
            .access_cb  = lua_ble_cccd_reserve_access_cb,
            .flags      = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE,
        };
    }
    memset(&s_cccd_reserve_chrs[LUA_BLE_GATTS_MAX_CHARS], 0, sizeof(s_cccd_reserve_chrs[0]));

    s_cccd_reserve_svcs[0] = (struct ble_gatt_svc_def){
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_cccd_reserve_svc_uuid.u,
        .characteristics = s_cccd_reserve_chrs,
    };
    memset(&s_cccd_reserve_svcs[1], 0, sizeof(s_cccd_reserve_svcs[0]));

    rc = ble_gatts_count_cfg(s_cccd_reserve_svcs);
    if (rc != 0) {
        ESP_LOGW(TAG, "[init]: ble_gatts_count_cfg for CCCD reserve failed rc=%d", rc);
    }
}

static esp_err_t lua_ble_stack_start(void)
{
    esp_err_t err;
    int rc;

    if (s_stack_unrecoverable) {
        return LUA_BLE_ERR_REBOOT_REQUIRED;
    }
    if (s_stack_inited) {
        return ESP_OK;
    }
    if (lua_ble_slot_count() < 1) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    err = lua_ble_stack_prepare();
    if (err != ESP_OK) {
        return err;
    }

    ble_hs_cfg.reset_cb = lua_ble_on_reset;
    ble_hs_cfg.sync_cb = lua_ble_on_sync;
    ble_hs_cfg.gatts_register_cb = NULL;
    ble_hs_cfg.gatts_register_arg = NULL;
    lua_ble_smp_apply_default_config();

    rc = ble_att_set_preferred_mtu(s_preferred_mtu);
    if (rc != 0) {
        return ESP_FAIL;
    }

    ble_svc_gap_init();
    ble_svc_gatt_init();
    lua_ble_stack_reserve_cccd_cfg();

    rc = ble_svc_gap_device_name_set(LUA_BLE_DEFAULT_NAME);
    if (rc != 0) {
        return ESP_FAIL;
    }

    s_host_synced = false;
    nimble_port_freertos_init(lua_ble_host_task);
    s_host_task_started = true;

    if (xSemaphoreTake(s_host_sync_sem, pdMS_TO_TICKS(LUA_BLE_HOST_SYNC_TIMEOUT_MS)) != pdTRUE) {
        err = lua_ble_stack_stop();
        if (err == LUA_BLE_ERR_REBOOT_REQUIRED) {
            return err;
        }
        return ESP_ERR_TIMEOUT;
    }

    lua_ble_runtime_lock();
    s_event_dropped = 0;
    lua_ble_runtime_unlock();
    lua_ble_connection_init_slots();
    s_stack_inited = true;
    return ESP_OK;
}

static esp_err_t lua_ble_stack_stop(void)
{
    esp_err_t err;
    esp_err_t ret = ESP_OK;

    if (s_stack_unrecoverable) {
        return LUA_BLE_ERR_REBOOT_REQUIRED;
    }
    if (!s_stack_inited && !s_nimble_inited &&
            !s_host_task_started && !s_controller_inited_by_ble && !s_controller_enabled_by_ble) {
        return ESP_OK;
    }

    if (s_advertising) {
        (void)ble_gap_adv_stop();
        s_advertising = false;
    }
    if (s_host_task_started) {
        if (s_host_exit_sem) {
            (void)xSemaphoreTake(s_host_exit_sem, 0);
        }
        err = nimble_port_stop();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nimble_port_stop returned %s", esp_err_to_name(err));
            ret = err;
        } else {
            bool host_exit_timeout = s_host_exit_sem &&
                xSemaphoreTake(s_host_exit_sem, pdMS_TO_TICKS(LUA_BLE_HOST_EXIT_TIMEOUT_MS)) != pdTRUE;
            if (host_exit_timeout) {
                ESP_LOGW(TAG, "[deinit]: host task exit wait timed out");
                s_stack_unrecoverable = true;
                return LUA_BLE_ERR_REBOOT_REQUIRED;
            } else {
                s_host_task_started = false;
            }
        }
    }

    if (s_nimble_inited) {
        err = esp_nimble_deinit();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_nimble_deinit returned %s", esp_err_to_name(err));
        } else {
            s_nimble_inited = false;
        }
    }

    lua_ble_controller_stop();
    s_stack_inited = false;
    s_host_synced = false;
    s_advertising_requested = false;
    s_advertising = false;
    s_advertising_paused_for_connection_full = false;
    s_draining = false;
    lua_ble_events_clear();
    lua_ble_connection_clear();
    lua_ble_gatts_reset();
    lua_ble_stack_delete_runtime_objects();
    if (s_ble_rt.mutex) {
        vSemaphoreDelete(s_ble_rt.mutex);
        s_ble_rt.mutex = NULL;
    }
    return ret;
}

