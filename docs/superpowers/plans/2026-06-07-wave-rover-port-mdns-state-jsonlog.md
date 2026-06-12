# Wave Rover: mDNS, derived-state status pill, JSON log forwarding — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [x]`) syntax for tracking.

**Goal:** Port three features from `~/repos/ai-rover` into `wave_rover`: an mDNS
hostname (`wave-rover.local`), a derived-state color-coded status pill in the
web UI, and structured JSON log forwarding (for Loki ingestion) — all without
touching Wi-Fi connection logic (explicitly declined by the user) or rewriting
existing `ESP_LOGx` call sites.

**Architecture:**
- mDNS: one new managed-component dependency (`espressif/mdns`, declared in
  `main/idf_component.yml`) plus ~10 lines in `app_main.c` after Wi-Fi comes up.
- Status: a small new private module (`wave_rover_mcp_state.{h,c}`) inside the
  `wave_rover_mcp` component computes a *derived* state
  (`idle`/`driving`/`nav_busy`/`estop`) from values the code already polls
  (motor state, e-stop) plus one new flag set around the three blocking nav
  tool calls; `/status` gains a `"state"` field; the web UI's JS gains a
  color map.
- Logging: all formatting happens centrally inside the existing
  `wr_syslog.c` UDP-forwarder hook — it already intercepts every log line via
  `esp_log_set_vprintf()`. We parse the rendered line
  (`"LEVEL (timestamp) TAG: message"`, confirmed colorless because
  `CONFIG_LOG_COLORS` is unset) and re-emit it as a JSON object inside the
  existing RFC 3164 envelope. UART output is untouched (stays human-readable).

**Tech Stack:** ESP-IDF (C), `esp_http_server`, `cJSON`, FreeRTOS, PlatformIO
build (`pio run`) for this app — see Task 0 for the exact build/flash commands.

**Reference design doc:** `docs/superpowers/specs/2026-06-07-ai-rover-feature-port-design.md`

---

## Task 0: Confirm the build loop before changing anything

This app builds via PlatformIO (`platformio.ini`, `src_dir = main`,
`board_build.partitions = partitions.csv`), NOT `idf.py` directly — there is
a `.pio/build/wave_rover` artifact directory and the binary lives at
`~/.local/bin/pio`. Confirm this works *before* making any change, so a build
failure later is attributable to your edit, not a pre-existing issue.

**Files:** none (verification only)

- [x] **Step 1: Run a clean build and record the baseline**

```bash
cd /home/artem/repos/esp-claw/application/wave_rover
~/.local/bin/pio run
```

Expected: `SUCCESS` at the end, with a `.pio/build/wave_rover/firmware.bin`
timestamp updated. If it fails, STOP and resolve the pre-existing failure
first — do not build on top of a broken baseline.

- [x] **Step 2: Note the flash command for later** (do not run yet)

```bash
~/.local/bin/pio run -t upload
```

This is what you'll run after each task to flash the board over USB/serial.
(No OTA path is used in this plan — all three features are additive and safe
to verify over a normal serial flash + monitor cycle.)

---

## Task 1: mDNS hostname (`wave-rover.local`)

**Files:**
- Modify: `application/wave_rover/main/CMakeLists.txt`
- Modify: `application/wave_rover/main/idf_component.yml`
- Modify: `application/wave_rover/main/app_main.c:31-34`

**Correction found during implementation:** `application/wave_rover/idf_component.yml`
(top-level) declares `espressif/mdns: "^1.0.0"`, but that file is **not**
the manifest the build actually consults for `main`'s dependencies — the
build log shows components are resolved from `main/idf_component.yml`
(confirmed: `dependencies.lock` has no `mdns` entry, and a build attempt with
only the `CMakeLists.txt` change fails with `Failed to resolve component
'mdns' required by component 'main': unknown name`). `mdns` must be added to
`main/idf_component.yml`'s `dependencies:` map — the top-level file appears to
be a stray leftover, not a consulted manifest. `mdns` is also not a built-in
ESP-IDF component in this `framework-espidf` package — it's a managed
component pulled from the registry once declared.

The config struct already has a `hostname` field
(`wave_rover_config.h:19`, defaults to `"wave-rover"` in
`wave_rover_config.c:20`) — no new config is needed.

