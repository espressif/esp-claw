# rover_demo Design Spec

**Date:** 2026-04-29
**Status:** Approved

## Summary

New application `application/rover_demo/` inside the esp-claw repository. Targets M5StickC Plus (ESP32-PICO-D4) with RoverC Pro robot base and UnitV-M12 camera. User controls the rover via Telegram. No Lua. Lightweight memory mode. Single LLM configuration shared by all components.

---

## 1. Architecture and Data Flow

```
User → Telegram
  → cap_im_tg (long-polling)
  → claw_event_t
  → claw_event_router (default rule: all messages → run_agent)
  → claw_core (LLM loop, up to 20 tool iterations)
  → claw_cap dispatcher
      ├── cap_rover      → rover_move / rover_turn / rover_stop
      │                     rover_gripper_open / rover_gripper_close / rover_read_imu
      │                     → RoverC Pro (I2C addr 0x38, SDA=GPIO0, SCL=GPIO26)
      │
      ├── cap_unitv      → unitv_scan / unitv_capture
      │                     → UnitV-M12 (UART1, TX=GPIO32, RX=GPIO33, 115200 baud)
      │                     unitv_capture: sends JPEG to LLM API (same credentials as claw_core)
      │
      ├── cap_im_tg      → tg_send_message (outbound reply to user)
      ├── cap_skill_mgr  → activate_skill / deactivate_skill
      ├── cap_system     → system_info
      ├── cap_time       → time_get
      └── cap_files      → file r/w on FATFS

  ← Response sent to Telegram via outbound binding
```

**LLM-visible capability groups** (always in context, not skill-gated):
`cap_rover`, `cap_unitv`, `cap_files`, `cap_skill`, `cap_system`

cap_im_tg and cap_time are registered but not in the LLM context — the event router and outbound binding handle them transparently.

---

## 2. New Components

### 2.1 cap_rover

**Path:** `components/claw_capabilities/cap_rover/`

```
CMakeLists.txt
include/cap_rover.h
src/
  cap_rover.c           — group registration, execute dispatcher
  cap_rover_hw.c        — rover_hw_task, command queue, I2C writes
  cap_rover_internal.h
  cmd_cap_rover.c       — CLI commands for manual hardware testing
skills/
  cap_rover.md
  skills_list.json
```

**Capabilities (all LLM-callable):**

| ID | Parameters | Action |
|----|------------|--------|
| `rover_move` | x, y, z (−100..100), duration_ms (100-5000) | Move for N ms then stop |
| `rover_turn` | direction (left/right), angle_deg (5-360), speed_pct (20-100) | Rotate using IMU gyro feedback |
| `rover_stop` | — | Immediate emergency stop |
| `rover_gripper_open` | — | Servo → open angle (35°) |
| `rover_gripper_close` | — | Servo → close angle (150°) |
| `rover_read_imu` | — | Returns accelerometer + gyroscope JSON |

**Hardware task pattern:**
`rover_hw_task` (stack 4KB, core 0) is the sole owner of the I2C bus. All cap callbacks enqueue an `rover_action_req_t` and block on `rover_action_result_queue` until the task executes and acknowledges. During `rover_move` and `rover_turn`, the task re-sends speed every 50ms (RoverC Pro requires keepalive) and checks the emergency stop flag on each tick.

**I2C register map (addr 0x38):**
- `0x00–0x03`: Motor 1-4 speed (int8, −127..127)
- `0x10–0x11`: Servo 1-2 angle (uint8, 0-180°)
- `0x20–0x23`: Servo 1-2 pulse width (uint16 big-endian, 500-2500 µs)

**Mecanum kinematics** (same as ai-rover):
`m0 = y+x−z`, `m1 = y−x+z`, `m2 = y−x−z`, `m3 = y+x+z`, all clamped to [−100, 100].
z is negated before mixing to match hardware motor layout.

### 2.2 cap_unitv

**Path:** `components/claw_capabilities/cap_unitv/`

```
CMakeLists.txt
include/cap_unitv.h
src/
  cap_unitv.c           — group registration, execute dispatcher
  cap_unitv_uart.c      — UART init/send/recv, timeout, UnitV protocol
  cap_unitv_capture.c   — JPEG assembly, HTTPS call to vision LLM
  cmd_cap_unitv.c       — CLI: unitv scan/capture
skills/
  cap_unitv.md
  skills_list.json
```

**Capabilities:**

| ID | Parameters | Action |
|----|------------|--------|
| `unitv_scan` | mode (fast/reliable) | SCAN command → JSON with objects, faces |
| `unitv_capture` | question (string), quality (30-95) | CAPTURE → JPEG → vision LLM → analysis JSON |

