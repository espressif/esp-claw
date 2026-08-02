/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_power_mgr.h"
#include "wave_rover_mcp_state.h"
#include "wave_rover_hal.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "wr_power_mgr";

#define WR_POWER_MAX_LOCKS 4
#define WR_POWER_EVAL_PERIOD_MS 1000

typedef struct {
    bool     active;
    char     reason[32];
    uint32_t lock_id;
    int64_t  expires_at_us;   /* 0 = never expires */
} wr_power_lock_t;

struct wr_power_mgr_t {
    wr_power_mgr_config_t config;
    wr_power_mode_t       mode;
    int64_t               last_activity_us;
    int64_t               start_us;
    wr_power_lock_t       locks[WR_POWER_MAX_LOCKS];
    uint32_t              next_lock_id;
    SemaphoreHandle_t     mutex;
    SemaphoreHandle_t     stopped_sem;
    TaskHandle_t          eval_task;
    bool                  running;
};

const char *wr_power_mode_name(wr_power_mode_t mode)
{
    switch (mode) {
    case WR_POWER_MODE_ACTIVE:    return "active";
    case WR_POWER_MODE_IDLE:      return "idle";
    case WR_POWER_MODE_LOW_POWER: return "low_power";
    default:                      return "unknown";
    }
}

/* Side effects applied on transition. Always succeeds — individual
 * peripheral failures inside it are logged but never block the
 * transition. Wi-Fi and PM errors are logged here; HAL peripherals
 * (mag, display) log failures internally. */
static void apply_mode(struct wr_power_mgr_t *m, wr_power_mode_t mode)
{
    if (m->config.wifi_power_save) {
        wifi_ps_type_t ps;
        switch (mode) {
        case WR_POWER_MODE_ACTIVE:    ps = WIFI_PS_NONE; break;
        case WR_POWER_MODE_IDLE:      ps = WIFI_PS_MIN_MODEM; break;
        case WR_POWER_MODE_LOW_POWER: ps = WIFI_PS_MAX_MODEM; break;
        default:                      ps = WIFI_PS_NONE; break;
        }
        /* esp_wifi_set_ps() returns ESP_ERR_WIFI_NOT_INIT if called before
         * Wi-Fi starts (e.g. first transition at power_mgr startup); this is
         * expected and the warning is the intended handling. */
        esp_err_t err = esp_wifi_set_ps(ps);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "power: esp_wifi_set_ps(%d) failed: %s", ps, esp_err_to_name(err));
        }
    }

    if (m->config.reduce_cpu_frequency) {
        int min_mhz = (mode == WR_POWER_MODE_ACTIVE) ? 240 : 80; /* lock to max in ACTIVE mode */
        esp_pm_config_t pm_cfg = {
            .max_freq_mhz = 240,
            .min_freq_mhz = min_mhz,
            .light_sleep_enable = false,
        };
        esp_err_t err = esp_pm_configure(&pm_cfg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "power: esp_pm_configure(min=%d) failed: %s", min_mhz, esp_err_to_name(err));
        }
    }

    wr_imu_set_mag_continuous(mode == WR_POWER_MODE_ACTIVE);

    if (m->config.disable_display_when_idle) {
        /* display off only in LOW_POWER; stays on in IDLE for readability */
        wr_display_set_power(mode != WR_POWER_MODE_LOW_POWER);
    }
}

/* Caller must hold m->mutex. */
static void transition_locked(struct wr_power_mgr_t *m, wr_power_mode_t new_mode,
                               const char *reason)
{
    if (m->mode == new_mode) return;
    wr_power_mode_t old = m->mode;
    apply_mode(m, new_mode);
    m->mode = new_mode;
    ESP_LOGI(TAG, "power: mode changed %s -> %s, reason=%s",
             wr_power_mode_name(old), wr_power_mode_name(new_mode), reason);
}

/* Caller must hold m->mutex. Returns true if any lock is currently active
 * (after expiring stale ones). */