- [x] **Step 1a: Declare the `espressif/mdns` managed-component dependency**

Edit `application/wave_rover/main/idf_component.yml` (the manifest the build
actually consults for `main` — not the stray top-level
`application/wave_rover/idf_component.yml`). Current content:

```yaml
dependencies:
  wave_rover_config:
    path: ../components/wave_rover_config
  wave_rover_hal:
    path: ../components/wave_rover_hal
  wave_rover_mcp:
    path: ../components/wave_rover_mcp
```

Add an `espressif/mdns` entry:

```yaml
dependencies:
  espressif/mdns:
    version: "^1.0.0"
  wave_rover_config:
    path: ../components/wave_rover_config
  wave_rover_hal:
    path: ../components/wave_rover_hal
  wave_rover_mcp:
    path: ../components/wave_rover_mcp
```

- [x] **Step 1b: Add `mdns` to the main component's `PRIV_REQUIRES`**

Edit `application/wave_rover/main/CMakeLists.txt`. Current content:

```cmake
idf_component_register(
    SRCS "app_main.c" "wr_wifi.c" "wr_syslog.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        wave_rover_config
        wave_rover_hal
        wave_rover_mcp
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        lwip
        log
        esp_common
        freertos
)
```

Add `mdns` to the `PRIV_REQUIRES` list (alphabetical placement next to
`lwip`/`log` is fine — follow the existing list's rough grouping):

```cmake
idf_component_register(
    SRCS "app_main.c" "wr_wifi.c" "wr_syslog.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        wave_rover_config
        wave_rover_hal
        wave_rover_mcp
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        lwip
        mdns
        log
        esp_common
        freertos
)
```

- [x] **Step 2: Initialize mDNS in `app_main.c` after Wi-Fi comes up**

Current relevant section of `application/wave_rover/main/app_main.c`
(lines 5-14 includes, 31-41 body):

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "wave_rover_config.h"
#include "wave_rover_hal.h"
#include "wave_rover_mcp.h"
#include "wr_wifi.h"
#include "wr_syslog.h"
```

```c
    ESP_ERROR_CHECK(wr_wifi_init(&s_cfg));
    wr_syslog_start(s_cfg.syslog_enabled, s_cfg.syslog_host,
                    s_cfg.syslog_port, s_cfg.syslog_facility);
    ESP_ERROR_CHECK(wave_rover_mcp_start(&s_cfg));

    const char *ip = wr_wifi_get_ip();
```

Add `#include "mdns.h"` to the include block, and insert mDNS bring-up right
after `wr_wifi_init()` (it needs the netifs that call creates) and before
`wave_rover_mcp_start()`. mDNS failure must not be fatal — log a warning and
continue, matching the optional-feature style already used for syslog:

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "mdns.h"
#include "wave_rover_config.h"
#include "wave_rover_hal.h"
#include "wave_rover_mcp.h"
#include "wr_wifi.h"
#include "wr_syslog.h"
```

```c
    ESP_ERROR_CHECK(wr_wifi_init(&s_cfg));

    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_hostname_set(s_cfg.hostname);
        mdns_instance_name_set("Wave Rover MCP");
        mdns_service_add(NULL, "_http", "_tcp", s_cfg.mcp_port, NULL, 0);
        ESP_LOGI(TAG, "mDNS: %s.local -> :%u", s_cfg.hostname, s_cfg.mcp_port);
    } else {
        ESP_LOGW(TAG, "mDNS init failed: %s", esp_err_to_name(mdns_err));
    }

    wr_syslog_start(s_cfg.syslog_enabled, s_cfg.syslog_host,
                    s_cfg.syslog_port, s_cfg.syslog_facility);
    ESP_ERROR_CHECK(wave_rover_mcp_start(&s_cfg));

    const char *ip = wr_wifi_get_ip();
