# Wave Rover Power Optimization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
>
> **Companion design doc:** `docs/superpowers/specs/2026-06-12-wave-rover-power-optimization-design.md` — read it first. It explains why `DEEP_SLEEP`, automatic light sleep, camera management, IMU wake-on-motion, and battery-percent estimation are **deferred/dropped** and why "sleep locks" are reframed as **mode-floor locks**.

**Goal:** Add a centralized `wave_rover_power_mgr` component implementing `ACTIVE`/`IDLE`/`LOW_POWER` power modes for the wave_rover firmware, with Wi-Fi power-save, CPU dynamic frequency scaling, magnetometer/display/sensor-poll adjustments, NVS config, new `rover.power_*` MCP tools, Web UI section, `/metrics` additions, and structured logs — without breaking existing motor/MCP/Web/OTA functionality.

**Architecture:** A new ESP-IDF C component (`wave_rover_power_mgr`, opaque-handle pattern per CLAUDE.md) owns a 1 Hz evaluation timer that derives the target power mode from: rover busy-state (`wr_rover_state_get()`), activity-notify timestamps, battery voltage, and mode-floor locks. On transition it applies Wi-Fi power-save, CPU DFS, magnetometer mode, and display power, then logs `power: mode changed X -> Y, reason=...` (forwarded to Loki via existing syslog) and invokes a callback. `app_main.c` creates/starts it; MCP tools, the web UI, and `/metrics` read its status via JSON helpers and call `notify_activity`/`set_mode`/lock APIs.

**Tech Stack:** ESP-IDF (PlatformIO build, `pio run` from `application/wave_rover/`), FreeRTOS timers, `esp_wifi`, `esp_pm`, existing `wave_rover_hal` / `wave_rover_config` / `wave_rover_mcp` components.

---

## Task 0: Enable OTA app-rollback safety net

**Why first / out of band:** This plan's riskiest change (Task 3/5: `CONFIG_PM_ENABLE` + `esp_pm_configure` running at boot, before the web server with `/update` comes up) will be validated **over OTA, not USB** (per discussion — disassembling the rover for USB access is impractical; USB is the last-resort fallback). ESP-IDF's app-rollback feature is the standard mitigation: if the newly-flashed firmware never confirms itself as valid (e.g. it crash-loops before `wave_rover_mcp_start()` succeeds), the bootloader automatically reverts to the previous working partition on the *next* boot — restoring OTA access without any manual intervention. Currently `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` and `CONFIG_APP_ROLLBACK_ENABLE` are both unset in `sdkconfig.wave_rover`.

**Files:**
- Modify: `application/wave_rover/sdkconfig.wave_rover`
- Modify: `application/wave_rover/main/app_main.c`
- Modify: `application/wave_rover/main/CMakeLists.txt`

- [ ] **Step 1: Enable rollback in sdkconfig**

In `sdkconfig.wave_rover`, change:
```
# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not set
```
to:
```
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
```
and:
```
# CONFIG_APP_ROLLBACK_ENABLE is not set
```
to:
```
CONFIG_APP_ROLLBACK_ENABLE=y
```

With rollback enabled, a freshly-flashed OTA image boots into state `ESP_OTA_IMG_PENDING_VERIFY`. If it reboots (crash, watchdog, panic) **without** being marked valid, the bootloader marks it `ESP_OTA_IMG_ABORTED` and boots the previous partition instead.

- [ ] **Step 2: Mark the running image valid after a successful boot**

Add `#include "esp_ota_ops.h"` to `app_main.c`. After `wave_rover_mcp_start(&s_cfg, power_mgr)` succeeds (end of `app_main`, after the existing `ESP_LOGI(TAG, "boot complete...")` line) — i.e. once Wi-Fi is up, the power manager started without crashing, and the MCP/web server is listening — add:

```c
    esp_ota_img_states_t ota_state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "OTA image marked valid");
    }
```

This must run on every boot path that reaches "the device is fully up and reachable", not just immediately after an OTA — it's a no-op (returns `ESP_ERR_NOT_SUPPORTED`/`ESP_OK` depending on state) for a normally-booted, already-confirmed image, so placing it unconditionally at the end of `app_main` is correct and simplest.

> **Important corollary for Tasks 1-10:** because this task is implemented *first* and committed/flashed-and-confirmed on its own, every subsequent OTA in this plan starts from a build that already has rollback wired up — so each later task's "flash via OTA" gets the safety net automatically. If a later task's firmware crash-loops before reaching this `esp_ota_mark_app_valid_cancel_rollback()` call, the device will self-revert to the last-confirmed-good build on its own within one reboot cycle (a few seconds), restoring OTA access without disassembly.

- [ ] **Step 3: Add `app_update` to `main`'s component requirements**