static bool locks_held_locked(struct wr_power_mgr_t *m)
{
    int64_t now = esp_timer_get_time();
    bool any = false;
    for (int i = 0; i < WR_POWER_MAX_LOCKS; i++) {
        wr_power_lock_t *l = &m->locks[i];
        if (!l->active) continue;
        if (l->expires_at_us != 0 && now >= l->expires_at_us) {
            ESP_LOGI(TAG, "power: lock expired, reason=%s", l->reason);
            l->active = false;
            continue;
        }
        any = true;
    }
    return any;
}

static void eval_task_fn(void *arg)
{
    struct wr_power_mgr_t *m = arg;
    while (m->running) {
        vTaskDelay(pdMS_TO_TICKS(WR_POWER_EVAL_PERIOD_MS));
        if (!m->config.enabled) continue;

        xSemaphoreTake(m->mutex, portMAX_DELAY);

        /* ESTOP means motors are stopped — treat it as idle so power can
         * reduce. Only DRIVING and NAV_BUSY actively consume power and
         * justify delaying the idle timer. */
        wr_rover_state_t rover_state = wr_rover_state_get();
        bool rover_busy = (rover_state == WR_ROVER_STATE_DRIVING ||
                           rover_state == WR_ROVER_STATE_NAV_BUSY);
        if (rover_busy) {
            m->last_activity_us = esp_timer_get_time();
        }

        bool lock_held = locks_held_locked(m);

        if (rover_busy || lock_held) {
            transition_locked(m, WR_POWER_MODE_ACTIVE,
                               rover_busy ? "rover_busy" : "lock_held");
            xSemaphoreGive(m->mutex);
            continue;
        }

        /* Critical battery check overrides idle-timer logic. */
        wr_power_status_t ps = {0};
        wr_power_get_status(&ps);
        if (ps.present && ps.load_voltage_v > 1.0f &&
            ps.load_voltage_v < m->config.critical_battery_voltage) {
            ESP_LOGW(TAG, "power: critical battery, voltage=%.2f",
                     (double)ps.load_voltage_v);
            wr_motor_stop();
            transition_locked(m, WR_POWER_MODE_LOW_POWER, "critical_battery");
            xSemaphoreGive(m->mutex);
            continue;
        }
        if (ps.present && ps.low_battery) {
            transition_locked(m, WR_POWER_MODE_LOW_POWER, "low_battery");
            xSemaphoreGive(m->mutex);
            continue;
        }

        int64_t idle_sec = (esp_timer_get_time() - m->last_activity_us) / 1000000;
        if (idle_sec >= m->config.idle_to_low_power_sec) {
            transition_locked(m, WR_POWER_MODE_LOW_POWER, "idle_timeout");
        } else if (idle_sec >= m->config.active_timeout_sec) {
            transition_locked(m, WR_POWER_MODE_IDLE, "active_timeout");
        }
        /* else: stay ACTIVE until active_timeout_sec elapses */

        xSemaphoreGive(m->mutex);
    }
    xSemaphoreGive(m->stopped_sem);
    vTaskDelete(NULL);
}

esp_err_t wr_power_mgr_create(const wr_power_mgr_config_t *config,
                               wr_power_mgr_handle_t *ret_handle)
{
    if (!config || !ret_handle) return ESP_ERR_INVALID_ARG;
    struct wr_power_mgr_t *m = calloc(1, sizeof(*m));
    if (!m) return ESP_ERR_NO_MEM;
    m->config = *config;
    m->mode = WR_POWER_MODE_ACTIVE;
    m->start_us = esp_timer_get_time();
    m->last_activity_us = m->start_us;
    m->next_lock_id = 1;
    m->mutex = xSemaphoreCreateMutex();
    if (!m->mutex) {
        free(m);
        return ESP_ERR_NO_MEM;
    }
    m->stopped_sem = xSemaphoreCreateBinary();
    if (!m->stopped_sem) {
        vSemaphoreDelete(m->mutex);
        free(m);
        return ESP_ERR_NO_MEM;
    }
    *ret_handle = m;
    return ESP_OK;
}