```

- [x] **Step 3: Build**

```bash
cd /home/artem/repos/esp-claw/application/wave_rover
~/.local/bin/pio run
```

Expected: `SUCCESS`. The component manager will print a line about resolving/
downloading `espressif/mdns` on first build — that's expected, not an error.
If it fails to resolve the dependency, check `dependencies.lock` got updated
(it's checked into git; a diff there is expected and should be committed
alongside the code change).

- [x] **Step 4: Flash and verify over the network**

```bash
~/.local/bin/pio run -t upload
```

Then from a machine on the same LAN as the rover (in STA mode):

```bash
ping wave-rover.local
```

Expected: resolves to the rover's IP and responds. Also check the boot log
(serial monitor or `wr_syslog` UDP stream) for the `"mDNS: wave-rover.local -> :80"`
line. If `wifi_mode` is AP-only (`0`), `.local` resolution requires the test
machine to be connected to the rover's AP — that's expected, not a bug.

- [x] **Step 5: Commit**

```bash
git add application/wave_rover/main/CMakeLists.txt \
        application/wave_rover/main/app_main.c \
        application/wave_rover/dependencies.lock
git commit -m "feat(wave_rover): advertise hostname via mDNS"
```

(If the component manager also touched `managed_components/` or
`sdkconfig.wave_rover`, check `git status` and include only the files that
are already tracked or clearly belong to this change — do not blanket-add.)

---

## Task 2: Derived rover state + color-coded web status pill

**Files:**
- Create: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_state.h`
- Create: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_state.c`
- Modify: `application/wave_rover/components/wave_rover_mcp/CMakeLists.txt`
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_tools.c`
- Modify: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_web.c`

The state is **derived**, not transition-tracked: three of its four values
come straight from data the code already polls every `/status` refresh
(`wr_motor_get_state()`, `wr_motor_emergency_stop_active()`). The only thing
that can't be derived from existing polled values is "a blocking nav command
is currently running" — that needs exactly one new shared flag, set/cleared
around the three MCP tools that call into `wr_nav_drive_cm`/`wr_nav_rotate_deg`.

States and their web-UI colors (mirrors ai-rover's `state_color()` palette):

| state      | meaning                                  | color     |
|------------|------------------------------------------|-----------|
| `idle`     | nothing happening                         | `#2d8b2d` (green)  |
| `driving`  | motors turning (joystick/tank/move)        | `#2563eb` (blue)   |
| `nav_busy` | blocking `drive_cm`/`rotate_deg`/`nav_to`  | `#d97706` (amber)  |
| `estop`    | emergency stop active                      | `#dc2626` (red)    |

- [x] **Step 1: Create the state module header**

Create `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_state.h`:

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WR_ROVER_STATE_IDLE = 0,
    WR_ROVER_STATE_DRIVING,
    WR_ROVER_STATE_NAV_BUSY,
    WR_ROVER_STATE_ESTOP,
} wr_rover_state_t;

/* Mark whether a blocking nav command (drive_cm / rotate_deg / nav_to) is
 * currently running. Called only from the MCP tool-dispatch task, around
 * the synchronous wr_nav_* calls. */
void wr_rover_state_set_nav_busy(bool busy);

/* Derives the current rover state from motor state, e-stop, and the
 * nav-busy flag. Safe to call from any task (e.g. the HTTP server task
 * serving /status). */
wr_rover_state_t wr_rover_state_get(void);

/* Lower-case identifier used as the JSON "state" value and as the lookup
 * key in the web UI's color map ("idle", "driving", "nav_busy", "estop"). */
const char *wr_rover_state_name(wr_rover_state_t state);

#ifdef __cplusplus
}
#endif
```

- [x] **Step 2: Create the state module source**

Create `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_state.c`:

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_mcp_state.h"
#include "wave_rover_hal.h"

/* Single bool flag, one writer (MCP tool-dispatch task), one reader (HTTP
 * server task via wr_rover_state_get). volatile is sufficient for a
 * single-word flag on this target — mirrors s_cached_bat_v in
 * wave_rover_mcp_web.c. */
static volatile bool s_nav_busy = false;

void wr_rover_state_set_nav_busy(bool busy)
{
    s_nav_busy = busy;
}

wr_rover_state_t wr_rover_state_get(void)
{
    if (wr_motor_emergency_stop_active()) {
        return WR_ROVER_STATE_ESTOP;
    }
    if (s_nav_busy) {
        return WR_ROVER_STATE_NAV_BUSY;
    }

    wr_motor_state_t ms = {0};
    wr_motor_get_state(&ms);
    if (ms.left != 0.0f || ms.right != 0.0f) {
        return WR_ROVER_STATE_DRIVING;
    }

    return WR_ROVER_STATE_IDLE;
}

const char *wr_rover_state_name(wr_rover_state_t state)
{
    switch (state) {
    case WR_ROVER_STATE_DRIVING:  return "driving";
    case WR_ROVER_STATE_NAV_BUSY: return "nav_busy";
    case WR_ROVER_STATE_ESTOP:    return "estop";
    case WR_ROVER_STATE_IDLE:
    default:                      return "idle";
    }
}
```