**Vision LLM config** — initialized from the same credentials as `claw_core`:
```c
cap_unitv_set_vision_config(&(cap_unitv_vision_config_t){
    .api_key      = settings->llm_api_key,
    .model        = settings->llm_model,
    .base_url     = settings->llm_base_url,
    .backend_type = settings->llm_backend_type,
    .auth_type    = settings->llm_auth_type,
    .timeout_ms   = (uint32_t)strtoul(settings->llm_timeout_ms, NULL, 10),
});
```

If the configured model does not support images, `unitv_capture` returns `{"status":"vision_not_supported"}`. JPEG buffer max 40KB (K210 limit), heap-allocated and freed after each call.

**Availability tracking:** first successful UART response sets `s_unitv_available = true` and emits a log event. Timeout returns `{"status":"camera_unavailable"}` without crashing.

---

## 3. Application Structure

```
application/rover_demo/
  CMakeLists.txt
  sdkconfig.defaults
  partitions.csv
  idf_component.yml
  boards/
    m5stickc_plus/
      board_info.yaml
      board_peripherals.yaml
      board_devices.yaml
      sdkconfig.defaults.board
      setup_device.cpp          — M5.begin(), exports C API for display/IMU/battery
  fatfs_image/
    memory/
      identity.md
      soul.md
      user.md
    router_rules/
      router_rules.json
    skills/
      skills_list.json
      rover_ops.md
      rover_search.md
  main/
    CMakeLists.txt
    idf_component.yml
    Kconfig.projbuild
    main.c                      — NVS, FATFS, WiFi, app_rover_start()
    rover_demo_settings.h/.c
    rover_demo_wifi.h/.c
    rover_display.h/.cpp        — M5Unified display: status, battery, IP
    rover_buttons.h/.c          — BtnA/BtnB interrupts, emergency stop flag
    app_rover.h/.c              — wires all modules (analogous to app_claw.c)
```

### Flash partition layout (4MB)

```
nvs,      data, nvs,   0x9000,   0x4000
otadata,  data, ota,   0xd000,   0x2000
app0,     app,  ota_0, 0xf000,   0x180000   # 1.5MB app
storage,  data, fat,   0x1f0000, 0x200000   # 2MB FATFS
```

### Key sdkconfig.defaults settings

```
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y
CONFIG_SPIRAM_SUPPORT=n
CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_DYNAMIC_FREE_PEER_CERT=y
CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA=y
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

`MBEDTLS_DYNAMIC_*` flags (carried from ai-rover) save ~30KB heap during TLS handshake.

### Top-level idf_component.yml

```yaml
dependencies:
  m5stack/m5unified: ">=0.2.0"
  espressif/mdns: ">=1.2.0"
  idf: ">=5.3.0"
```

M5Unified pulls M5GFX automatically.

### M5Unified C wrapper (setup_device.cpp)

`setup_device.cpp` calls `M5.begin()` and exports a C API used by the rest of the (pure C) codebase:

```c
void  rover_board_init(void);
int   rover_board_get_battery_pct(void);
void  rover_board_get_imu(float *ax, float *ay, float *az,
                          float *gx, float *gy, float *gz);
