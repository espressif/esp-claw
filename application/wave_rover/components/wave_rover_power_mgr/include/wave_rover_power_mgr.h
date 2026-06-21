/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WR_POWER_MODE_ACTIVE = 0,
    WR_POWER_MODE_IDLE,
    WR_POWER_MODE_LOW_POWER,
} wr_power_mode_t;

/* Returns "active" / "idle" / "low_power" — lower-case, used in JSON and
 * Prometheus label values. */
const char *wr_power_mode_name(wr_power_mode_t mode);

typedef struct {
    bool     enabled;
    uint16_t active_timeout_sec;
    uint16_t idle_to_low_power_sec;
    bool     wifi_power_save;
    bool     reduce_cpu_frequency;
    bool     disable_display_when_idle;
    float    critical_battery_voltage;
    uint16_t telemetry_interval_active_sec;
    uint16_t telemetry_interval_idle_sec;
    uint16_t telemetry_interval_low_power_sec;
} wr_power_mgr_config_t;

typedef struct wr_power_mgr_t *wr_power_mgr_handle_t;

esp_err_t wr_power_mgr_create(const wr_power_mgr_config_t *config,
                               wr_power_mgr_handle_t *ret_handle);
esp_err_t wr_power_mgr_delete(wr_power_mgr_handle_t handle);

/* Starts the 1Hz evaluation timer. */
esp_err_t wr_power_mgr_start(wr_power_mgr_handle_t handle);

/* Called on user-driven activity (motor/nav/display/web/MCP). Resets the
 * idle timer and, if not already, transitions to ACTIVE. */
void wr_power_mgr_notify_activity(wr_power_mgr_handle_t handle, const char *source);

/* Explicit mode change (MCP power_set_mode / Web UI).
 * Returns ESP_ERR_INVALID_STATE if:
 *  - target < current required floor due to a held lock, or
 *  - target != ACTIVE while wr_rover_state_get() != WR_ROVER_STATE_IDLE. */
esp_err_t wr_power_mgr_set_mode(wr_power_mgr_handle_t handle,
                                 wr_power_mode_t mode, const char *reason);

wr_power_mode_t wr_power_mgr_get_mode(wr_power_mgr_handle_t handle);

/* Mode-floor lock: while >=1 lock is held, mode cannot drop below ACTIVE.
 * ttl_sec == 0 means "until explicitly released" (used by OTA).
 * Up to 4 concurrent locks; returns ESP_ERR_NO_MEM if full. */
esp_err_t wr_power_mgr_acquire_lock(wr_power_mgr_handle_t handle,
                                     const char *reason, uint32_t ttl_sec,
                                     uint32_t *ret_lock_id);
/* ESP_ERR_NOT_FOUND if lock_id doesn't exist (already expired/released). */
esp_err_t wr_power_mgr_release_lock(wr_power_mgr_handle_t handle, uint32_t lock_id);

/* Writes a JSON object (no trailing newline) into buf, e.g.:
 * {"mode":"idle","battery_voltage":11.80,"low_battery":false,
 *  "uptime_sec":1234,"last_activity_sec_ago":42,"locks_active":false}
 * Returns ESP_ERR_INVALID_SIZE if buf is too small (output untouched). */
esp_err_t wr_power_mgr_get_status_json(wr_power_mgr_handle_t handle,
                                        char *buf, size_t buf_len);

/* Writes {"locks":[{"lock_id":1,"reason":"ota","expires_in_sec":null}, ...]} */
esp_err_t wr_power_mgr_get_locks_json(wr_power_mgr_handle_t handle,
                                       char *buf, size_t buf_len);

/* Current INA219 poll interval for the active mode, per config. Used by
 * wave_rover_mcp_web's sensor_poll_task (Task 5). */
uint16_t wr_power_mgr_get_telemetry_interval_sec(wr_power_mgr_handle_t handle);

#ifdef __cplusplus
}
#endif