- [x] **Step 3: Register the new source file in the component**

Edit `application/wave_rover/components/wave_rover_mcp/CMakeLists.txt`.
Current `SRCS` line:

```cmake
    SRCS "wave_rover_mcp.c" "wave_rover_mcp_tools.c" "wave_rover_mcp_web.c"
```

Change to:

```cmake
    SRCS "wave_rover_mcp.c" "wave_rover_mcp_tools.c" "wave_rover_mcp_web.c" "wave_rover_mcp_state.c"
```

(`wave_rover_mcp_state.h` is a private header — it lives next to the `.c`
files, not under `include/`, so no `INCLUDE_DIRS` change is needed; the two
consumer files in the same directory include it with `"wave_rover_mcp_state.h"`.)

- [x] **Step 4: Build to confirm the new module compiles and links**

```bash
cd /home/artem/repos/esp-claw/application/wave_rover
~/.local/bin/pio run
```

Expected: `SUCCESS` (the new files aren't referenced anywhere yet, so this
just confirms they compile standalone).

- [x] **Step 5: Wire `nav_busy` into the three blocking nav tools**

Edit `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_tools.c`.

Add the include near the top, alongside the existing local includes:

```c
#include "wave_rover_mcp_state.h"
```

**5a. `tool_rotate_deg`** — current body around line 513-521:

```c
    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    float integrated = 0.0f;
    esp_err_t err = wr_nav_rotate_deg(angle_deg, speed, &integrated);
```

Change the last two lines to:

```c
    float integrated = 0.0f;
    wr_rover_state_set_nav_busy(true);
    esp_err_t err = wr_nav_rotate_deg(angle_deg, speed, &integrated);
    wr_rover_state_set_nav_busy(false);
```

(`wr_nav_rotate_deg` is synchronous/blocking — it always returns before this
line resumes, so a single set-before/clear-after pair is correct regardless
of which of its internal early-return paths fires; no restructuring of
`wave_rover_nav.c` is needed.)

**5b. `tool_drive_cm`** — current body around line 552-561:

```c
    wr_nav_cal_t cal = wr_nav_get_cal();
    if (!cal.k_dist_valid) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "distance_not_calibrated");
        cJSON_AddStringToObject(root, "hint",  "call rover.set_nav_cal with k_dist");
        return json_obj_str(root);
    }

    int32_t dur_ms = (int32_t)(distance_cm / (cal.k_dist * speed));
    esp_err_t err = wr_nav_drive_cm(distance_cm, speed);
```

Change the last two lines to:

```c
    int32_t dur_ms = (int32_t)(distance_cm / (cal.k_dist * speed));
    wr_rover_state_set_nav_busy(true);
    esp_err_t err = wr_nav_drive_cm(distance_cm, speed);
    wr_rover_state_set_nav_busy(false);
```

**5c. `tool_nav_to`** — current body around line 587-611:

```c
    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    bool drive_ok = true;
    bool rotate_ok = true;
    float integrated_deg = 0.0f;

    if (distance_cm > 0.01f) {
        wr_nav_cal_t cal = wr_nav_get_cal();
        if (!cal.k_dist_valid) {
            cJSON_AddBoolToObject(root,   "ok",    false);
            cJSON_AddStringToObject(root, "error", "distance_not_calibrated");
            cJSON_AddStringToObject(root, "hint",  "call rover.set_nav_cal with k_dist");
            return json_obj_str(root);
        }
        drive_ok = (wr_nav_drive_cm(distance_cm, speed) == ESP_OK);
    }

    if (drive_ok && fabsf(angle_deg) > 0.5f) {
        rotate_ok = (wr_nav_rotate_deg(angle_deg, speed, &integrated_deg) == ESP_OK);
    }
```