In `main/CMakeLists.txt`, add `app_update` to `PRIV_REQUIRES` (it's already used by `wave_rover_mcp` for `esp_ota_set_boot_partition`, but `main` needs its own dependency for `esp_ota_ops.h`).

- [ ] **Step 4: Build check**

```bash
cd application/wave_rover && pio run
```

- [ ] **Step 5: Flash this task alone via OTA and confirm self-validation**

Flash this build via the existing `/update` OTA flow (this is the *last* time in the project's history that OTA doesn't yet have the rollback net — treat it with extra care, e.g. have the USB cable accessible just for this one flash if at all convenient). After reboot, confirm via Loki (`wr_syslog` forwarding) or `curl http://wave-rover.local/status` that the device is reachable, then re-flash trivially (or just wait) and confirm no rollback occurred. From here on, every OTA in Tasks 1-10 is protected.

- [ ] **Step 6: Commit**

```bash
git add application/wave_rover/sdkconfig.wave_rover application/wave_rover/main
git commit -m "feat(wave_rover): enable OTA app-rollback as a safety net for in-place OTA iteration"
```

---

## Task 1: Config — add power fields to `wave_rover_config_t`

**Files:**
- Modify: `application/wave_rover/components/wave_rover_config/include/wave_rover_config.h`
- Modify: `application/wave_rover/components/wave_rover_config/wave_rover_config.c`

- [ ] **Step 1: Add fields to the struct**

In `wave_rover_config.h`, append to `wave_rover_config_t` (after `syslog_facility`, before the closing `}`):

```c
    /* Power management (Wave Rover power optimization) */
    bool     power_mgr_enabled;            /* default true */
    uint16_t power_active_timeout_sec;     /* ACTIVE -> IDLE after this many idle seconds */
    uint16_t power_idle_to_low_power_sec;  /* total idle seconds before LOW_POWER */
    bool     power_wifi_power_save;        /* enable esp_wifi_set_ps() per mode */
    bool     power_reduce_cpu_frequency;   /* enable esp_pm DFS per mode */
    bool     power_disable_display_idle;   /* turn SSD1306 off in LOW_POWER */
    float    power_critical_battery_v;     /* forces LOW_POWER + motor stop below this */
    uint16_t power_telemetry_active_sec;   /* INA219 poll interval in ACTIVE */
    uint16_t power_telemetry_idle_sec;     /* INA219 poll interval in IDLE */
    uint16_t power_telemetry_low_power_sec;/* INA219 poll interval in LOW_POWER */
```

- [ ] **Step 2: Add defaults**

In `wave_rover_config.c`, in `wave_rover_config_defaults()`, append before the closing `}`:

```c
    cfg->power_mgr_enabled             = true;
    cfg->power_active_timeout_sec      = 60;
    cfg->power_idle_to_low_power_sec   = 300;
    cfg->power_wifi_power_save         = true;
    cfg->power_reduce_cpu_frequency    = true;
    cfg->power_disable_display_idle    = true;
    cfg->power_critical_battery_v      = 9.6f;
    cfg->power_telemetry_active_sec    = 5;
    cfg->power_telemetry_idle_sec      = 30;
    cfg->power_telemetry_low_power_sec = 120;
```

- [ ] **Step 3: Build check**

```bash
cd application/wave_rover && pio run
```
Expected: builds successfully (struct grows, blob load/save already tolerates growth — same pattern used when `syslog_*` fields were added).

- [ ] **Step 4: Commit**

```bash
git add application/wave_rover/components/wave_rover_config
git commit -m "feat(wave_rover): add power-management config fields"
```

---

## Task 2: New component `wave_rover_power_mgr` — core mode state machine

**Files:**
- Create: `application/wave_rover/components/wave_rover_power_mgr/CMakeLists.txt`
- Create: `application/wave_rover/components/wave_rover_power_mgr/include/wave_rover_power_mgr.h`
- Create: `application/wave_rover/components/wave_rover_power_mgr/wave_rover_power_mgr.c`

This task implements the mode state machine, activity tracking, and mode-floor locks — **without** the Wi-Fi/CPU/IMU/display side effects (those are Task 3). `apply_mode()` in this task only logs the transition. This keeps the task reviewable and independently testable.

- [ ] **Step 1: Create the component directory and CMakeLists**

```bash
mkdir -p application/wave_rover/components/wave_rover_power_mgr/include
```

`application/wave_rover/components/wave_rover_power_mgr/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "wave_rover_power_mgr.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        wave_rover_hal
        wave_rover_mcp
        esp_timer
        esp_wifi
        esp_pm
        log
        esp_common
        freertos
)
```

> Note: depends on `wave_rover_mcp` for `wr_rover_state_get()` (declared in `wave_rover_mcp_state.h`). If this introduces a circular dependency at build time (`wave_rover_mcp` does not currently depend on `wave_rover_power_mgr`, so it shouldn't — but verify in Step 6), the fallback is to move `wr_rover_state_t`/`wr_rover_state_get()` into `wave_rover_hal` instead. Don't do this preemptively; only if the build fails.

- [ ] **Step 2: Write the header**

`include/wave_rover_power_mgr.h`:

```c
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

/* old_mode/new_mode/reason are valid only for the duration of the callback. */
typedef void (*wr_power_mode_cb_t)(wr_power_mode_t old_mode,
                                    wr_power_mode_t new_mode,
                                    const char *reason, void *user_ctx);

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

esp_err_t wr_power_mgr_register_cb(wr_power_mgr_handle_t handle,
                                    wr_power_mode_cb_t cb, void *user_ctx);

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
 *  "wifi_power_save":"min_modem","cpu_freq_mhz":80,"display_on":true,
 *  "uptime_sec":1234,"last_activity_sec_ago":42,"locks_active":0}
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
```

- [ ] **Step 3: Write the implementation**

`wave_rover_power_mgr.c`:

```c
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
    wr_power_mode_cb_t    cb;
    void                 *cb_ctx;
    SemaphoreHandle_t     mutex;
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

/* Side effects applied on transition. Task 3 fills this in; for now it
 * only logs. Always succeeds — individual peripheral failures inside it
 * (added in Task 3) are logged but never block the transition. */
static void apply_mode(struct wr_power_mgr_t *m, wr_power_mode_t mode)
{
    (void)m;
    (void)mode;
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
    if (m->cb) m->cb(old, new_mode, reason, m->cb_ctx);
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

        bool rover_busy = wr_rover_state_get() != WR_ROVER_STATE_IDLE;
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
    *ret_handle = m;
    return ESP_OK;
}

esp_err_t wr_power_mgr_delete(wr_power_mgr_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    handle->running = false;
    if (handle->mutex) vSemaphoreDelete(handle->mutex);
    free(handle);
    return ESP_OK;
}

esp_err_t wr_power_mgr_start(wr_power_mgr_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (handle->running) return ESP_OK;
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

esp_err_t wr_power_mgr_register_cb(wr_power_mgr_handle_t handle,
                                    wr_power_mode_cb_t cb, void *user_ctx)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    handle->cb = cb;
    handle->cb_ctx = user_ctx;
    xSemaphoreGive(handle->mutex);
    return ESP_OK;
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
        ESP_LOGI(TAG, "power: lock acquired, reason=%s, ttl=%u", reason, ttl_sec);
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
        long expires_in = l->expires_at_us == 0 ? -1 : (long)((l->expires_at_us - now) / 1000000);
        n = snprintf(buf + off, buf_len - off, "%s{\"lock_id\":%u,\"reason\":\"%s\",\"expires_in_sec\":%s%ld%s}",
                     first ? "" : ",", l->lock_id, l->reason,
                     expires_in < 0 ? "" : "", expires_in < 0 ? 0 : expires_in,
                     expires_in < 0 ? "" : "");
        /* Render null for permanent locks without printing a 0 placeholder: */
        if (expires_in < 0) {
            n = snprintf(buf + off, buf_len - off, "%s{\"lock_id\":%u,\"reason\":\"%s\",\"expires_in_sec\":null}",
                         first ? "" : ",", l->lock_id, l->reason);
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
```

> Implementer note: the `expires_in_sec` formatting in `wr_power_mgr_get_locks_json` is written in two passes to keep "null" vs. integer rendering simple with `snprintf`. Simplify if you find a cleaner one-pass formatting — the JSON output (`{"lock_id":N,"reason":"...","expires_in_sec":null|<int>}`) is what matters, not this exact code shape.

- [ ] **Step 4: Register the new component with the build**

Check `application/wave_rover/CMakeLists.txt` (or `main/CMakeLists.txt`) for how `wave_rover_mcp` / `wave_rover_hal` are referenced as component dirs (likely an `EXTRA_COMPONENT_DIRS` or automatic `components/` discovery). New ESP-IDF components under `components/` are auto-discovered — confirm no explicit list needs updating.

- [ ] **Step 5: Build check**

```bash
cd application/wave_rover && pio run
```
Expected: new component compiles (it's not referenced by `main` yet, so it won't link into the final binary — that's fine for this task; ESP-IDF still compiles all discovered components). If the build system errors with "component not used" or similar, that's unexpected — investigate rather than working around it.

- [ ] **Step 6: Commit**

```bash
git add application/wave_rover/components/wave_rover_power_mgr
git commit -m "feat(wave_rover): add power_mgr component with mode state machine"
```

---

## Task 3: Wi-Fi power-save and CPU DFS side effects

**Files:**
- Modify: `application/wave_rover/components/wave_rover_power_mgr/wave_rover_power_mgr.c`
- Modify: `application/wave_rover/sdkconfig.wave_rover` (enable `CONFIG_PM_ENABLE`)

- [ ] **Step 1: Enable `esp_pm` in sdkconfig**

In `sdkconfig.wave_rover`, change:
```
# CONFIG_PM_ENABLE is not set
```
to:
```
CONFIG_PM_ENABLE=y
```
Leave `CONFIG_FREERTOS_USE_TICKLESS_IDLE` and any `light_sleep`-related options at their current (disabled) settings — v1 deliberately excludes automatic light sleep (see design doc §1, §2.1).

- [ ] **Step 2: Implement `apply_mode` side effects**

In `wave_rover_power_mgr.c`, add includes:
```c
#include "esp_wifi.h"
#include "esp_pm.h"
```

Replace the Task-2 stub `apply_mode()` body with:

```c
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
        esp_err_t err = esp_wifi_set_ps(ps);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "power: esp_wifi_set_ps(%d) failed: %s", ps, esp_err_to_name(err));
        }
    }

    if (m->config.reduce_cpu_frequency) {
        int min_mhz = (mode == WR_POWER_MODE_ACTIVE) ? 240 : 80;
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
}
```

- [ ] **Step 3: Build check**

```bash
cd application/wave_rover && pio run
```
Expected: builds successfully. `esp_pm.h` / `esp_wifi.h` come from `esp_pm` / `esp_wifi` components already in `PRIV_REQUIRES` (Task 2 CMakeLists).

> **OTA-safety note:** `CONFIG_PM_ENABLE=y` has no runtime effect yet — `wave_rover_power_mgr` isn't created/started until Task 5. Task 5 is where this setting first runs on real hardware, **before** the web server (and `/update`) comes up in `app_main`. See Task 5 Step 9 for the required USB-flash verification before relying on OTA again.

- [ ] **Step 4: Commit**

```bash
git add application/wave_rover/components/wave_rover_power_mgr application/wave_rover/sdkconfig.wave_rover
git commit -m "feat(wave_rover): apply Wi-Fi power-save and CPU DFS per power mode"
```

---

## Task 4: Magnetometer and display power side effects

**Files:**
- Modify: `application/wave_rover/components/wave_rover_hal/include/wave_rover_hal.h`
- Modify: `application/wave_rover/components/wave_rover_hal/wave_rover_imu.c`
- Modify: `application/wave_rover/components/wave_rover_hal/wave_rover_display.c`
- Modify: `application/wave_rover/components/wave_rover_power_mgr/wave_rover_power_mgr.c`

- [ ] **Step 1: Add `wr_imu_set_mag_continuous` to the HAL**

In `wave_rover_hal.h`, add near the other IMU declarations:
```c
/* Switches AK09918 magnetometer between continuous 10Hz mode (true) and
 * power-down mode (false). No-op (returns ESP_OK) if magnetometer not
 * present. */
esp_err_t wr_imu_set_mag_continuous(bool enable);
```

In `wave_rover_imu.c`: find the existing `ak_write(AK_CNTL2, 0x08)` call at init (around line 84) and the `AK_CNTL2` register definition. Add:
```c
esp_err_t wr_imu_set_mag_continuous(bool enable)
{
    if (!s_mag_present) return ESP_OK;   /* match the existing presence-flag name used in this file */
    /* AK09918: 0x08 = continuous measurement mode 2 (10Hz), 0x00 = power-down */
    return ak_write(AK_CNTL2, enable ? 0x08 : 0x00);
}
```
Check the actual name of the "mag present" static flag in `wave_rover_imu.c` (it feeds `wr_imu_sample_t.mag_present`) and use that — don't introduce a second flag.

- [ ] **Step 2: Add `wr_display_set_power` to the HAL**

In `wave_rover_hal.h`:
```c
/* SSD1306 display on/off via command 0xAF/0xAE. Framebuffer content is
 * preserved and reappears when re-enabled with `on=true`. */
esp_err_t wr_display_set_power(bool on);
```

In `wave_rover_display.c`, using the existing `ssd_cmd()` helper and the `0xAE`/`0xAF` constants already present (display init sequence):
```c
esp_err_t wr_display_set_power(bool on)
{
    return ssd_cmd(on ? 0xAF : 0xAE);
}
```

- [ ] **Step 3: Wire into `apply_mode`**

In `wave_rover_power_mgr.c`'s `apply_mode()`, add:
```c
    wr_imu_set_mag_continuous(mode == WR_POWER_MODE_ACTIVE);

    if (m->config.disable_display_when_idle) {
        wr_display_set_power(mode != WR_POWER_MODE_LOW_POWER);
    }
```
(Errors from these two calls are intentionally not checked individually beyond what the HAL already logs internally — keep `apply_mode` simple; if either HAL function doesn't already `ESP_LOGW` on I2C failure, add that inside the HAL function itself, not here.)

- [ ] **Step 4: Build check**

```bash
cd application/wave_rover && pio run
```

- [ ] **Step 5: Commit**

```bash
git add application/wave_rover/components/wave_rover_hal application/wave_rover/components/wave_rover_power_mgr
git commit -m "feat(wave_rover): power-manage AK09918 magnetometer and SSD1306 display"
```

---

## Task 5: Wire `wave_rover_power_mgr` into `app_main` and activity hooks

**Files:**
- Modify: `application/wave_rover/main/app_main.c`
- Modify: `application/wave_rover/components/wave_rover_mcp/include/wave_rover_mcp.h`
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp.c`
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_tools.c`
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_web.c`
- Modify: `application/wave_rover/components/wave_rover_mcp/CMakeLists.txt`

- [ ] **Step 1: Create and start the manager in `app_main.c`**

Add include `"wave_rover_power_mgr.h"`. After `wr_wifi_init(&s_cfg)` (around line 40) and before `wave_rover_mcp_start(&s_cfg)`:

```c
    wr_power_mgr_handle_t power_mgr = NULL;
    wr_power_mgr_config_t pm_cfg = {
        .enabled                          = s_cfg.power_mgr_enabled,
        .active_timeout_sec               = s_cfg.power_active_timeout_sec,
        .idle_to_low_power_sec            = s_cfg.power_idle_to_low_power_sec,
        .wifi_power_save                  = s_cfg.power_wifi_power_save,
        .reduce_cpu_frequency              = s_cfg.power_reduce_cpu_frequency,
        .disable_display_when_idle        = s_cfg.power_disable_display_idle,
        .critical_battery_voltage         = s_cfg.power_critical_battery_v,
        .telemetry_interval_active_sec    = s_cfg.power_telemetry_active_sec,
        .telemetry_interval_idle_sec      = s_cfg.power_telemetry_idle_sec,
        .telemetry_interval_low_power_sec = s_cfg.power_telemetry_low_power_sec,
    };
    ESP_ERROR_CHECK(wr_power_mgr_create(&pm_cfg, &power_mgr));
    ESP_ERROR_CHECK(wr_power_mgr_start(power_mgr));
```

- [ ] **Step 2: Thread the handle into `wave_rover_mcp_start`**

In `wave_rover_mcp.h`, change:
```c
esp_err_t wave_rover_mcp_start(const wave_rover_config_t *cfg);
```
to:
```c
esp_err_t wave_rover_mcp_start(const wave_rover_config_t *cfg, wr_power_mgr_handle_t power_mgr);
```
(add `#include "wave_rover_power_mgr.h"` to this header)

In `app_main.c`, update the call: `wave_rover_mcp_start(&s_cfg, power_mgr)`.

In `wave_rover_mcp.c`, `wave_rover_mcp_start()`: store the handle in a module-static, mirroring the existing `s_cfg` pattern (line ~39, ~388):
```c
static wr_power_mgr_handle_t s_power_mgr = NULL;
...
s_power_mgr = power_mgr;
```
Pass it on to the tools/web/metrics modules via setter functions (Step 3-5), following the existing `wr_mcp_tools_set_config(cfg)` pattern (`wave_rover_mcp_tools.c:25`).

- [ ] **Step 3: Add setters and activity-notify calls in `wave_rover_mcp_tools.c`**

Add near `wr_mcp_tools_set_config`:
```c
static wr_power_mgr_handle_t s_power_mgr = NULL;
void wr_mcp_tools_set_power_mgr(wr_power_mgr_handle_t pm) { s_power_mgr = pm; }
```
(declare both in a shared private header or directly in `wave_rover_mcp.c` if that's where the setter is called — follow whatever pattern `wr_mcp_tools_set_config` already uses for visibility)

At the top of each motor/nav/display tool handler that represents user-driven activity — `tool_move`, `tool_drive_tank`, `tool_turn`, `tool_stop`, `tool_rotate_deg` (nav), `tool_drive_cm` (nav), `tool_nav_to`, and the `display_*` tools — add:
```c
    if (s_power_mgr) wr_power_mgr_notify_activity(s_power_mgr, "mcp_tool");
```
`tool_get_status`/`tool_get_config`/`tool_get_power`/`tool_get_ups`/`tool_get_imu` etc. (read-only queries) do **not** call this — querying status shouldn't keep the rover ACTIVE forever.

- [ ] **Step 4: Add setter and activity-notify in `wave_rover_mcp_web.c`**

Same setter pattern: `wr_mcp_web_set_power_mgr(wr_power_mgr_handle_t pm)`.

In `handle_cmd` (the `/cmd` motor-control endpoint) and `handle_settings_post`, add the same `wr_power_mgr_notify_activity(s_power_mgr, "web")` call at the top of the handler (after auth check).

In `handle_update` (OTA, lines ~463-541): at the start, acquire a permanent lock:
```c
    uint32_t ota_lock_id = 0;
    if (s_power_mgr) wr_power_mgr_acquire_lock(s_power_mgr, "ota", 0, &ota_lock_id);
```
The device reboots at the end of a successful OTA, so no explicit release is needed on the success path; on early-return error paths, release it:
```c
    if (s_power_mgr && ota_lock_id) wr_power_mgr_release_lock(s_power_mgr, ota_lock_id);
```
Add this release call to every early `return` in `handle_update` before the reboot point.

- [ ] **Step 5: Update `wave_rover_mcp.c` registration call sites**

Wherever `wave_rover_mcp.c` calls `wr_mcp_tools_set_config(cfg)` / web registration (around `wave_rover_mcp_start`), add the matching `wr_mcp_tools_set_power_mgr(s_power_mgr)` / `wr_mcp_web_set_power_mgr(s_power_mgr)` calls.

- [ ] **Step 6: Update `wave_rover_mcp/CMakeLists.txt`**

Add `wave_rover_power_mgr` to `PRIV_REQUIRES`.

- [ ] **Step 7: Build check**

```bash
cd application/wave_rover && pio run
```

- [ ] **Step 8: Commit**

```bash
git add application/wave_rover/main application/wave_rover/components/wave_rover_mcp
git commit -m "feat(wave_rover): wire power_mgr into app_main, MCP tools, web, and OTA"
```

- [ ] **Step 9: Flash via OTA and verify, relying on the Task 0 rollback safety net**

This is the first task where `CONFIG_PM_ENABLE=y` (Task 3) and `wr_power_mgr_create/start()` actually run at boot, **before** `wave_rover_mcp_start()` brings up the web server that serves `/update`. If `esp_pm_configure()` or the eval task panics/hangs here, Task 0's `esp_ota_mark_app_valid_cancel_rollback()` (called only after `wave_rover_mcp_start()` succeeds) never runs, so the bootloader reverts to the previous working build on the next reboot — restoring OTA without disassembly.

Flash via the existing `/update` OTA flow, then confirm via Loki (`wr_syslog`) or `curl`:
- `curl http://wave-rover.local/status` and `curl http://wave-rover.local/metrics` respond normally (device is up on the new image, not rolled back).
- Loki shows `"OTA image marked valid"` (Task 0 Step 2) for this boot, and `"boot complete. MCP at http://..."`.
- No repeated boot-log cycles in Loki (which would indicate a crash-loop/rollback happened).

If the device becomes unreachable for more than ~30s after the OTA, wait for it to reboot and self-revert (Task 0), then fix the issue and re-flash via OTA. Only fall back to disassembly + USB if rollback itself is somehow ineffective (e.g. the crash happens *after* `esp_ota_mark_app_valid_cancel_rollback()` — shouldn't occur if Step 9 of Task 0 was verified, but is the documented last resort).

---

## Task 6: Mode-aware INA219 poll interval

**Files:**
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_web.c`

- [ ] **Step 1: Use `wr_power_mgr_get_telemetry_interval_sec` in `sensor_poll_task`**

Current code (`wave_rover_mcp_web.c:370-378`):
```c
static void sensor_poll_task(void *arg)
{
    for (;;) {
        wr_power_status_t ps = {0};
        wr_power_get_status(&ps);
        s_cached_bat_v = ps.load_voltage_v;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```
Change the delay to:
```c
static void sensor_poll_task(void *arg)
{
    for (;;) {
        wr_power_status_t ps = {0};
        wr_power_get_status(&ps);
        s_cached_bat_v = ps.load_voltage_v;
        uint16_t interval_sec = s_power_mgr
            ? wr_power_mgr_get_telemetry_interval_sec(s_power_mgr) : 1;
        if (interval_sec < 1) interval_sec = 1;
        vTaskDelay(pdMS_TO_TICKS(interval_sec * 1000));
    }
}
```
This relies on `s_power_mgr` from Task 5 Step 4 being set before `sensor_poll_task` starts (`xTaskCreate(sensor_poll_task, ...)` at line ~814) — confirm registration order in `wave_rover_mcp_start`, or read `s_power_mgr` lazily inside the loop (it's already a static, safe to read each iteration).

- [ ] **Step 2: Build check**

```bash
cd application/wave_rover && pio run
```

- [ ] **Step 3: Commit**

```bash
git add application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_web.c
git commit -m "feat(wave_rover): scale INA219 poll interval with power mode"
```

---

## Task 7: MCP tools `rover.power_*`

**Files:**
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_tools.c`

- [ ] **Step 1: `rover.power_get_status`**

Add a handler following the existing `tool_get_power` pattern (`wave_rover_mcp_tools.c:297`), but without arguments:
```c
static esp_mcp_value_t tool_power_get_status(const esp_mcp_property_list_t *p)
{
    char buf[256];
    if (!s_power_mgr || wr_power_mgr_get_status_json(s_power_mgr, buf, sizeof(buf)) != ESP_OK) {
        return esp_mcp_value_create_string("{\"error\":\"power manager unavailable\"}");
    }
    return esp_mcp_value_create_string(buf);
}
```
Register with `esp_mcp_register_tool` (or whatever the existing registration call is — match `tool_get_power`'s registration block exactly, including description/schema with no properties).

- [ ] **Step 2: `rover.power_set_mode`**

```c
static esp_mcp_value_t tool_power_set_mode(const esp_mcp_property_list_t *p)
{
    const char *mode_str = esp_mcp_property_list_get_property_string(p, "mode");
    const char *reason   = esp_mcp_property_list_get_property_string(p, "reason");
    wr_power_mode_t mode;
    if (mode_str && strcasecmp(mode_str, "ACTIVE") == 0)        mode = WR_POWER_MODE_ACTIVE;
    else if (mode_str && strcasecmp(mode_str, "IDLE") == 0)      mode = WR_POWER_MODE_IDLE;
    else if (mode_str && strcasecmp(mode_str, "LOW_POWER") == 0) mode = WR_POWER_MODE_LOW_POWER;
    else return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"invalid mode\"}");

    if (!s_power_mgr) return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"power manager unavailable\"}");

    esp_err_t err = wr_power_mgr_set_mode(s_power_mgr, mode, reason ? reason : "mcp");
    if (err != ESP_OK) {
        return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"rejected: rover busy or sleep lock held\"}");
    }
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"mode\":\"%s\"}", wr_power_mode_name(mode));
    return esp_mcp_value_create_string(out);
}
```
Schema: required string property `mode` (enum ACTIVE/IDLE/LOW_POWER), optional string `reason` — match the property-schema style used by e.g. `tool_stop`'s `reason` property (`wave_rover_mcp_tools.c:112`).

- [ ] **Step 3: `rover.power_configure`**

```c
static esp_mcp_value_t tool_power_configure(const esp_mcp_property_list_t *p)
{
    if (!s_cfg_mut) return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"config unavailable\"}");
    wave_rover_config_t cfg = *s_cfg_mut;

    if (esp_mcp_property_list_has_property(p, "active_timeout_sec"))
        cfg.power_active_timeout_sec = (uint16_t)esp_mcp_property_list_get_property_int(p, "active_timeout_sec");
    if (esp_mcp_property_list_has_property(p, "idle_to_low_power_sec"))
        cfg.power_idle_to_low_power_sec = (uint16_t)esp_mcp_property_list_get_property_int(p, "idle_to_low_power_sec");
    if (esp_mcp_property_list_has_property(p, "wifi_power_save"))
        cfg.power_wifi_power_save = esp_mcp_property_list_get_property_bool(p, "wifi_power_save");
    if (esp_mcp_property_list_has_property(p, "reduce_cpu_frequency"))
        cfg.power_reduce_cpu_frequency = esp_mcp_property_list_get_property_bool(p, "reduce_cpu_frequency");
    if (esp_mcp_property_list_has_property(p, "disable_display_when_idle"))
        cfg.power_disable_display_idle = esp_mcp_property_list_get_property_bool(p, "disable_display_when_idle");
    if (esp_mcp_property_list_has_property(p, "critical_battery_voltage"))
        cfg.power_critical_battery_v = (float)esp_mcp_property_list_get_property_float(p, "critical_battery_voltage");

    esp_err_t err = wave_rover_config_save(&cfg);
    if (err != ESP_OK) return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"save failed\"}");
    *s_cfg_mut = cfg;
    return esp_mcp_value_create_string("{\"ok\":true,\"note\":\"some settings take effect after reboot\"}");
}
```

> **Decision needed during implementation (not a question to the user — pick the simplest option that compiles):** `s_cfg` in `wave_rover_mcp_tools.c` is currently `const wave_rover_config_t *`. `power_configure` needs to mutate and persist it. Simplest approach: keep `s_cfg` const for reads, and have `wr_mcp_tools_set_config` also store a separate non-const pointer `s_cfg_mut` to the *same* struct that `app_main.c` owns (`&s_cfg` in `app_main.c` is already a long-lived static). Add a second setter parameter or a second static — whichever is less invasive. Live-apply of the new values (vs. "takes effect after reboot") is **not required for v1**; the `power_mgr` reads its config copy from `wr_power_mgr_create()` time only. Document this limitation in the final report.

- [ ] **Step 4: `rover.power_prevent_sleep` / `rover.power_release_sleep_lock`**

```c
static esp_mcp_value_t tool_power_prevent_sleep(const esp_mcp_property_list_t *p)
{
    const char *reason = esp_mcp_property_list_get_property_string(p, "reason");
    int ttl_sec = esp_mcp_property_list_get_property_int(p, "ttl_sec");
    if (!reason) reason = "manual_control";
    if (ttl_sec < 0) ttl_sec = 0;

    if (!s_power_mgr) return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"power manager unavailable\"}");
    uint32_t lock_id = 0;
    esp_err_t err = wr_power_mgr_acquire_lock(s_power_mgr, reason, (uint32_t)ttl_sec, &lock_id);
    if (err != ESP_OK) return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"too many locks\"}");
    char out[64];
    snprintf(out, sizeof(out), "{\"ok\":true,\"lock_id\":%u}", lock_id);
    return esp_mcp_value_create_string(out);
}

static esp_mcp_value_t tool_power_release_sleep_lock(const esp_mcp_property_list_t *p)
{
    int lock_id = esp_mcp_property_list_get_property_int(p, "lock_id");
    if (!s_power_mgr) return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"power manager unavailable\"}");
    esp_err_t err = wr_power_mgr_release_lock(s_power_mgr, (uint32_t)lock_id);
    if (err != ESP_OK) return esp_mcp_value_create_string("{\"ok\":false,\"error\":\"lock not found\"}");
    return esp_mcp_value_create_string("{\"ok\":true}");
}
```

- [ ] **Step 5: Register all five tools**

Add registration calls for `rover.power_get_status`, `rover.power_set_mode`, `rover.power_configure`, `rover.power_prevent_sleep`, `rover.power_release_sleep_lock` in `wr_mcp_register_all_tools`, matching the existing registration block format (name, description, handler, property schema) used for the other ~30 tools.

- [ ] **Step 6: Build check**

```bash
cd application/wave_rover && pio run
```

- [ ] **Step 7: Manual verification**

Flash and, via an MCP client (or the existing `Rover.*` tools already configured in this session's allowlist), call `rover.power_get_status` and confirm it returns valid JSON with `mode`, `battery_voltage`, etc.

- [ ] **Step 8: Commit**

```bash
git add application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_tools.c
git commit -m "feat(wave_rover): add rover.power_* MCP tools"
```

---

## Task 8: `/metrics` additions

**Files:**
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_metrics.c`
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_metrics.h` (if it exposes a setter)
- Modify: `application/wave_rover/components/wave_rover_mcp/CMakeLists.txt` (if not already updated in Task 5)

- [ ] **Step 1: Add a power_mgr setter**

Add `wr_mcp_metrics_set_power_mgr(wr_power_mgr_handle_t pm)` following the `s_cfg`/setter pattern already in this file (`static const wave_rover_config_t *s_cfg`).

- [ ] **Step 2: Emit the new metrics**

In the metrics-generation function, following the existing `wave_rover_state{state=...}` enum pattern (one line per possible value), add:

```c
    if (s_power_mgr) {
        wr_power_mode_t mode = wr_power_mgr_get_mode(s_power_mgr);
        off += snprintf(buf + off, size - off,
            "# TYPE wave_rover_power_mode gauge\n"
            "wave_rover_power_mode{mode=\"active\"} %d\n"
            "wave_rover_power_mode{mode=\"idle\"} %d\n"
            "wave_rover_power_mode{mode=\"low_power\"} %d\n",
            mode == WR_POWER_MODE_ACTIVE ? 1 : 0,
            mode == WR_POWER_MODE_IDLE ? 1 : 0,
            mode == WR_POWER_MODE_LOW_POWER ? 1 : 0);

        char status_json[256];
        bool locks_active = false;
        if (wr_power_mgr_get_status_json(s_power_mgr, status_json, sizeof(status_json)) == ESP_OK) {
            locks_active = strstr(status_json, "\"locks_active\":true") != NULL;
        }
        off += snprintf(buf + off, size - off,
            "# TYPE wave_rover_power_locks_active gauge\n"
            "wave_rover_power_locks_active %d\n",
            locks_active ? 1 : 0);
    }
```
Follow the existing bounded-`snprintf`-with-offset / overflow-handling convention already used in this file (don't introduce a different style). Use the existing `off`/buffer-size variable names from this file rather than `off`/`size` if they differ.

> CPU-freq and Wi-Fi-PS metrics (`wave_rover_power_cpu_freq_mhz`, `wave_rover_power_wifi_ps`) from the design doc are **deferred from this task** — reading back the *actual* applied `esp_pm`/`esp_wifi` state requires additional getters not yet defined. If straightforward (`esp_pm_get_configured_min_freq`-equivalent / a static cache in `wave_rover_power_mgr` updated in `apply_mode`), add them; otherwise note as a remaining item in the final report. The `wave_rover_power_mode` and `wave_rover_power_locks_active` metrics above are the required minimum.

- [ ] **Step 3: Build check**

```bash
cd application/wave_rover && pio run
```

- [ ] **Step 4: Manual verification**

```bash
curl http://wave-rover.local/metrics | grep wave_rover_power
```
Expected: `wave_rover_power_mode{mode="..."}` lines (one =1, others =0) and `wave_rover_power_locks_active`.

- [ ] **Step 5: Commit**

```bash
git add application/wave_rover/components/wave_rover_mcp
git commit -m "feat(wave_rover): expose power mode and lock state via /metrics"
```

---

## Task 9: Web UI "Power" section

**Files:**
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_web.c`

- [ ] **Step 1: Add `GET /power` and `POST /power` handlers**

```c
static esp_err_t handle_power_get(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    char buf[256];
    if (!s_power_mgr || wr_power_mgr_get_status_json(s_power_mgr, buf, sizeof(buf)) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "{\"error\":\"power manager unavailable\"}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_power_post(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    char body[128] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) return httpd_resp_send_500(req);

    cJSON *root = cJSON_Parse(body);
    if (!root) return httpd_resp_send_500(req);
    const cJSON *mode_j = cJSON_GetObjectItem(root, "mode");
    wr_power_mode_t mode;
    bool ok = true;
    if (cJSON_IsString(mode_j) && strcasecmp(mode_j->valuestring, "ACTIVE") == 0) mode = WR_POWER_MODE_ACTIVE;
    else if (cJSON_IsString(mode_j) && strcasecmp(mode_j->valuestring, "IDLE") == 0) mode = WR_POWER_MODE_IDLE;
    else if (cJSON_IsString(mode_j) && strcasecmp(mode_j->valuestring, "LOW_POWER") == 0) mode = WR_POWER_MODE_LOW_POWER;
    else ok = false;
    cJSON_Delete(root);

    if (!ok || !s_power_mgr) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid mode\"}", HTTPD_RESP_USE_STRLEN);
    }
    esp_err_t err = wr_power_mgr_set_mode(s_power_mgr, mode, "web_ui");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, err == ESP_OK ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rejected\"}", HTTPD_RESP_USE_STRLEN);
}
```
Register both with `httpd_register_uri_handler` alongside the other endpoints (`/status`, `/cmd`, etc.) — check the `max_uri_handlers` config (currently 16 per the metrics design doc, now at 12 after `/metrics`; +2 for `/power` brings it to 14, still under 16).

- [ ] **Step 2: Add a Power section to the HTML/JS**

In the existing single-page HTML string (`s_html`), add a `<section id="power">` with: mode display, battery voltage, last-activity, three buttons (ACTIVE/IDLE/LOW_POWER) that `POST /power {"mode":"..."}`,  and a periodic `fetch('/power')` (reuse the existing polling pattern used for `/status`) to update the display. Keep it visually consistent with the existing page's inline-CSS style — no new framework/build step (this app's web UI is a hand-written string, distinct from `edge_agent`'s `frontend_source`).

- [ ] **Step 3: Build check**

```bash
cd application/wave_rover && pio run
```

- [ ] **Step 4: Manual verification**

Flash, open `http://wave-rover.local/`, confirm the Power section renders, shows the current mode, and that clicking "IDLE"/"LOW_POWER" while the rover is stationary changes the displayed mode (and is rejected — `ok:false` — while driving).

- [ ] **Step 5: Commit**

```bash
git add application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_web.c
git commit -m "feat(wave_rover): add Power section to web UI"
```

---

## Task 10: Documentation — power optimization test plan

**Files:**
- Modify: `docs/wave_rover_user_guide.md`

- [ ] **Step 1: Add a "Power optimization" section**

Document:
- The three modes (ACTIVE/IDLE/LOW_POWER), their entry conditions and effects (table from design doc §2.1).
- The new `power_*` config fields (§2.4) and the `rover.power_*` MCP tools (§2.7), plus `GET/POST /power`.
- `DEEP_SLEEP` deferral rationale (one paragraph, from design doc §1).
- A test plan table adapted from the prompt's 10 scenarios, dropping the camera/video scenario (#6) and the deep-sleep-recovery framing of #10 (since LOW_POWER→ACTIVE recovery is the realistic equivalent):

  | # | Scenario | Expected mode | Expect ON | Expect OFF | Expected log line |
  |---|---|---|---|---|---|
  | 1 | Boot → Wi-Fi connect → no activity | ACTIVE → IDLE after `active_timeout_sec` | Wi-Fi, MCP, Web, display | — | `power: mode changed ACTIVE -> IDLE, reason=active_timeout` |
  | 2 | Drive via MCP/web for 60s | ACTIVE throughout | motors, full CPU, Wi-Fi PS off | — | (no transition while driving) |
  | 3 | Idle 5+ minutes, no battery issue | LOW_POWER after `idle_to_low_power_sec` | Wi-Fi (max PS), MCP, Web | display, mag continuous | `power: mode changed IDLE -> LOW_POWER, reason=idle_timeout` |
  | 4 | Web UI open, polling `/status`, no motion | stays ACTIVE while polling triggers `notify_activity`, else IDLE | — | — | depends on whether `/status` itself calls notify_activity (it should **not**, per Task 5 — only `/cmd`/`/settings` do) |
  | 5 | MCP client connected, only read-only tool calls | IDLE/LOW_POWER per timers (reads don't reset activity) | — | — | — |
  | 6 | Wi-Fi connection lost | mode logic continues independent of Wi-Fi state (no special-case) | — | — | existing Wi-Fi reconnect logging |
  | 7 | Battery voltage < `power_critical_battery_v` | forced LOW_POWER, motors stopped | — | motors | `power: critical battery, voltage=X.XX` |
  | 8 | OTA update in progress | pinned ACTIVE via `ota` lock | Wi-Fi full power, full CPU | — | `power: lock acquired, reason=ota, ttl=0` |
  | 9 | LOW_POWER → drive command received | immediate ACTIVE | motors, full CPU, Wi-Fi PS off | — | `power: mode changed LOW_POWER -> ACTIVE, reason=rover_busy` (or `mcp_tool`/`web`) |

- [ ] **Step 2: Manual current-measurement table (USB power meter / inline ammeter)**

```text
Scenario | Expected mode | Current before | Current after | Notes
Boot     | ACTIVE        |                |               |
Idle     | IDLE          |                |               |
LowPower | LOW_POWER     |                |               |
Driving  | ACTIVE        |                |               |
```

- [ ] **Step 3: Commit**

```bash
git add docs/wave_rover_user_guide.md
git commit -m "docs(wave_rover): document power modes and power optimization test plan"
```

---

## Final Report (after all tasks)

When all tasks are complete, produce a final report (per the original prompt's "Формат работы" section) covering:
- What was found during the audit (design doc §1's per-section verdicts).
- What was changed (Tasks 1-10).
- Implemented modes: ACTIVE, IDLE, LOW_POWER. `DEEP_SLEEP` deferred — reason: Wi-Fi/MCP/Web-server availability requirements conflict with deep-sleep wake latency and reassociation cost (design doc §1).
- New config parameters (Task 1 list).
- New API/MCP tools (Task 7), `/metrics` additions (Task 8), Web UI section (Task 9).
- Remaining risks: `esp_pm` DFS interaction with Wi-Fi/I2C under real load (verify empirically — Task 3's "no light sleep" choice mitigates but doesn't eliminate this); `power_configure`'s "takes effect after reboot" limitation (Task 7 Step 3); battery thresholds are voltage-based, not percent (design doc §1).
- How to measure the effect: Task 10's test plan + manual current-measurement table; `/metrics`'s `wave_rover_power_*` series for Grafana-side before/after comparison using the existing "Wave Rover" dashboard.
