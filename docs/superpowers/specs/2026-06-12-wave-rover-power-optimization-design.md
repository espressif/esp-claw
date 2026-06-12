# Wave Rover Power Optimization — Critical Assessment & Design

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement the companion plan task-by-task.

This document evaluates `docs/rover_power_optimization_agent_prompt.md` against the actual `application/wave_rover` codebase, then defines a scoped design for the parts that are worth doing.

---

## 1. Critical assessment of the prompt

The prompt is a **generic, hardware-agnostic template** written for "an ESP32 rover" in general (Arduino/ESP32 style, optional camera, optional UPS, optional display). Large parts of it do not match this project. Below is a section-by-section verdict.

| Prompt section | Verdict | Why |
|---|---|---|
| `PowerManager` C++ class (`String`, `src/power/*.cpp`, `enum class PowerMode`) | **Reject the interface, keep the idea** | This repo is ESP-IDF C, not Arduino/C++. CLAUDE.md mandates opaque-handle objects (`wr_power_mgr_handle_t`, `wr_power_mgr_create/delete/start`, `esp_err_t`, callbacks via `_register_cb`). `String` and `.cpp` files don't exist anywhere in `application/wave_rover`. The prompt itself says "don't copy this interface blindly" — taking that at face value. |
| Camera section (entire) | **Drop — not applicable** | `Explore` confirmed there is no camera driver, no stream endpoint, and no camera MCP tool in wave_rover. The "camera" reference in the prompt's project context is explicitly listed as "возможно" (maybe present). It isn't. Including a camera config/telemetry surface for hardware that doesn't exist would violate the "don't add speculative abstractions" rule in CLAUDE.md. |
| Display: "no continuous redraw", "dirty updates", "backlight off" | **Mostly already true / partially inapplicable** | `wave_rover_display.c` already does on-demand SSD1306 updates only — called once at boot and via explicit MCP tools (`rover.display_text/clear/status`). There is **no periodic redraw loop to fix**. The SSD1306 is OLED with no separate backlight, so "gasить подсветку" (dim backlight) doesn't map to real hardware. What *does* map: SSD1306 supports a display-off command (0xAE) that cuts most of its current draw — that's a real, small win for LOW_POWER. |
| IMU section | **Partially valid** | `wave_rover_imu.c` reads accel/gyro on-demand (good, nothing to fix). But the AK09918 magnetometer is initialized into **continuous 10 Hz mode at boot and left running forever** (`ak_write(AK_CNTL2, 0x08)`), regardless of whether anything ever reads it. This is a genuine, currently-undocumented background power draw — a real target. "Wake-on-motion" interrupts: the QMI8658 supports this, but wiring an interrupt GPIO + ISR is a much larger change than this project's current scope; not pursued in v1. |
| UPS/battery section | **Valid, but no percent model exists** | `wr_power_get_status()` already returns `low_battery` (bool, `load_voltage_v < 10.5 V` for a 3S LiPo, from `board_config.h`). There is **no battery-percent estimation** anywhere — the prompt's `low_battery_threshold_percent`/`critical_battery_threshold_percent` (in %) don't have a data source. Rather than inventing a voltage→percent curve (speculative, hard to calibrate without real cell data), v1 uses **voltage thresholds** directly: the existing 10.5 V `low_battery` flag drives LOW_POWER, and a new lower `critical_battery_voltage` (default 9.6 V ≈ 3.2 V/cell) drives a "critical" alarm. |
| Wi-Fi power-save | **Valid, real gap** | Confirmed: `esp_wifi_set_ps()` is called nowhere in `wr_wifi.c`. STA mode currently runs at full power (`WIFI_PS_NONE`, the IDF default) continuously. This is the single highest-value, lowest-risk change in the whole prompt. |
| CPU frequency scaling | **Valid but higher-risk; scope to DFS only** | `CONFIG_PM_ENABLE` is unset; CPU is fixed at 240 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`). Enabling `esp_pm` with dynamic frequency scaling (240↔80 MHz, no automatic light sleep) is moderate-risk: on ESP32-S3, APB clock stays at the configured rate as long as light sleep isn't entered, so LEDC (motor PWM) and I2C (display/IMU/INA219) timing are not affected by CPU-only DFS. **Automatic light sleep** (`CONFIG_FREERTOS_USE_TICKLESS_IDLE` + `esp_pm_config.light_sleep_enable`) is excluded from v1 — it can disrupt Wi-Fi association timing and I2C transactions mid-flight, and the prompt itself treats `DEEP_SLEEP`-adjacent behavior as optional/deferrable. |
| `while(true)` / busy-loop audit | **Already clean** | `app_main`'s loop is `vTaskDelay(30000ms)` + a heap log — already idle. Motor worker (50 ms), sensor poll (1000 ms), MCP keepalive (5000 ms), web watchdog (500 ms) are all `vTaskDelay`-based, none are busy-spins. No action needed beyond *folding these into mode-aware intervals* where it's cheap (sensor poll, telemetry).
| `DEEP_SLEEP` | **Explicitly deferred** (the prompt permits this) | wave_rover is a network-controlled robot: MCP server + Web UI + mDNS all depend on Wi-Fi staying associated. Deep sleep drops the Wi-Fi connection entirely; reassociation after wake costs ~1-3 s and a new IP/mDNS announce, during which the device is uncontrollable. Wake sources (timer/GPIO) don't map to "someone wants to drive the rover again" — the only sensible wake trigger would be a GPIO button, which doesn't exist on this board. Given the DoD explicitly allows "implemented or explicitly deferred with reasons", **v1 defers `DEEP_SLEEP` entirely**. |
| Existing `wr_rover_state_t` (IDLE/DRIVING/NAV_BUSY/ESTOP) vs. new power modes | **Must be reconciled, not duplicated** | `wr_rover_state_get()` already derives "is the rover busy" from motor state + estop + nav-busy. This is *exactly* the signal a PowerManager needs to know "don't demote out of ACTIVE". The design below makes the PowerManager **consume** `wr_rover_state_get()` as its busy signal rather than re-implementing motion/activity detection. |
| MCP tools (`power.*`) | **Valid, adapt naming** | Existing tools are namespaced `rover.*` (e.g. `rover.get_status`, `rover.get_power`). New tools should follow that convention: `rover.power_get_status`, `rover.power_set_mode`, `rover.power_configure`, `rover.power_prevent_sleep`, `rover.power_release_sleep_lock` — consistent with the 30 existing tools in `wave_rover_mcp_tools.c`. |
| "Sleep locks" (`acquireSleepLock`/`prevent_sleep`) | **Reframe as mode-floor locks** | With `DEEP_SLEEP` and automatic light sleep both out of v1, there is no actual "sleep" to prevent. Reframing the same mechanism as a **mode-floor lock** — "don't demote below ACTIVE while this lock is held" — keeps the API meaningful now (OTA holds one; a manual web/MCP session can hold one) and extends naturally if real sleep is added later. |
| Telemetry / structured logs | **Valid, extend existing pipeline** | wave_rover already forwards all `ESP_LOG*` output as structured JSON via `wr_syslog` (commit `c26ab47`) and exposes Prometheus `/metrics` (the previous task). New `power: mode changed ...` lines via `ESP_LOGI` are automatically captured by both — no new telemetry transport needed. Add `wave_rover_power_mode` and `wave_rover_power_locks_active` gauges to the existing `/metrics` catalog. |
| Config format | **Adapt to existing flat NVS struct** | `wave_rover_config_t` is a flat C struct persisted via NVS (`wave_rover_config.c`). New `power_*` fields are added to this struct directly — no parallel YAML/JSON config mechanism, per the prompt's own "adapt to existing style" instruction. |
| Web UI "Power" page | **Valid**, scoped down | `wave_rover_mcp_web.c` serves a single hand-written HTML/JS page (no SPA framework, no `frontend_source` build step for this app — that's the unrelated `edge_agent` app). Add a "Power" section to the existing page: mode, battery voltage, Wi-Fi PS state, CPU freq, last-activity, mode buttons, lock list. No camera/display-activity rows (not applicable here). |
| Test plan / measurement doc | **Valid**, trim scenarios 6 (video stream) and parts of 9 (OTA + sleep interplay is now simpler since no real sleep exists) | Adapt the 10-scenario table to wave_rover's actual surface (no camera/video scenario). |

### Net scope for v1

**In scope:**
- New `wave_rover_power_mgr` component (ESP-IDF C, opaque handle): modes `ACTIVE` / `IDLE` / `LOW_POWER`, activity timers, mode-floor locks, mode-change callback.
- Reconciliation with `wr_rover_state_get()` — busy rover ⇒ forced `ACTIVE`, no demotion.
- Wi-Fi power-save (`esp_wifi_set_ps`) driven by mode.
- CPU dynamic frequency scaling (esp_pm DFS, no light sleep) driven by mode.
- AK09918 magnetometer: continuous 10 Hz only in `ACTIVE`; single-shot/disabled otherwise.
- INA219 poll interval scales with mode (`telemetry_interval_*_sec`).
- SSD1306 display off in `LOW_POWER` (via existing `wave_rover_display` driver, new "set power" API).
- NVS config additions (flat fields in `wave_rover_config_t`).
- New MCP tools: `rover.power_get_status`, `rover.power_set_mode`, `rover.power_configure`, `rover.power_prevent_sleep`, `rover.power_release_sleep_lock`.
- Web UI "Power" section.
- `/metrics` additions: `wave_rover_power_mode`, `wave_rover_power_cpu_freq_mhz`, `wave_rover_power_wifi_ps`, `wave_rover_power_locks_active`.
- Structured `power: ...` log lines (via existing syslog/Loki pipeline).
- Docs: "Power optimization test plan" section in `docs/wave_rover_user_guide.md`.

**Explicitly deferred (with rationale, per DoD):**
- `DEEP_SLEEP` (Wi-Fi/MCP/Web availability requirements conflict with deep sleep wake latency and lost reassociation).
- Automatic light sleep / tickless idle (I2C/Wi-Fi timing risk; only CPU DFS is used).
- Camera management (no camera hardware/driver exists).
- IMU wake-on-motion interrupts (requires new GPIO wiring; out of scope).
- Battery percent estimation (no calibrated voltage curve exists; v1 uses voltage thresholds).

---

## 2. Design

### 2.1 Power modes

```c
typedef enum {
    WR_POWER_MODE_ACTIVE = 0,
    WR_POWER_MODE_IDLE,
    WR_POWER_MODE_LOW_POWER,
} wr_power_mode_t;
```

| Mode | Entry condition | Wi-Fi PS | CPU freq | Magnetometer | INA219 poll | Display |
|---|---|---|---|---|---|---|
| `ACTIVE` | Boot, any activity notify, or `wr_rover_state_get() != IDLE`, or a mode-floor lock is held | `WIFI_PS_NONE` | 240 MHz | continuous 10 Hz | `telemetry_interval_active_sec` (default 5 s) | on |
| `IDLE` | No activity for `active_timeout_sec` (default 60 s) **and** rover state == IDLE **and** no lock held | `WIFI_PS_MIN_MODEM` | 80 MHz (DFS floor) | single-shot on demand | `telemetry_interval_idle_sec` (default 30 s) | on |
| `LOW_POWER` | No activity for `idle_to_low_power_sec` (default 300 s) total, **or** `low_battery` true, **and** rover state == IDLE **and** no lock held | `WIFI_PS_MAX_MODEM` | 80 MHz (DFS floor) | single-shot on demand | `telemetry_interval_low_power_sec` (default 120 s) | off |

"Activity" = any of: motor command (move/stop/tank/turn), nav command, display command, Wi-Fi config change, OTA in progress, MCP tool call that mutates rover state, web `/cmd` request.

A `critical_battery_voltage` (default 9.6 V) check runs independently of mode timers: when crossed, the manager calls `wr_motor_stop()`, forces `LOW_POWER`, and logs `power: critical battery, voltage=%.2f` — it does not attempt to power off the MCU (no safe shutdown path exists).

### 2.2 `wave_rover_power_mgr` component (new)

Location: `application/wave_rover/components/wave_rover_power_mgr/` — new component, following CLAUDE.md's opaque-handle pattern (this is new code; existing `wave_rover_hal` predates this convention and is left as-is).

```c
// include/wave_rover_power_mgr.h
typedef struct wr_power_mgr_t *wr_power_mgr_handle_t;