This one needs a small reorder: the calibration check must happen — and be
able to early-return — *before* `nav_busy` is set, otherwise the early return
on `distance_not_calibrated` would leave the flag stuck `true`. Replace the
whole block above with:

```c
    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root,   "ok",    false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    bool need_drive = (distance_cm > 0.01f);
    wr_nav_cal_t cal = {0};
    if (need_drive) {
        cal = wr_nav_get_cal();
        if (!cal.k_dist_valid) {
            cJSON_AddBoolToObject(root,   "ok",    false);
            cJSON_AddStringToObject(root, "error", "distance_not_calibrated");
            cJSON_AddStringToObject(root, "hint",  "call rover.set_nav_cal with k_dist");
            return json_obj_str(root);
        }
    }

    bool drive_ok = true;
    bool rotate_ok = true;
    float integrated_deg = 0.0f;

    wr_rover_state_set_nav_busy(true);
    if (need_drive) {
        drive_ok = (wr_nav_drive_cm(distance_cm, speed) == ESP_OK);
    }
    if (drive_ok && fabsf(angle_deg) > 0.5f) {
        rotate_ok = (wr_nav_rotate_deg(angle_deg, speed, &integrated_deg) == ESP_OK);
    }
    wr_rover_state_set_nav_busy(false);
```

(`(void)cal;` is not needed — `cal` is read inside the `if (need_drive)`
branch via `cal.k_dist_valid`/implicitly through `wr_nav_drive_cm`'s use of
the calibration stored in `wave_rover_nav`; it's only fetched here to run the
validity check. If your compiler warns about `cal` being set-but-unused
outside that branch, that's a pre-existing pattern question — leave `cal` as
declared since it mirrors the original code's structure and the validity
check is the only thing that needs it.)

- [x] **Step 6: Extend `/status` JSON with the `state` field**

Edit `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_web.c`.

Add the include near the top alongside `"wave_rover_hal.h"`:

```c
#include "wave_rover_mcp_state.h"
```

Current `handle_status` (lines 387-403):

```c
static esp_err_t handle_status(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    wr_motor_state_t ms = {0};
    wr_motor_get_state(&ms);

    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"bat_v\":%.2f,\"estop\":%s,\"left\":%.2f,\"right\":%.2f}",
             (double)s_cached_bat_v,
             wr_motor_emergency_stop_active() ? "true" : "false",
             (double)ms.left,
             (double)ms.right);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}
```

Replace with:

```c
static esp_err_t handle_status(httpd_req_t *req)
{
    WEB_REQUIRE_AUTH(req);
    wr_motor_state_t ms = {0};
    wr_motor_get_state(&ms);

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"bat_v\":%.2f,\"estop\":%s,\"left\":%.2f,\"right\":%.2f,\"state\":\"%s\"}",
             (double)s_cached_bat_v,
             wr_motor_emergency_stop_active() ? "true" : "false",
             (double)ms.left,
             (double)ms.right,
             wr_rover_state_name(wr_rover_state_get()));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}
```

(Buffer grew from 128 to 160 — the longest possible addition is
`,"state":"nav_busy"` = 20 bytes, so 128+32 headroom is comfortable.)

- [x] **Step 7: Color the status pill from `state` in the web UI JS**

Current pill markup (`wave_rover_mcp_web.c:162`):

```c
    "<span class='pill' id='ePill' style='background:#2d8b2d'>OK</span>"
```

Change the initial label to `IDLE` so it matches the new vocabulary before
the first `/status` fetch completes:

```c
    "<span class='pill' id='ePill' style='background:#2d8b2d'>IDLE</span>"
```

Current `rf()` (status refresh) function (lines 273-278):

```c
    "async function rf(){try{"
    "const r=await fetch('/status');const j=await r.json();"
    "document.getElementById('ePill').textContent=j.estop?'E-STOP':'OK';"
    "document.getElementById('ePill').style.background=j.estop?'#dc2626':'#2d8b2d';"
    "document.getElementById('sBat').textContent=j.bat_v>0.1?j.bat_v.toFixed(1)+'V':'bat:--';"
    "}catch(e){document.getElementById('ePill').textContent='ERR';}}"
```

Replace with a state→color lookup map (mirrors ai-rover's `state_color()`
palette) driven by the new `state` field:

```c
    "const ST_COLORS={idle:'#2d8b2d',driving:'#2563eb',nav_busy:'#d97706',estop:'#dc2626'};"
    "async function rf(){try{"
    "const r=await fetch('/status');const j=await r.json();"
    "const st=j.state||'idle';"
    "const pill=document.getElementById('ePill');"
    "pill.textContent=st.toUpperCase().replace('_',' ');"
    "pill.style.background=ST_COLORS[st]||'#374151';"
    "document.getElementById('sBat').textContent=j.bat_v>0.1?j.bat_v.toFixed(1)+'V':'bat:--';"
    "}catch(e){document.getElementById('ePill').textContent='ERR';}}"
```

(`'NAV BUSY'`/`'IDLE'`/`'DRIVING'`/`'ESTOP'` displayed text via
`.toUpperCase().replace('_',' ')`; unknown/future state values fall back to
gray `#374151`, matching ai-rover's `default:` branch in `state_color()`.)

- [x] **Step 8: Build**

```bash
cd /home/artem/repos/esp-claw/application/wave_rover
~/.local/bin/pio run
```

Expected: `SUCCESS`.

- [x] **Step 9: Flash and verify in the browser**

```bash
~/.local/bin/pio run -t upload
```

Open the rover's web UI (`http://wave-rover.local/` or its IP). Verify:
- Pill shows `IDLE` / green at rest.
- Driving via the joystick/tank controls turns the pill `DRIVING` / blue.
- Calling `rover.drive_cm` or `rover.rotate_deg` (e.g. via an MCP client, or
  `curl -u <user>:<pass> -X POST http://<ip>/cmd ...` if the web UI exposes a
  manual trigger) turns it `NAV BUSY` / amber for the command's duration, then
  back to `IDLE`/`DRIVING`.
- Triggering E-STOP turns it `ESTOP` / red regardless of motor state, and
  clearing it returns to the motor-derived state.
- `curl -s http://<ip>/status` (with auth if enabled) returns valid JSON
  containing the new `"state"` field.

- [x] **Step 10: Commit**

```bash
git add application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_state.h \
        application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_state.c \
        application/wave_rover/components/wave_rover_mcp/CMakeLists.txt \
        application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_tools.c \
        application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_web.c
git commit -m "feat(wave_rover): derived rover state + color-coded web status pill"
```

---

## Task 3: Forward all logs as structured JSON (for Loki)

**Files:**
- Modify: `application/wave_rover/main/wr_syslog.c`

`wr_syslog_init()` already installs `syslog_vprintf` via
`esp_log_set_vprintf()`, which intercepts **every** `ESP_LOGx` call in the
firmware — this is the single chokepoint we need; no call site changes
anywhere else. `CONFIG_LOG_DEFAULT_LEVEL_INFO=y` and `CONFIG_LOG_COLORS` is
**not** set in `sdkconfig.wave_rover`, so every rendered line has the fixed,
colorless shape `"<LEVEL> (<ms-timestamp>) <TAG>: <message>"`, e.g.:

```
I (12345) wr_wifi: STA connected, IP=192.168.1.5
```

`syslog_vprintf` already does `vprintf(fmt, args)` for UART (kept exactly as
is — serial stays human-readable) and separately enqueues the
newline-stripped rendered line for UDP forwarding. We change only what
`syslog_task` does with that queued line before sending: parse it into
fields, then emit JSON as the body of the existing RFC 3164 envelope so a
Loki `syslog` receiver + `json` pipeline stage can extract clean labels:

```
<134>wave-rover: {"ts":12345,"level":"info","tag":"wr_wifi","msg":"STA connected, IP=192.168.1.5"}
```

- [x] **Step 1: Add the includes the parser needs**

Current includes in `application/wave_rover/main/wr_syslog.c` (lines 9-18):

```c
#include "wr_syslog.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
```

Add `<stdlib.h>` (for `strtoul`):

```c
#include "wr_syslog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
```

- [x] **Step 2: Add a JSON-string-escaping helper**

Insert this just above the `/* UDP sender task */` section comment (before
`syslog_task`, around current line 55):

```c
/* ------------------------------------------------------------------ */
/* JSON formatting for the UDP-forwarded copy (UART stays plain text)  */
/* ------------------------------------------------------------------ */

/* Copies up to out_sz-1 bytes from in[0..in_len) into out, escaping the
 * characters that are illegal or special inside a JSON string (quote,
 * backslash, control characters). Always NUL-terminates. Truncates
 * silently on overflow rather than overrunning out — acceptable for a
 * best-effort log line; never crashes or writes out of bounds. */
static void json_escape(const char *in, size_t in_len, char *out, size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; i < in_len && in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        char seq[8];
        const char *esc = NULL;
        switch (c) {
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c < 0x20) {
                snprintf(seq, sizeof(seq), "\\u%04x", c);
                esc = seq;
            }
            break;
        }
        size_t add_len = esc ? strlen(esc) : 1;
        if (o + add_len > out_sz - 1) break;
        if (esc) {
            memcpy(out + o, esc, add_len);
        } else {
            out[o] = (char)c;
        }
        o += add_len;
    }
    out[o] = '\0';
}
```

- [x] **Step 3: Add the line parser + JSON formatter**

Insert directly below `json_escape` (still before `syslog_task`):

```c
/* Parses a rendered ESP-IDF log line "L (TTTT) TAG: message" (the fixed
 * LOG_FORMAT shape with CONFIG_LOG_COLORS unset — confirmed in
 * sdkconfig.wave_rover) and writes a single-line JSON object to out.
 * Lines that don't match the expected shape (e.g. raw printf output, the
 * "wr_syslog: forwarding logs to ..." startup line) fall back to emitting
 * the whole line as "msg" with level "info" and tag "" — never dropped,
 * never misparsed into a crash. */
static void format_json(const char *line, char *out, size_t out_sz)
{
    const char *level_word = "info";
    unsigned long ts = 0;
    const char *tag = "";
    size_t tag_len = 0;
    const char *msg = line;

    if (line[0] != '\0' && line[1] == ' ' && line[2] == '(') {
        switch (line[0]) {
        case 'E': level_word = "error";   break;
        case 'W': level_word = "warn";    break;
        case 'I': level_word = "info";    break;
        case 'D': level_word = "debug";   break;
        case 'V': level_word = "verbose"; break;
        default:                          break;
        }
        char *end = NULL;
        ts = strtoul(line + 3, &end, 10);
        if (end != NULL && end[0] == ')' && end[1] == ' ') {
            const char *tag_start = end + 2;
            const char *colon = strstr(tag_start, ": ");
            if (colon != NULL) {
                tag     = tag_start;
                tag_len = (size_t)(colon - tag_start);
                msg     = colon + 2;
            }
        }
    }

    char tag_buf[32];
    char msg_buf[MSG_MAX];
    json_escape(tag, tag_len, tag_buf, sizeof(tag_buf));
    json_escape(msg, strlen(msg), msg_buf, sizeof(msg_buf));

    snprintf(out, out_sz,
             "{\"ts\":%lu,\"level\":\"%s\",\"tag\":\"%s\",\"msg\":\"%s\"}",
             ts, level_word, tag_buf, msg_buf);
}
```

- [x] **Step 4: Use the formatter in `syslog_task` and size buffers for it**

Current `syslog_task` (lines 59-76):

```c
static void syslog_task(void *arg)
{
    char msg[MSG_MAX];
    char pkt[MSG_MAX + 32];

    for (;;) {
        if (xQueueReceive(s_queue, msg, portMAX_DELAY) != pdTRUE) continue;
        int fd = s_sock;
        if (fd < 0) continue;

        /* RFC 3164: <PRI>HOSTNAME TAG: MSG
         * PRI = facility*8 | severity (6 = info) */
        int pri = (int)s_facility * 8 + 6;
        int n = snprintf(pkt, sizeof(pkt), "<%d>wave-rover: %s", pri, msg);
        if (n > 0)
            send(fd, pkt, (size_t)n, 0);
    }
}
```

Replace with:

```c
static void syslog_task(void *arg)
{
    char msg[MSG_MAX];
    char json[MSG_MAX + 96];
    char pkt[sizeof(json) + 32];

    for (;;) {
        if (xQueueReceive(s_queue, msg, portMAX_DELAY) != pdTRUE) continue;
        int fd = s_sock;
        if (fd < 0) continue;

        format_json(msg, json, sizeof(json));

        /* RFC 3164: <PRI>HOSTNAME TAG: MSG
         * PRI = facility*8 | severity (6 = info) — MSG is now a JSON object
         * so a Loki "syslog" receiver + "json" pipeline stage can extract
         * level/tag/msg as labels without per-line regex parsing. */
        int pri = (int)s_facility * 8 + 6;
        int n = snprintf(pkt, sizeof(pkt), "<%d>wave-rover: %s", pri, json);
        if (n > 0)
            send(fd, pkt, (size_t)n, 0);
    }
}
```

- [x] **Step 5: Bump the task's stack size for the new local buffers**

`syslog_task` now carries `msg` (480 B) + `json` (576 B) + `pkt` (608 B) ≈
1.7 KB of locals, plus `format_json`'s own frame (`tag_buf`/`msg_buf` ≈
512 B) and `json_escape`'s small frame, plus libc `snprintf`/`vsnprintf`
overhead. The existing 3072-byte allocation is too tight a margin for that.