esp_err_t wr_power_mgr_delete(wr_power_mgr_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->eval_task) {
        handle->running = false;
        xSemaphoreTake(handle->stopped_sem, portMAX_DELAY);
    }
    if (handle->mutex) vSemaphoreDelete(handle->mutex);
    if (handle->stopped_sem) vSemaphoreDelete(handle->stopped_sem);
    free(handle);
    return ESP_OK;
}

esp_err_t wr_power_mgr_start(wr_power_mgr_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->running) return ESP_OK;
    /* Apply initial mode so WiFi/CPU settings match the starting mode
     * immediately — without this, ESP32 boots with WIFI_PS_MIN_MODEM
     * regardless of the configured mode. */
    apply_mode(handle, handle->mode);
    handle->running = true;
    BaseType_t ok = xTaskCreate(eval_task_fn, "wr_power_eval", 3072, handle, 3,
                                 &handle->eval_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void wr_power_mgr_notify_activity(wr_power_mgr_handle_t handle, const char *source)
{
    if (!handle) return;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    handle->last_activity_us = esp_timer_get_time();
    if (handle->mode != WR_POWER_MODE_ACTIVE) {
        transition_locked(handle, WR_POWER_MODE_ACTIVE, source ? source : "activity");
    }
    xSemaphoreGive(handle->mutex);
}

esp_err_t wr_power_mgr_set_mode(wr_power_mgr_handle_t handle,
                                 wr_power_mode_t mode, const char *reason)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);

    if (mode != WR_POWER_MODE_ACTIVE) {
        if (wr_rover_state_get() != WR_ROVER_STATE_IDLE) {
            ESP_LOGW(TAG, "power: mode change rejected, target=%s, reason=rover_busy",
                     wr_power_mode_name(mode));
            err = ESP_ERR_INVALID_STATE;
        } else if (locks_held_locked(handle)) {
            ESP_LOGW(TAG, "power: mode change rejected, target=%s, reason=lock_held",
                     wr_power_mode_name(mode));
            err = ESP_ERR_INVALID_STATE;
        }
    }

    if (err == ESP_OK) {
        handle->last_activity_us = esp_timer_get_time();
        transition_locked(handle, mode, reason ? reason : "manual");
    }

    xSemaphoreGive(handle->mutex);
    return err;
}

wr_power_mode_t wr_power_mgr_get_mode(wr_power_mgr_handle_t handle)
{
    if (!handle) return WR_POWER_MODE_ACTIVE;
    wr_power_mode_t mode;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    mode = handle->mode;
    xSemaphoreGive(handle->mutex);
    return mode;
}