typedef enum {
    WR_POWER_MODE_ACTIVE = 0,
    WR_POWER_MODE_IDLE,
    WR_POWER_MODE_LOW_POWER,
} wr_power_mode_t;

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

/* old_mode/new_mode/reason are valid only for the duration of the callback */
typedef void (*wr_power_mode_cb_t)(wr_power_mode_t old_mode,
                                    wr_power_mode_t new_mode,
                                    const char *reason, void *user_ctx);

esp_err_t wr_power_mgr_create(const wr_power_mgr_config_t *config,
                               wr_power_mgr_handle_t *ret_handle);
esp_err_t wr_power_mgr_delete(wr_power_mgr_handle_t handle);

/* Starts the internal FreeRTOS timer task that evaluates transitions. */
esp_err_t wr_power_mgr_start(wr_power_mgr_handle_t handle);

/* Called from motor/nav/display/web/MCP code paths on user-driven activity. */
void wr_power_mgr_notify_activity(wr_power_mgr_handle_t handle, const char *source);

/* Explicit mode change (MCP power.set_mode / Web UI button).
 * Rejected with ESP_ERR_INVALID_STATE if it would demote below a held lock,
 * or move to ACTIVE->non-ACTIVE while wr_rover_state_get() != IDLE. */
esp_err_t wr_power_mgr_set_mode(wr_power_mgr_handle_t handle,
                                 wr_power_mode_t mode, const char *reason);