Current line (in `wr_syslog_init`, around line 89):

```c
    xTaskCreate(syslog_task, "wr_syslog", 3072, NULL, 2, NULL);
```

Change to:

```c
    xTaskCreate(syslog_task, "wr_syslog", 4096, NULL, 2, NULL);
```

- [x] **Step 6: Build**

```bash
cd /home/artem/repos/esp-claw/application/wave_rover
~/.local/bin/pio run
```

Expected: `SUCCESS`.

- [x] **Step 7: Flash and verify the JSON stream on the wire**

```bash
~/.local/bin/pio run -t upload
```

On a machine on the same subnet, listen on the syslog UDP port (default
`5514`, or whatever `cfg->syslog_port` is configured to — check
`rover.get_config` or the web settings page):

```bash
nc -ul 5514
```

Expected: lines like

```
<134>wave-rover: {"ts":12345,"level":"info","tag":"wr_wifi","msg":"STA connected, IP=192.168.1.5"}
```

Verify:
- Every line is valid JSON (pipe through `jq` to confirm:
  `nc -ul 5514 | while read -r l; do echo "$l" | sed 's/^<[0-9]*>wave-rover: //' | jq .; done`).
- `level`/`tag`/`msg` fields are populated correctly across at least one
  `I`, one `W` (e.g. trigger the "STA connect timeout" path or similar), and
  the periodic `D` heap log (raise the log level temporarily if needed to see
  a `D` line — `CONFIG_LOG_DEFAULT_LEVEL_INFO` means `ESP_LOGD` is compiled
  out by default, so a `debug`-level line may simply not appear; that's
  expected, not a bug).