esp_err_t wr_power_mgr_acquire_lock(wr_power_mgr_handle_t handle,
                                     const char *reason, uint32_t ttl_sec,
                                     uint32_t *ret_lock_id)
{
    if (!handle || !reason || !ret_lock_id) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_NO_MEM;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    for (int i = 0; i < WR_POWER_MAX_LOCKS; i++) {
        if (handle->locks[i].active) continue;
        handle->locks[i].active = true;
        strlcpy(handle->locks[i].reason, reason, sizeof(handle->locks[i].reason));
        handle->locks[i].lock_id = handle->next_lock_id++;
        handle->locks[i].expires_at_us =
            ttl_sec == 0 ? 0 : esp_timer_get_time() + (int64_t)ttl_sec * 1000000;
        *ret_lock_id = handle->locks[i].lock_id;
        err = ESP_OK;
        ESP_LOGI(TAG, "power: lock acquired, reason=%s, ttl=%lu", reason, (unsigned long)ttl_sec);
        if (handle->mode != WR_POWER_MODE_ACTIVE) {
            transition_locked(handle, WR_POWER_MODE_ACTIVE, "lock_acquired");
        }
        break;
    }
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t wr_power_mgr_release_lock(wr_power_mgr_handle_t handle, uint32_t lock_id)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    for (int i = 0; i < WR_POWER_MAX_LOCKS; i++) {
        if (handle->locks[i].active && handle->locks[i].lock_id == lock_id) {
            ESP_LOGI(TAG, "power: lock released, reason=%s", handle->locks[i].reason);
            handle->locks[i].active = false;
            err = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(handle->mutex);
    return err;
}

esp_err_t wr_power_mgr_get_status_json(wr_power_mgr_handle_t handle,
                                        char *buf, size_t buf_len)
{
    if (!handle || !buf) return ESP_ERR_INVALID_ARG;
    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    int last_activity_sec_ago = (int)((now - handle->last_activity_us) / 1000000);
    int uptime_sec = (int)((now - handle->start_us) / 1000000);
    wr_power_mode_t mode = handle->mode;
    bool any_lock = locks_held_locked(handle);
    xSemaphoreGive(handle->mutex);

    int n = snprintf(buf, buf_len,
        "{\"mode\":\"%s\",\"battery_voltage\":%.2f,\"low_battery\":%s,"
        "\"uptime_sec\":%d,\"last_activity_sec_ago\":%d,\"locks_active\":%s}",
        wr_power_mode_name(mode), (double)ps.load_voltage_v,
        ps.low_battery ? "true" : "false",
        uptime_sec, last_activity_sec_ago, any_lock ? "true" : "false");
    return (n < 0 || (size_t)n >= buf_len) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t wr_power_mgr_get_locks_json(wr_power_mgr_handle_t handle,
                                       char *buf, size_t buf_len)
{
    if (!handle || !buf) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    int64_t now = esp_timer_get_time();
    size_t off = 0;
    int n = snprintf(buf + off, buf_len - off, "{\"locks\":[");
    if (n < 0 || (size_t)n >= buf_len - off) { xSemaphoreGive(handle->mutex); return ESP_ERR_INVALID_SIZE; }
    off += (size_t)n;
    bool first = true;
    for (int i = 0; i < WR_POWER_MAX_LOCKS; i++) {
        wr_power_lock_t *l = &handle->locks[i];
        if (!l->active) continue;
        if (l->expires_at_us == 0) {
            n = snprintf(buf + off, buf_len - off,
                          "%s{\"lock_id\":%lu,\"reason\":\"%s\",\"expires_in_sec\":null}",
                          first ? "" : ",", (unsigned long)l->lock_id, l->reason);
        } else {
            long expires_in = (long)((l->expires_at_us - now) / 1000000);
            if (expires_in < 0) expires_in = 0;
            n = snprintf(buf + off, buf_len - off,
                          "%s{\"lock_id\":%lu,\"reason\":\"%s\",\"expires_in_sec\":%ld}",
                          first ? "" : ",", (unsigned long)l->lock_id, l->reason, expires_in);
        }
        if (n < 0 || (size_t)n >= buf_len - off) { xSemaphoreGive(handle->mutex); return ESP_ERR_INVALID_SIZE; }
        off += (size_t)n;
        first = false;
    }
    n = snprintf(buf + off, buf_len - off, "]}");
    xSemaphoreGive(handle->mutex);
    return (n < 0 || (size_t)n >= buf_len - off) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

uint16_t wr_power_mgr_get_telemetry_interval_sec(wr_power_mgr_handle_t handle)
{
    if (!handle) return 1;
    switch (wr_power_mgr_get_mode(handle)) {
    case WR_POWER_MODE_IDLE:      return handle->config.telemetry_interval_idle_sec;
    case WR_POWER_MODE_LOW_POWER: return handle->config.telemetry_interval_low_power_sec;
    default:                      return handle->config.telemetry_interval_active_sec;
    }
}