wr_power_mode_t wr_power_mgr_get_mode(wr_power_mgr_handle_t handle);

esp_err_t wr_power_mgr_register_cb(wr_power_mgr_handle_t handle,
                                    wr_power_mode_cb_t cb, void *user_ctx);

/* Mode-floor lock: while held, mode cannot drop below WR_POWER_MODE_ACTIVE.
 * ttl_sec == 0 means "until explicitly released" (used by OTA). */
esp_err_t wr_power_mgr_acquire_lock(wr_power_mgr_handle_t handle,
                                     const char *reason, uint32_t ttl_sec,
                                     uint32_t *ret_lock_id);
esp_err_t wr_power_mgr_release_lock(wr_power_mgr_handle_t handle, uint32_t lock_id);

/* JSON helpers for MCP/web — caller-owned buffer, bounded snprintf, matches
 * the wave_rover_mcp_metrics convention. */
esp_err_t wr_power_mgr_get_status_json(wr_power_mgr_handle_t handle,
                                        char *buf, size_t buf_len);
esp_err_t wr_power_mgr_get_locks_json(wr_power_mgr_handle_t handle,
                                       char *buf, size_t buf_len);

/* Apply current config's CPU-frequency policy for the given mode.
 * Internal helper, exposed for the metrics module to read back the
 * *actual* applied frequency via esp_pm. */