- **No password/token fields appear** — spot-check the boot sequence
  (`wave_rover_config: config loaded: ...`) and any Wi-Fi-related lines;
  they should look identical in content to the existing plain-text forwarding,
  just JSON-wrapped (the formatter only restructures bytes that were already
  being forwarded — it cannot introduce a new secret leak, but verify anyway
  since this is a security-sensitive surface).
- The serial monitor (`~/.local/bin/pio run -t monitor`) still shows
  human-readable plain-text lines — UART output is untouched.

- [x] **Step 8: Commit**

```bash
git add application/wave_rover/main/wr_syslog.c
git commit -m "feat(wave_rover): forward all logs as structured JSON over the syslog UDP path"
```

---

## Final check

- [x] Run `~/.local/bin/pio run` once more from a clean state to confirm all
      three features build together:

```bash
cd /home/artem/repos/esp-claw/application/wave_rover
~/.local/bin/pio run -t clean
~/.local/bin/pio run
```

Expected: `SUCCESS`. Flash once more (`~/.local/bin/pio run -t upload`) and
re-verify all three behaviors (mDNS resolution, status-pill state coloring,
JSON log lines on the wire) together in one boot cycle.

- [x] Update `docs/wave_rover_user_guide.md` if it documents `/status` JSON
      shape, the syslog format, or how to discover the rover's address — add
      `wave-rover.local`, the new `"state"` field, and the JSON log shape
      where relevant. (Check first whether it documents these at all; if not,
      no doc change is needed — don't add new sections speculatively.)