bool  rover_board_imu_enabled(void);
void  rover_board_m5_update(void);   // M5.update() — called each loop tick
```

Display rendering lives in `rover_display.cpp` (also C++ for M5GFX access).

`rover_board_m5_update()` is called from `rover_buttons_task` — a lightweight FreeRTOS task (2KB stack) that runs every 20ms, calls `M5.update()` to refresh button state, checks `M5.BtnA` / `M5.BtnB`, and fires the appropriate actions (demo event injection or sleep).

---

## 4. Memory Strategy

**Mode:** lightweight only (`CONFIG_ROVER_DEMO_MEMORY_MODE_FULL=n`).

Context providers registered with claw_core (in order):
1. `claw_memory_profile_provider` — identity.md + soul.md + user.md
2. `claw_memory_long_term_lightweight_provider` — full MEMORY.md as plain text
3. `claw_memory_session_history_provider` — last 20 messages
4. `claw_skill_skills_list_provider` — skills catalog
5. `claw_skill_active_skill_docs_provider` — active skill documents
6. `claw_cap_tools_provider` — LLM-visible capability tool schemas

**Rough RAM budget (ESP32-PICO-D4, 520KB SRAM):**

| Component | ~KB |
|-----------|-----|
| FreeRTOS + WiFi + TCP/IP stack | 200 |
| TLS buffers (dynamic, peak during handshake) | 32 |
| claw_core task (16KB stack) | 16 |
| cap_im_tg task (8KB stack) | 8 |
| rover_hw_task (4KB stack) | 4 |
| M5Unified + M5GFX (display framebuffer) | 20 |
| LLM response + cJSON buffers | 20 |
| FATFS + NVS | 8 |
| UART Rx buffer (cap_unitv) | 4 |
| JPEG buffer (cap_unitv, peak, 40KB) | 40 |
| **Total peak estimate** | **~352** |
| **Headroom** | **~168** |

JPEG and TLS peaks overlap during `unitv_capture`: the 40KB JPEG buffer is held in memory while the HTTPS call is in flight (TLS active). Both are already counted in the total above — the ~168KB headroom is valid even under simultaneous peak load.

---

## 5. System Prompt and Skills

### System prompt

```
"You are AI Rover — a mecanum robot with a gripper and a fixed front-facing camera. "
"Answer briefly in the user's language. "
"Use tools directly when the user gives a command — do not ask permission first. "
"Use rover_move() for timed movement, rover_turn() for precise rotation (uses IMU). "
"Use unitv_scan() for quick object detection. "
"Use unitv_capture(question=...) for detailed scene analysis. "
"Camera is fixed — to change the view, call rover_turn() or rover_move(). "
"For multi-step tasks, activate rover_ops or rover_search skill first."
```

### Skills

**`rover_ops.md`** — detailed movement instructions: combining `rover_move` + `rover_turn` for trajectories, max 5 seconds per `rover_move`, how to handle `emergency_stop` error in a tool response.

**`rover_search.md`** — object search pattern: 360° sweep via repeated `rover_turn(direction, 90°)` + `unitv_scan()`, stop when target detected, confirm with `unitv_capture`.

### identity.md (starter content)

```markdown
Ты AI Rover. Ты физический робот на колёсах Mecanum с захватом и камерой.
Ты находишься в реальном мире. Твои действия имеют физические последствия.
Будь точен в командах движения.
```

### router_rules.json (starter content)

```json
[{
  "id": "default_to_agent",
  "enabled": true,
  "match": {"event_type": "message"},
  "actions": [{"kind": "run_agent"}]
}]
```

---

## 6. Display, Buttons, and Error Handling

### Display (135×240 landscape via M5Unified)

Three-row layout, updates on state change only (dirty flag):

```
┌─────────────────────────┐
│  AI Rover               │  ← header bar (color-coded by state)
│                         │
│      AI_THINK           │  ← current FSM state (large text)
│                         │
│  192.168.1.42  87%      │  ← WiFi IP + battery (small text)
└─────────────────────────┘
```

State → header color: IDLE=blue, AI_THINKING/AI_EXECUTING=orange, OFFLINE=red, SLEEPING=purple.

### Buttons

**BtnB (GPIO39) — emergency stop:**
GPIO interrupt (NEGEDGE) → ISR sets `s_emergency_requested = true` (atomic).
`rover_hw_task` checks flag every 50ms during motion → zeroes all motors → returns `ESP_ERR_INVALID_STATE` to result queue → cap_rover returns `{"status":"emergency_stop"}` to LLM → agent notifies user in Telegram.

**BtnA (GPIO37):**
- Short press: injects synthetic `message` event (`"demo"`) into event router → agent handles it like a Telegram command.
- Long press (>3s): enters deep sleep.

### Deep sleep

After `kInactivityTimeoutMs` (120s) without activity:
1. Zero all motors via I2C
2. Disconnect WiFi, free mDNS
3. `M5.Display.setBrightness(0)` + `M5.Display.sleep()`
4. `esp_sleep_enable_ext0_wakeup(GPIO37, 0)` — BtnA wakes
5. `esp_sleep_enable_ext1_wakeup(1ULL<<GPIO39, ALL_LOW)` — BtnB wakes
6. `esp_sleep_pd_config(RTC_PERIPH, ON)` — keep RTC peripherals for reliable wake
7. `esp_deep_sleep_start()`

### Offline fallback

WiFi disconnect → FSM → `STATE_OFFLINE`. cap_im_tg continues reconnect attempts (exponential backoff). claw_core keeps running. Incoming events buffer in event router queue. Processing resumes after reconnection.

### Initialization order (app_main)

```
NVS init
→ rover_demo_settings_load
→ rover_board_init()          (M5.begin(), display, IMU, power)
→ FATFS mount
→ rover_demo_wifi_start
→ claw_event_router_init
→ claw_memory_init            (lightweight)
→ claw_skill_init
→ cap_rover_init              (I2C + rover_hw_task)
→ cap_unitv_init              (UART1)
→ claw_cap_init
→ register all cap groups
→ claw_cap_set_llm_visible_groups
→ claw_cap_start_all
→ claw_core_init
→ add context providers
→ claw_core_start
→ claw_event_router_start
→ cap_time_sync_start
→ rover_buttons_init          (GPIO interrupts)
```

---

## Out of Scope (v1)

- Lua scripting
- QQ / Feishu / WeChat
- MCP client/server
- Full memory mode (async extraction, recall)
- Web chat interface
- cap_scheduler
- OTA updates