uint32_t wr_power_mgr_get_cpu_freq_mhz(wr_power_mgr_handle_t handle);
```

Internals (`.c`, single ~600-line file — under the 1500-line guidance):
- `struct wr_power_mgr_t`: config copy, current mode, last-activity timestamp (`esp_timer_get_time()`), array of up to 4 locks (`{reason[32], expires_at_us, active}`), mutex (`SemaphoreHandle_t`), registered callback + user_ctx, FreeRTOS timer (1 Hz tick).
- Evaluation timer task (1 Hz):
  1. Expire timed-out locks.
  2. If `wr_rover_state_get() != WR_ROVER_STATE_IDLE` → activity (forces ACTIVE, resets timer) — this is the state-machine reconciliation point.
  3. If a lock is held → floor at ACTIVE, skip demotion checks.
  4. Check `wr_power_get_status(&ps)`; if `ps.load_voltage_v < critical_battery_voltage` and `> 1.0` → `wr_motor_stop()`, force LOW_POWER, log `power: critical battery, voltage=%.2f`.
  5. Else if `ps.low_battery` → target LOW_POWER.
  6. Else compute target mode from elapsed idle time vs. `active_timeout_sec` / `idle_to_low_power_sec`.
  7. If target != current → `apply_mode()`.
- `apply_mode()`: under mutex, calls (best-effort, logs+continues on individual `esp_err_t` failures — never aborts a transition because one peripheral call failed):
  - `esp_wifi_set_ps()` per table (only if `config.wifi_power_save`).
  - `esp_pm_configure()` to set min/max CPU freq per table (only if `config.reduce_cpu_frequency`; no-op + `ESP_LOGW` once if `CONFIG_PM_ENABLE` is off).
  - `wave_rover_imu_set_mag_continuous(bool)` (new HAL function, see 2.3).
  - `wave_rover_display_set_power(bool on)` (new HAL function, see 2.3) — only if `config.disable_display_when_idle`.
  - Records new mode, timestamp; invokes user callback with `(old, new, reason)`.
  - `ESP_LOGI(TAG, "power: mode changed %s -> %s, reason=%s", ...)`.

### 2.3 HAL additions (existing files, small diffs)

- `wave_rover_imu.h/.c`: add
  ```c
  /* Switches AK09918 between continuous 10Hz (true) and power-down/single-shot (false).
   * No-op if magnetometer not present. */
  esp_err_t wr_imu_set_mag_continuous(bool enable);
  ```
  `wr_imu_get_sample()` already does a single-shot trigger-and-read when not in continuous mode for QMI8658; the same `AK_CNTL2` register write (0x08 = continuous 10Hz, 0x01 = single measurement) toggles AK09918 mode.

- `wave_rover_display.h/.c`: add
  ```c
  /* SSD1306 display on/off via command 0xAF/0xAE. Does not clear framebuffer —
   * content reappears when re-enabled. */
  esp_err_t wr_display_set_power(bool on);
  ```

- `wave_rover_power.h` (HAL): no change — `wr_power_status_t.load_voltage_v` and `.low_battery` already sufficient.

### 2.4 Config (`wave_rover_config.h` / `.c`)

New fields appended to `wave_rover_config_t` (after `syslog_facility`):

```c
bool     power_mgr_enabled;            /* default true */
uint16_t power_active_timeout_sec;     /* default 60 */
uint16_t power_idle_to_low_power_sec;  /* default 300 */
bool     power_wifi_power_save;        /* default true */
bool     power_reduce_cpu_frequency;   /* default true */
bool     power_disable_display_idle;   /* default true */
float    power_critical_battery_v;     /* default 9.6 */
uint16_t power_telemetry_active_sec;   /* default 5 */
uint16_t power_telemetry_idle_sec;     /* default 30 */
uint16_t power_telemetry_low_power_sec;/* default 120 */
```

`wave_rover_config_defaults()` and the NVS load/save loops in `wave_rover_config.c` get matching entries (mirroring the existing per-field `nvs_get_*`/`nvs_set_*` pattern).

### 2.5 Wiring into `app_main.c`

- After `wr_hal_init()` and `wr_wifi_init()`, before `wave_rover_mcp_start()`:
  ```c
  wr_power_mgr_config_t pm_cfg = {
      .enabled = s_cfg.power_mgr_enabled,
      .active_timeout_sec = s_cfg.power_active_timeout_sec,
      .idle_to_low_power_sec = s_cfg.power_idle_to_low_power_sec,
      .wifi_power_save = s_cfg.power_wifi_power_save,
      .reduce_cpu_frequency = s_cfg.power_reduce_cpu_frequency,
      .disable_display_when_idle = s_cfg.power_disable_display_idle,
      .critical_battery_voltage = s_cfg.power_critical_battery_v,
      .telemetry_interval_active_sec = s_cfg.power_telemetry_active_sec,
      .telemetry_interval_idle_sec = s_cfg.power_telemetry_idle_sec,
      .telemetry_interval_low_power_sec = s_cfg.power_telemetry_low_power_sec,
  };
  ESP_ERROR_CHECK(wr_power_mgr_create(&pm_cfg, &s_power_mgr));
  ESP_ERROR_CHECK(wr_power_mgr_start(s_power_mgr));
  ```
- `wave_rover_mcp_start()` signature gains `wr_power_mgr_handle_t power_mgr` so tool/web registration can reach it (mirrors how `s_cfg` is already threaded through).
- OTA handler (`handle_update`, `wave_rover_mcp_web.c`) acquires a `reason="ota", ttl_sec=0` lock at the start and releases it (or simply reboots, which re-creates the manager) at the end — satisfies "never lose Wi-Fi during OTA" by keeping mode at ACTIVE (full CPU/Wi-Fi) throughout.

### 2.6 Activity hooks

`wr_power_mgr_notify_activity(handle, source)` is called from:
- `wave_rover_mcp_tools.c`: any `rover.move/drive_tank/turn/stop/rotate_deg/drive_cm/nav_to/display_*` tool handler, on entry.
- `wave_rover_mcp_web.c`: `handle_cmd` (the `/cmd` motor-control endpoint), `handle_settings_post`.
- This is in addition to (not instead of) the `wr_rover_state_get()` busy check — covers "user is actively poking the web UI but motors are at 0" (e.g. adjusting settings), which `wr_rover_state_get()` alone wouldn't catch.

### 2.7 MCP tools (`wave_rover_mcp_tools.c`)

Following the existing tool-registration pattern (`mcp_register_tool(..., handler, schema)`):

| Tool | Request | Response |
|---|---|---|
| `rover.power_get_status` | `{}` | `{"mode":"IDLE","battery_voltage":11.8,"low_battery":false,"wifi_power_save":"min_modem","cpu_freq_mhz":80,"display_on":true,"uptime_sec":1234,"last_activity_sec_ago":42}` (from `wr_power_mgr_get_status_json`) |
| `rover.power_set_mode` | `{"mode":"ACTIVE"\|"IDLE"\|"LOW_POWER","reason":"..."}` | `{"ok":true,"mode":"ACTIVE"}` or `{"ok":false,"error":"rover is driving"}` |
| `rover.power_configure` | any subset of the `power_*` config fields (§2.4) | updated config (saved via `wave_rover_config_save`), applied live via `wr_power_mgr` setters |
| `rover.power_prevent_sleep` | `{"reason":"manual_control","ttl_sec":300}` | `{"lock_id":3}` |
| `rover.power_release_sleep_lock` | `{"lock_id":3}` | `{"ok":true}` |

`rover.power_set_mode("LOW_POWER"/"IDLE")` returns an error (not silently ignored — "не скрывать ошибки питания") if `wr_rover_state_get() != IDLE` or any lock is held.

### 2.8 `/metrics` additions (`wave_rover_mcp_metrics.c`)

```
# TYPE wave_rover_power_mode gauge
wave_rover_power_mode{mode="active"} 0
wave_rover_power_mode{mode="idle"} 1
wave_rover_power_mode{mode="low_power"} 0
# TYPE wave_rover_power_cpu_freq_mhz gauge
wave_rover_power_cpu_freq_mhz 80
# TYPE wave_rover_power_wifi_ps gauge
wave_rover_power_wifi_ps 1
# TYPE wave_rover_power_locks_active gauge
wave_rover_power_locks_active 0
```
(Same "enum" pattern as the existing `wave_rover_state{state=...}` metric.)

### 2.9 Web UI (`wave_rover_mcp_web.c`)

A new `<section id="power">` block in the existing single-page HTML, polled via the existing `/status`-style fetch pattern. New `GET /power` JSON endpoint returns the same payload as `rover.power_get_status`, reusing `wr_power_mgr_get_status_json`. Buttons for ACTIVE/IDLE/LOW_POWER call `POST /power` `{"mode":...}` → `wr_power_mgr_set_mode`. No camera/video rows.

### 2.10 Logging

All transitions and notable events go through `ESP_LOGI`/`ESP_LOGW` with the `power:` prefix matching the prompt's examples, e.g.:
```
power: mode changed ACTIVE -> IDLE, reason=active_timeout
power: mode changed IDLE -> LOW_POWER, reason=low_battery
power: critical battery, voltage=9.4
power: mode change rejected, target=LOW_POWER, reason=rover_busy
power: lock acquired, reason=ota, ttl=0
power: lock released, reason=ota
```
These are automatically forwarded as structured JSON to Loki via the existing `wr_syslog` pipeline — no new transport.

### 2.11 Safety constraint mapping

| Constraint (prompt) | Enforcement point |
|---|---|
| Never sleep during movement | §2.2 step 2: `wr_rover_state_get() != IDLE` forces ACTIVE every tick |
| Never leave motors on after disconnect | Already enforced by existing 500 ms web watchdog (`wr_web_wd`) — unchanged |
| Never disable Wi-Fi during OTA | OTA acquires a lock → mode pinned at ACTIVE → `WIFI_PS_NONE` |
| Never break Web UI/MCP | No mode disables the httpd or MCP server; only Wi-Fi PS / CPU freq / sensor poll rates / display change |
| `DEEP_SLEEP` off by default | Not implemented at all in v1 (see §1) |
| Don't degrade control via CPU freq | DFS floor is 80 MHz (not the ESP32 minimum of 10 MHz); LEDC/I2C run off APB clock, unaffected by CPU DFS without light sleep |
| Don't hide power errors | `esp_pm_configure`/`esp_wifi_set_ps`/HAL call failures are `ESP_LOGW`'d with the error code, mode transition still completes for the parts that succeeded |
| Don't remove existing functionality | All new behavior is additive and gated by `power_mgr_enabled` (default true, but settable false to fully restore today's behavior) |

---

## 3. Open items carried into the plan as explicit decisions (not questions)

1. **CPU DFS scope**: only `esp_pm_configure(min=80,max=240,light_sleep=false)`-style DFS, gated by `CONFIG_PM_ENABLE`. If enabling `CONFIG_PM_ENABLE` itself causes build/sdkconfig friction (it pulls in `esp_pm` + may require `CONFIG_FREERTOS_USE_TICKLESS_IDLE=n` to stay false explicitly), the plan's CPU-freq task is independent and can be dropped without affecting Wi-Fi PS / IMU / display / config / MCP work.
2. **Battery thresholds use voltage, not percent** (§1) — `power_critical_battery_v` default 9.6 V is a placeholder for a 3S pack at ~3.2 V/cell; document as tunable via `rover.power_configure`.
3. **Mode-floor locks, not sleep locks** (§1) — naming kept as `power_prevent_sleep`/`power_release_sleep_lock` for prompt/API continuity, but semantics are "floor at ACTIVE".
