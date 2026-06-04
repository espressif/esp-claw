# Wave Rover MCP Firmware Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a PlatformIO ESP-IDF firmware for Waveshare WAVE ROVER SKU 25377 that exposes rover hardware as an on-device MCP server over Wi-Fi.

**Architecture:** New standalone application `application/wave_rover/` using `espressif/mcp-c-sdk` for tool management; a thin custom HTTP handler wraps the SDK to add `resources/list` and `resources/read` on the same `/mcp` endpoint (port 8080). Hardware access goes through `wave_rover_hal` component with dry-run and real backends. Wi-Fi config stored in NVS via `wave_rover_config`.

**Tech Stack:** ESP-IDF 5.5 + PlatformIO `espidf` framework, ESP32-WROOM-32 target (`esp32dev` board), `espressif/mcp-c-sdk ^1.0.0`, `espressif/mdns`, cJSON (bundled with IDF), `esp_http_server` (IDF built-in).

---

## Hardware Reference (confirmed from ugv_base_general source)

| Subsystem | Details |
|---|---|
| MCU | ESP32-WROOM-32 (`esp32dev` PlatformIO board) |
| Motor A left | PWMA=GPIO25, AIN1=GPIO21, AIN2=GPIO17, LEDC ch5 |
| Motor B right | PWMB=GPIO26, BIN1=GPIO22, BIN2=GPIO23, LEDC ch6 |
| PWM config | 100 kHz, 8-bit (0–255) |
| Encoders | AENCA=35, AENCB=34, BENCA=27, BENCB=16 (stubs only in MVP) |
| I2C bus | SDA=GPIO32, SCL=GPIO33 |
| INA219 | I2C addr=0x42, shunt=0.01 Ω, bus range 16 V, pgain ±320 mV |
| OLED | SSD1306 128×32 px, I2C addr=0x3C |
| IMU accel/gyro | QMI8658, I2C addr=0x6B |
| IMU magnetometer | AK09918, I2C addr=0x0C |
| Default Wi-Fi AP | SSID `WR-ESP32`, password `12345678` (reference firmware) |

## MCP SDK Constraints and API Notes

`espressif/mcp-c-sdk v1.x` supports: `initialize`, `ping`, `tools/list`, `tools/call`.
It does **NOT** support `resources` or `prompts`. Workaround in this firmware:
- Use a **null transport** (custom no-op function table) with `esp_mcp_mgr_init` so the engine initialises without trying to start an HTTP server.
- `esp_mcp_mgr_register_endpoint` adds the endpoint to the internal list; with a null transport its `register_endpoint` is a no-op.
- A custom `esp_http_server` POST handler at `/mcp` (port 8080) pre-checks the JSON-RPC `method`:
  - `resources/list` or `resources/read` → handled inline.
  - All other methods → delegated to `esp_mcp_mgr_req_handle(mgr, "mcp", ...)`.

**Verified SDK API (from `esp_mcp_property.h`, `esp_mcp_data.h`):**
- `esp_mcp_property_create_with_range(name, int_min, int_max)` creates an **INTEGER** range — not float.
- For float parameters (linear, angular, speed, left, right): use `esp_mcp_property_create_with_float(name, 0.0f)`.
- For integer parameters (duration_ms, line): use `esp_mcp_property_create_with_range(name, min, max)`.
- For bool parameters: use `esp_mcp_property_create_with_bool(name, false)`.
- For string parameters: use `esp_mcp_property_create_with_string(name, "")`.
- Return values: `esp_mcp_value_create_string(json_str)` for JSON responses.
- CMakeLists REQUIRES name: `espressif__mcp-c-sdk` (double underscore + hyphen).

---

## File Map

```
application/wave_rover/
  CMakeLists.txt
  platformio.ini
  partitions.csv
  sdkconfig.defaults
  idf_component.yml                      ← managed deps (mcp-c-sdk, mdns)
  main/
    CMakeLists.txt
    idf_component.yml                    ← local component deps
    app_main.c
  components/
    wave_rover_config/
      CMakeLists.txt
      include/wave_rover_config.h
      wave_rover_config.c
    wave_rover_hal/
      CMakeLists.txt
      include/wave_rover_hal.h           ← unified public API
      wave_rover_hal.c                   ← init, i2c bus
      wave_rover_motor.c                 ← LEDC PWM motor control
      wave_rover_power.c                 ← INA219
      wave_rover_imu.c                   ← QMI8658 + AK09918
      wave_rover_display.c               ← SSD1306 OLED
    wave_rover_mcp/
      CMakeLists.txt
      include/wave_rover_mcp.h
      wave_rover_mcp.c                   ← HTTP server, routing, resources
      wave_rover_mcp_tools.c             ← all tool callbacks
  boards/
    wave_rover/
      board_config.h                     ← GPIO defines, I2C addrs, build-time flags

docs/
  references.md
  reference_firmware_analysis.md
  platformio.md
  quickstart.md
  mcp_api.md
  safety.md
  protocol_compatibility.md
  troubleshooting.md

tools/
  mcp_smoke_test.py
  rover_cli.py
```

---

## Task 1 — Research docs: `docs/references.md`

**Files:**
- Create: `docs/references.md`

- [ ] **Step 1: Create the file**

```markdown
# Wave Rover MCP — Reference Sources

| Source | URL | Type | What was found | Relevant files/sections | Confidence |
|---|---|---|---|---|---|
| ESP-Claw repo | https://github.com/espressif/esp-claw | GitHub | rover_demo app pattern, cap_rover I2C driver, cap_mcp_server cap, PlatformIO configs in rover_demo/rover_s3 | application/rover_demo/, components/claw_capabilities/cap_rover/ | High |
| ESP-Claw tutorial | https://esp-claw.com/en/tutorial/ | Docs | Architecture, MCP server capability, capability registration | Tutorial pages | Medium (403 at plan time) |
| WAVE ROVER product | https://www.waveshare.com/wave-rover.htm | Product | SKU 25377, ESP32 board, 4WD N20 motors, INA219 UPS, OLED, 9-axis IMU, TF slot | Product page | High (403, confirmed via wiki) |
| WAVE ROVER English wiki | https://www.waveshare.com/wiki/WAVE_ROVER | Wiki | HTTP/serial/USB/ESP-NOW transports, JSON commands, Wi-Fi AP default, OLED boot | Wiki page | High (403, confirmed via ugv_base) |
| WAVE ROVER Chinese wiki | https://www.waveshare.net/wiki/WAVE_ROVER | Wiki | Returned 403; content inferred from ugv_base_general source | — | Medium |
| General Driver for Robots | https://www.waveshare.com/wiki/General_Driver_for_Robots | Wiki | ESP32-WROOM-32 MCU, motor GPIO, I2C pins, INA219 addr=0x42, OLED SSD1306 addr=0x3C, IMU QMI8658+AK09918 | ugv_config.h, oled_ctrl.h, battery_ctrl.h, IMU.h | High (via GitHub source) |
| ugv_base_general | https://github.com/waveshareteam/ugv_base_general | GitHub | Motor GPIO map, INA219, OLED, QMI8658+AK09918, JSON command protocol, heartbeat | General_Driver/*.h | High |
| ugv_base_ros | https://github.com/waveshareteam/ugv_base_ros | GitHub | ROS variant; CMD_ROS_CTRL T=13 (m/s, rad/s); serial transport; same GPIO base | README, source | Medium |
| ugv_rpi | https://github.com/waveshareteam/ugv_rpi | GitHub | Python upper-computer; JSON commands T=1 (speed), T=126 (IMU), T=127 (calibrate); heartbeat 3 s | Python client | High |
| ugv_rdk | https://github.com/waveshareteam/ugv_rdk | GitHub | RDK upper-computer; JSON-over-UART; same command set as ugv_rpi | Source | Medium |
| wave_rover_serial | https://github.com/msanterre/wave_rover_serial | GitHub | Python wrapper; serial command quirks; speed [-1,1] to motor | Python | Medium |
| waveshare-rover-development-setup | https://github.com/winetree94/waveshare-rover-development-setup | GitHub | Arduino IDE setup notes; no PlatformIO config found | README | Low |
| espressif/mcp-c-sdk | https://github.com/espressif/esp-iot-solution (component) | Component | SDK for on-device MCP server; tools only; resources/prompts not supported v1.x | include/*.h, src/ | High |
```

- [ ] **Step 2: Commit**

```bash
git add docs/references.md
git commit -m "docs: add wave rover reference sources table"
```

---

## Task 2 — Research docs: `docs/reference_firmware_analysis.md`

**Files:**
- Create: `docs/reference_firmware_analysis.md`

- [ ] **Step 1: Create the file**

```markdown
# Wave Rover Reference Firmware Analysis

## 1. Hardware confirmed for SKU 25377

- 4WD chassis with 4× N20 geared motors (2 per driver side)
- ESP32-WROOM-32 multi-function driver board
- Integrated 3S 18650 UPS module (INA219 monitor)
- SSD1306 OLED 128×32
- QMI8658 (accel+gyro) + AK09918 (mag) 9-axis IMU
- TF/SD card slot (not used in MVP)
- Serial bus servo interface (not used in MVP)

## 2. Controller board model

General Driver for Robots (Waveshare). ESP32-WROOM-32 module.

## 3. ESP32 chip/board target

ESP32-WROOM-32. PlatformIO board: `esp32dev`. Framework: `espidf`.

## 4. Motor driver and GPIO/PWM mapping

| Signal | GPIO | Role |
|---|---|---|
| PWMA | 25 | Motor A (left) PWM |
| AIN1 | 21 | Motor A direction 1 |
| AIN2 | 17 | Motor A direction 2 |
| PWMB | 26 | Motor B (right) PWM |
| BIN1 | 22 | Motor B direction 1 |
| BIN2 | 23 | Motor B direction 2 |
| AENCA/B | 35/34 | Encoder A (left, stubs) |
| BENCA/B | 27/16 | Encoder B (right, stubs) |

LEDC channel A=5, channel B=6. Frequency: 100 kHz. Resolution: 8-bit (0–255).

WAVE ROVER uses direct PWM (no PID). Speed [-1.0, 1.0] → PWM [0, 255]:
- Positive speed: AIN1=HIGH, AIN2=LOW, ledcWrite(ch, round(spd*255))
- Negative speed: AIN1=LOW, AIN2=HIGH, ledcWrite(ch, round(-spd*255))
- Zero: AIN1=LOW, AIN2=LOW, ledcWrite(ch, 0)

## 5. UPS/INA219 wiring and I2C address

I2C bus: SDA=GPIO32, SCL=GPIO33. INA219 address: **0x42**.
Shunt: 0.01 Ω. Bus range: 16 V. PGain: ±320 mV.
Reads: bus voltage (V), shunt voltage (mV), current (mA), power (mW).
3S LiPo: nominal 11.1 V (range 9–12.6 V).

## 6. OLED controller and I2C address

SSD1306, 128×32 px. I2C address: **0x3C**. Same I2C bus (SDA=32, SCL=33).
ESP-IDF driver: use `i2c_master_transmit` with SSD1306 command protocol.
4 text lines at 8px font height.

## 7. IMU chip and I2C address

QMI8658: accel+gyro, I2C address **0x6B**.
AK09918: magnetometer, I2C address **0x0C**.
Both on same I2C bus. Init via register writes (WHO_AM_I check, CTRL7 enable).

## 8. JSON command protocol (ugv_base_general)

| Cmd | T value | Format | Notes |
|---|---|---|---|
| Speed ctrl | 1 | `{"T":1,"L":<float>,"R":<float>}` | L/R in [-1,1] |
| PWM input | 11 | `{"T":11,"L":<int>,"R":<int>}` | L/R in [-255,255] |
| ROS ctrl | 13 | `{"T":13,"X":<m/s>,"Z":<rad/s>}` | linear+angular |
| OLED ctrl | 3 | `{"T":3,"lineNum":<0-3>,"Text":"..."}` | |
| Get IMU | 126 | `{"T":126}` | returns accel/gyro/mag |
| Calibrate IMU | 127 | `{"T":127}` | 5-second still calibration |
| Heartbeat stop | — | No cmd for 3000ms | Motors stop automatically |

## 9. Serial/USB/HTTP/ESP-NOW transports

Reference firmware accepts JSON commands on:
- UART0 (Serial, 115200 baud)
- USB-Serial (same as UART0)
- HTTP POST (Wi-Fi AP or STA mode)
- ESP-NOW broadcast (MAC FF:FF:FF:FF:FF:FF)

## 10. Existing build system

Reference firmware: Arduino IDE. Libraries: ArduinoJson, Adafruit_SSD1306, INA219_WE, ESP32Encoder.
Our firmware: ESP-IDF 5.5 via PlatformIO `espidf` framework. Native I2C master driver.

## 11. What can be reused directly

- GPIO pin definitions (direct from ugv_config.h)
- Motor direction logic (direct translation from switchPortCtrlA/B)
- INA219 register protocol (translated from INA219_WE to raw I2C)
- SSD1306 command sequence (translatable from Adafruit_SSD1306)
- QMI8658 register map (from QMI8658.h, direct translation)
- JSON command protocol reference (for MCP tool parameter alignment)

## 12. What must be wrapped behind MCP

All hardware access. MCP tools are the only public interface.

## 13. What is still unknown

- Exact 3S battery voltage thresholds for "low battery" flag (assumed <10.5 V).
- QMI8658 full initialization sequence (power-on defaults vs explicit config).
- Whether AK09918 requires QMI8658 pass-through mode or is directly accessible.
- Exact Wi-Fi AP default password (assumed `12345678` from community sources).
- Exact flash partition layout for wave rover (using rover_demo's partitions.csv as reference).
```

- [ ] **Step 2: Commit**

```bash
git add docs/reference_firmware_analysis.md
git commit -m "docs: add wave rover hardware and protocol analysis"
```

---

## Task 3 — PlatformIO skeleton: project files

**Files:**
- Create: `application/wave_rover/CMakeLists.txt`
- Create: `application/wave_rover/platformio.ini`
- Create: `application/wave_rover/partitions.csv`
- Create: `application/wave_rover/sdkconfig.defaults`
- Create: `application/wave_rover/idf_component.yml`
- Create: `application/wave_rover/main/CMakeLists.txt`
- Create: `application/wave_rover/main/idf_component.yml`

- [ ] **Step 1: Create `application/wave_rover/CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wave_rover)
```

- [ ] **Step 2: Create `application/wave_rover/platformio.ini`**

```ini
[platformio]
default_envs = wave_rover

[env:wave_rover]
platform = espressif32
board = esp32dev
framework = espidf
monitor_speed = 115200
upload_speed = 921600
board_build.flash_mode = dio
board_build.f_flash = 40000000L
board_upload.flash_size = 4MB
board_build.partitions = partitions.csv
extra_scripts =
    pre:scripts/pio_fatfs.py

build_flags =
    -DCORE_DEBUG_LEVEL=3

monitor_filters =
    esp32_exception_decoder
    time
```

Note: `pio_fatfs.py` is copied from `application/rover_demo/scripts/` in Task 4.

- [ ] **Step 3: Create `application/wave_rover/partitions.csv`**

Copy from `application/rover_demo/partitions.csv`. If that file doesn't exist, use:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x5000,
otadata,  data, ota,     0xe000,  0x2000,
app0,     app,  ota_0,   0x10000, 0x1F0000,
spiffs,   data, spiffs,  0x200000,0x200000,
```

- [ ] **Step 4: Create `application/wave_rover/sdkconfig.defaults`**

```
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_LWIP_LOCAL_HOSTNAME="wave-rover"
CONFIG_MDNS_HOSTNAME="wave-rover"
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=32
CONFIG_ESP_WIFI_TX_BUFFER_TYPE_DYNAMIC=y
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
```

- [ ] **Step 5: Create `application/wave_rover/idf_component.yml`** (managed components at project root)

```yaml
dependencies:
  idf: ">=5.5.0"
  espressif/mcp-c-sdk: "^1.0.0"
  espressif/mdns: "^1.10.1"
```

- [ ] **Step 6: Create `application/wave_rover/main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        wave_rover_config
        wave_rover_hal
        wave_rover_mcp
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_log
)
```

- [ ] **Step 7: Create `application/wave_rover/main/idf_component.yml`**

```yaml
dependencies:
  wave_rover_config:
    path: ../components/wave_rover_config
  wave_rover_hal:
    path: ../components/wave_rover_hal
  wave_rover_mcp:
    path: ../components/wave_rover_mcp
```

- [ ] **Step 8: Commit**

```bash
git add application/wave_rover/CMakeLists.txt application/wave_rover/platformio.ini \
        application/wave_rover/partitions.csv application/wave_rover/sdkconfig.defaults \
        application/wave_rover/idf_component.yml application/wave_rover/main/
git commit -m "feat(wave_rover): add PlatformIO project skeleton"
```

---

## Task 4 — Minimal app_main + scripts

**Files:**
- Create: `application/wave_rover/main/app_main.c`
- Create: `application/wave_rover/scripts/pio_fatfs.py` (copy from rover_demo)

- [ ] **Step 1: Copy pio_fatfs.py script**

```bash
mkdir -p application/wave_rover/scripts
cp application/rover_demo/scripts/pio_fatfs.py application/wave_rover/scripts/
```

- [ ] **Step 2: Create `application/wave_rover/main/app_main.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wave_rover";

void app_main(void)
{
    ESP_LOGI(TAG, "Wave Rover MCP firmware v0.1.0 starting");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "NVS initialized");
    /* subsequent init in later tasks */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
```

- [ ] **Step 3: Create stub component directories so CMake doesn't error**

```bash
mkdir -p application/wave_rover/components/wave_rover_config/include
mkdir -p application/wave_rover/components/wave_rover_hal/include
mkdir -p application/wave_rover/components/wave_rover_mcp/include
```

Create minimal `CMakeLists.txt` for each stub:

`application/wave_rover/components/wave_rover_config/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "" INCLUDE_DIRS "include")
```

`application/wave_rover/components/wave_rover_hal/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "" INCLUDE_DIRS "include")
```

`application/wave_rover/components/wave_rover_mcp/CMakeLists.txt`:
```cmake
idf_component_register(SRCS "" INCLUDE_DIRS "include")
```

Create empty headers:

`application/wave_rover/components/wave_rover_config/include/wave_rover_config.h`:
```c
#pragma once
```

`application/wave_rover/components/wave_rover_hal/include/wave_rover_hal.h`:
```c
#pragma once
```

`application/wave_rover/components/wave_rover_mcp/include/wave_rover_mcp.h`:
```c
#pragma once
```

- [ ] **Step 4: Verify build**

```bash
cd application/wave_rover
. $IDF_PATH/export.sh
idf.py set-target esp32
# OR via PlatformIO:
# cd application/wave_rover && ~/.local/bin/pio run
```

Expected: Build succeeds. No firmware functionality yet.

- [ ] **Step 5: Commit**

```bash
git add application/wave_rover/main/app_main.c application/wave_rover/scripts/ \
        application/wave_rover/components/
git commit -m "feat(wave_rover): add minimal app_main and component stubs"
```

---

## Task 5 — Config component (`wave_rover_config`)

**Files:**
- Modify: `application/wave_rover/components/wave_rover_config/CMakeLists.txt`
- Create: `application/wave_rover/components/wave_rover_config/include/wave_rover_config.h`
- Create: `application/wave_rover/components/wave_rover_config/wave_rover_config.c`

- [ ] **Step 1: Update CMakeLists.txt**

```cmake
idf_component_register(
    SRCS "wave_rover_config.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES nvs_flash esp_log
)
```

- [ ] **Step 2: Write `include/wave_rover_config.h`**

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

#define WR_CFG_SSID_LEN      64
#define WR_CFG_PASS_LEN      64
#define WR_CFG_HOST_LEN      32
#define WR_CFG_TOKEN_LEN     64

typedef struct {
    char     wifi_ssid[WR_CFG_SSID_LEN];
    char     wifi_password[WR_CFG_PASS_LEN];
    char     wifi_ap_ssid[WR_CFG_SSID_LEN];   /* AP mode SSID */
    char     wifi_ap_password[WR_CFG_PASS_LEN];
    uint8_t  wifi_mode;                         /* 0=ap, 1=sta, 2=ap_sta */
    char     hostname[WR_CFG_HOST_LEN];
    uint16_t mcp_port;
    bool     auth_enabled;
    char     auth_token[WR_CFG_TOKEN_LEN];
    bool     safe_mode;
    bool     dry_run;                           /* true=no real hardware */
    float    max_speed;                         /* [0.0, 1.0] */
    uint16_t max_command_duration_ms;
} wave_rover_config_t;

esp_err_t wave_rover_config_init(void);
esp_err_t wave_rover_config_load(wave_rover_config_t *cfg);
esp_err_t wave_rover_config_save(const wave_rover_config_t *cfg);
void      wave_rover_config_defaults(wave_rover_config_t *cfg);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Write `wave_rover_config.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "wr_config";
#define NVS_NS "wr_cfg"

void wave_rover_config_defaults(wave_rover_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strlcpy(cfg->wifi_ap_ssid, "WR-ESP32", sizeof(cfg->wifi_ap_ssid));
    strlcpy(cfg->wifi_ap_password, "12345678", sizeof(cfg->wifi_ap_password));
    strlcpy(cfg->hostname, "wave-rover", sizeof(cfg->hostname));
    cfg->wifi_mode              = 0; /* AP */
    cfg->mcp_port               = 8080;
    cfg->auth_enabled           = false;
    cfg->safe_mode              = false;
    cfg->dry_run                = true; /* safe default until HW confirmed */
    cfg->max_speed              = 0.4f;
    cfg->max_command_duration_ms = 3000;
}

esp_err_t wave_rover_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t wave_rover_config_load(wave_rover_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    wave_rover_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved config, using defaults");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_open");

    size_t sz = sizeof(*cfg);
    err = nvs_get_blob(h, "cfg", cfg, &sz);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        wave_rover_config_defaults(cfg);
        err = ESP_OK;
    }
    nvs_close(h);
    /* Never log password fields */
    ESP_LOGI(TAG, "config loaded: wifi_mode=%u mcp_port=%u dry_run=%d",
             cfg->wifi_mode, cfg->mcp_port, cfg->dry_run);
    return err;
}

esp_err_t wave_rover_config_save(const wave_rover_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_blob(h, "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
```

- [ ] **Step 4: Verify build**

Run `idf.py build` (or `pio run`) from `application/wave_rover/`. Expected: success.

- [ ] **Step 5: Commit**

```bash
git add application/wave_rover/components/wave_rover_config/
git commit -m "feat(wave_rover): add NVS config component"
```

---

## Task 6 — Wi-Fi init in app_main

**Files:**
- Modify: `application/wave_rover/main/app_main.c`
- Create: `application/wave_rover/main/wr_wifi.h`
- Create: `application/wave_rover/main/wr_wifi.c`
- Modify: `application/wave_rover/main/CMakeLists.txt`

- [ ] **Step 1: Create `main/wr_wifi.h`**

```c
#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "wave_rover_config.h"

esp_err_t wr_wifi_init(const wave_rover_config_t *cfg);
bool      wr_wifi_is_connected(void);
const char *wr_wifi_get_ip(void);
```

- [ ] **Step 2: Create `main/wr_wifi.c`**

```c
#include "wr_wifi.h"
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"

static const char *TAG = "wr_wifi";
static EventGroupHandle_t s_wifi_eg;
static char s_ip[20] = {0};
static bool s_connected = false;

#define WIFI_CONNECTED_BIT BIT0

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "STA connected, IP=%s", s_ip);
    }
}

esp_err_t wr_wifi_init(const wave_rover_config_t *cfg)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    if (cfg->wifi_mode == 0 || cfg->wifi_mode == 2) {
        /* AP mode */
        esp_netif_create_default_wifi_ap();
    }
    if (cfg->wifi_mode == 1 || cfg->wifi_mode == 2) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "ev reg");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "ip ev");

    if (cfg->wifi_mode == 0) {
        /* AP only */
        wifi_config_t ap = {.ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
        }};
        strlcpy((char *)ap.ap.ssid, cfg->wifi_ap_ssid, sizeof(ap.ap.ssid));
        strlcpy((char *)ap.ap.password, cfg->wifi_ap_password, sizeof(ap.ap.password));
        ap.ap.ssid_len = (uint8_t)strlen((char *)ap.ap.ssid);
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    } else if (cfg->wifi_mode == 1) {
        /* STA only */
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, cfg->wifi_password, sizeof(sta.sta.password));
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "sta config");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    if (cfg->wifi_mode == 1) {
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect");
        ESP_LOGI(TAG, "connecting to STA SSID '%s'...", cfg->wifi_ssid);
        xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
        if (!s_connected) {
            ESP_LOGW(TAG, "STA connect timeout, continuing offline");
        }
    } else {
        ESP_LOGI(TAG, "AP started: SSID=%s", cfg->wifi_ap_ssid);
    }
    return ESP_OK;
}

bool wr_wifi_is_connected(void) { return s_connected; }
const char *wr_wifi_get_ip(void) { return s_ip; }
```

- [ ] **Step 3: Update `main/CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "app_main.c" "wr_wifi.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        wave_rover_config
        wave_rover_hal
        wave_rover_mcp
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_log
)
```

- [ ] **Step 4: Update `app_main.c`**

```c
#include "esp_log.h"
#include "nvs_flash.h"
#include "wave_rover_config.h"
#include "wr_wifi.h"

static const char *TAG = "wave_rover";
static wave_rover_config_t s_cfg;

void app_main(void)
{
    ESP_LOGI(TAG, "Wave Rover MCP firmware v0.1.0 starting");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wave_rover_config_load(&s_cfg));
    ESP_ERROR_CHECK(wr_wifi_init(&s_cfg));
    ESP_LOGI(TAG, "init complete. dry_run=%d", s_cfg.dry_run);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
```

- [ ] **Step 5: Build and verify**

```bash
cd application/wave_rover && idf.py build
```

Expected output (last lines):
```
...
[100%] Linking CXX executable wave_rover.elf
...
Project build complete.
```

- [ ] **Step 6: Commit**

```bash
git add application/wave_rover/main/
git commit -m "feat(wave_rover): add Wi-Fi init (AP+STA)"
```

---

## Task 7 — HAL stubs (`wave_rover_hal`)

**Files:**
- Modify: `application/wave_rover/components/wave_rover_hal/CMakeLists.txt`
- Modify: `application/wave_rover/components/wave_rover_hal/include/wave_rover_hal.h`
- Create: `application/wave_rover/components/wave_rover_hal/wave_rover_hal.c`
- Create: `application/wave_rover/components/wave_rover_hal/wave_rover_motor.c`
- Create: `application/wave_rover/components/wave_rover_hal/wave_rover_power.c`
- Create: `application/wave_rover/components/wave_rover_hal/wave_rover_imu.c`
- Create: `application/wave_rover/components/wave_rover_hal/wave_rover_display.c`
- Create: `application/wave_rover/boards/wave_rover/board_config.h`

- [ ] **Step 1: Create `boards/wave_rover/board_config.h`**

```c
#pragma once
/* WAVE ROVER GPIO and hardware constants (confirmed from ugv_base_general) */

/* Motor A (left) */
#define WR_MOTOR_PWMA    25
#define WR_MOTOR_AIN1    21
#define WR_MOTOR_AIN2    17
#define WR_LEDC_CH_A      5

/* Motor B (right) */
#define WR_MOTOR_PWMB    26
#define WR_MOTOR_BIN1    22
#define WR_MOTOR_BIN2    23
#define WR_LEDC_CH_B      6

/* LEDC config */
#define WR_LEDC_FREQ_HZ  100000
#define WR_LEDC_BITS      LEDC_TIMER_8_BIT   /* 0-255 */
#define WR_LEDC_TIMER     LEDC_TIMER_0
#define WR_LEDC_SPEED     LEDC_LOW_SPEED_MODE

/* I2C bus */
#define WR_I2C_PORT      I2C_NUM_0
#define WR_I2C_SDA       32
#define WR_I2C_SCL       33
#define WR_I2C_FREQ_HZ   100000

/* INA219 */
#define WR_INA219_ADDR   0x42

/* OLED SSD1306 */
#define WR_OLED_ADDR     0x3C
#define WR_OLED_WIDTH    128
#define WR_OLED_HEIGHT    32

/* IMU */
#define WR_QMI8658_ADDR  0x6B
#define WR_AK09918_ADDR  0x0C

/* Safety */
#define WR_LOW_BATT_V    10.5f  /* 3S LiPo approx 3.5V/cell */
```

- [ ] **Step 2: Write `include/wave_rover_hal.h`**

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Motor state */
typedef struct {
    float left;   /* [-1.0, 1.0] */
    float right;
    bool  emergency_stop;
} wr_motor_state_t;

/* Power status */
typedef struct {
    bool  present;
    float bus_voltage_v;
    float shunt_voltage_mv;
    float current_ma;
    float power_mw;
    float load_voltage_v;
    bool  charging;
    bool  low_battery;
} wr_power_status_t;

/* IMU sample */
typedef struct {
    bool  present;
    float accel_x, accel_y, accel_z;   /* g */
    float gyro_x, gyro_y, gyro_z;      /* dps */
    bool  mag_present;
    float mag_x, mag_y, mag_z;         /* uT */
    float temperature_c;
    bool  has_temperature;
} wr_imu_sample_t;

/* HAL lifecycle */
esp_err_t wr_hal_init(bool dry_run);

/* Motor */
esp_err_t wr_motor_set(float left, float right);
esp_err_t wr_motor_stop(void);
esp_err_t wr_motor_get_state(wr_motor_state_t *state);
void      wr_motor_emergency_stop_set(void);
void      wr_motor_emergency_stop_clear(void);
bool      wr_motor_emergency_stop_active(void);

/* Power */
esp_err_t wr_power_get_status(wr_power_status_t *status);

/* IMU */
esp_err_t wr_imu_get_sample(wr_imu_sample_t *sample);

/* Display */
esp_err_t wr_display_clear(void);
esp_err_t wr_display_text(const char *text, int line, bool clear_first);
esp_err_t wr_display_status(const char *fw_ver, const char *wifi_info,
                             float battery_v, bool mcp_active, bool estop);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Update `CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS
        "wave_rover_hal.c"
        "wave_rover_motor.c"
        "wave_rover_power.c"
        "wave_rover_imu.c"
        "wave_rover_display.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../boards/wave_rover"
    PRIV_REQUIRES
        driver
        esp_log
        esp_check
        freertos
)
```

- [ ] **Step 4: Write `wave_rover_hal.c`** (init, I2C bus)

```c
#include "wave_rover_hal.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "wr_hal";

i2c_master_bus_handle_t g_wr_i2c_bus = NULL;
static bool s_dry_run = true;
bool g_wr_dry_run = true;

esp_err_t wr_hal_init(bool dry_run)
{
    s_dry_run = dry_run;
    g_wr_dry_run = dry_run;

    if (dry_run) {
        ESP_LOGI(TAG, "dry-run mode: no hardware access");
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = WR_I2C_PORT,
        .sda_io_num          = WR_I2C_SDA,
        .scl_io_num          = WR_I2C_SCL,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &g_wr_i2c_bus),
                        TAG, "i2c_new_master_bus");
    ESP_LOGI(TAG, "I2C bus init: SDA=%d SCL=%d", WR_I2C_SDA, WR_I2C_SCL);
    return ESP_OK;
}
```

Create `wave_rover_hal_internal.h` (shared across .c files):

```c
#pragma once
#include "driver/i2c_master.h"
extern i2c_master_bus_handle_t g_wr_i2c_bus;
extern bool g_wr_dry_run;
```

- [ ] **Step 5: Write `wave_rover_motor.c`** (dry-run + real stubs)

```c
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "wr_motor";

static float    s_left  = 0.0f;
static float    s_right = 0.0f;
static bool     s_estop = false;
static bool     s_hw_init = false;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static esp_err_t hw_motor_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode       = WR_LEDC_SPEED,
        .duty_resolution  = WR_LEDC_BITS,
        .timer_num        = WR_LEDC_TIMER,
        .freq_hz          = WR_LEDC_FREQ_HZ,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");

    ledc_channel_config_t chA = {
        .gpio_num   = WR_MOTOR_PWMA,
        .speed_mode = WR_LEDC_SPEED,
        .channel    = WR_LEDC_CH_A,
        .timer_sel  = WR_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config_t chB = chA;
    chB.gpio_num = WR_MOTOR_PWMB;
    chB.channel  = WR_LEDC_CH_B;

    ESP_RETURN_ON_ERROR(ledc_channel_config(&chA), TAG, "ledc ch A");
    ESP_RETURN_ON_ERROR(ledc_channel_config(&chB), TAG, "ledc ch B");

    gpio_config_t dir = {
        .pin_bit_mask = (1ULL<<WR_MOTOR_AIN1)|(1ULL<<WR_MOTOR_AIN2)|
                        (1ULL<<WR_MOTOR_BIN1)|(1ULL<<WR_MOTOR_BIN2),
        .mode         = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&dir), TAG, "dir gpio");

    /* safe initial state: motors off */
    gpio_set_level(WR_MOTOR_AIN1, 0);
    gpio_set_level(WR_MOTOR_AIN2, 0);
    gpio_set_level(WR_MOTOR_BIN1, 0);
    gpio_set_level(WR_MOTOR_BIN2, 0);
    s_hw_init = true;
    return ESP_OK;
}

static void hw_set_side(float spd, int pwm_gpio, ledc_channel_t ch,
                         int in1, int in2)
{
    if (!s_hw_init) return;
    int pwm = (int)roundf(fabsf(spd) * 255.0f);
    if (pwm > 255) pwm = 255;
    if (spd > 0.01f) {
        gpio_set_level(in1, 1);
        gpio_set_level(in2, 0);
    } else if (spd < -0.01f) {
        gpio_set_level(in1, 0);
        gpio_set_level(in2, 1);
    } else {
        gpio_set_level(in1, 0);
        gpio_set_level(in2, 0);
        pwm = 0;
    }
    ledc_set_duty(WR_LEDC_SPEED, ch, pwm);
    ledc_update_duty(WR_LEDC_SPEED, ch);
}

esp_err_t wr_motor_set(float left, float right)
{
    if (s_estop) return ESP_ERR_INVALID_STATE;

    left  = clampf(left,  -1.0f, 1.0f);
    right = clampf(right, -1.0f, 1.0f);
    s_left  = left;
    s_right = right;

    if (g_wr_dry_run) {
        ESP_LOGD(TAG, "dry-run motor_set L=%.2f R=%.2f", left, right);
        return ESP_OK;
    }
    if (!s_hw_init) {
        ESP_RETURN_ON_ERROR(hw_motor_init(), TAG, "hw_motor_init");
    }
    hw_set_side(left,  WR_MOTOR_PWMA, WR_LEDC_CH_A, WR_MOTOR_AIN1, WR_MOTOR_AIN2);
    hw_set_side(right, WR_MOTOR_PWMB, WR_LEDC_CH_B, WR_MOTOR_BIN1, WR_MOTOR_BIN2);
    return ESP_OK;
}

esp_err_t wr_motor_stop(void)
{
    s_left = s_right = 0.0f;
    if (g_wr_dry_run) {
        ESP_LOGD(TAG, "dry-run motor_stop");
        return ESP_OK;
    }
    if (!s_hw_init) return ESP_OK;
    gpio_set_level(WR_MOTOR_AIN1, 0); gpio_set_level(WR_MOTOR_AIN2, 0);
    gpio_set_level(WR_MOTOR_BIN1, 0); gpio_set_level(WR_MOTOR_BIN2, 0);
    ledc_set_duty(WR_LEDC_SPEED, WR_LEDC_CH_A, 0);
    ledc_update_duty(WR_LEDC_SPEED, WR_LEDC_CH_A);
    ledc_set_duty(WR_LEDC_SPEED, WR_LEDC_CH_B, 0);
    ledc_update_duty(WR_LEDC_SPEED, WR_LEDC_CH_B);
    return ESP_OK;
}

esp_err_t wr_motor_get_state(wr_motor_state_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    s->left          = s_left;
    s->right         = s_right;
    s->emergency_stop = s_estop;
    return ESP_OK;
}

void wr_motor_emergency_stop_set(void)
{
    s_estop = true;
    wr_motor_stop();
    ESP_LOGW(TAG, "EMERGENCY STOP SET");
}

void wr_motor_emergency_stop_clear(void)
{
    s_estop = false;
    ESP_LOGI(TAG, "emergency stop cleared");
}

bool wr_motor_emergency_stop_active(void) { return s_estop; }
```

- [ ] **Step 6: Write `wave_rover_power.c`** (dry-run + INA219 stub)

```c
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include <string.h>

static const char *TAG = "wr_power";
static i2c_master_dev_handle_t s_ina219_dev = NULL;

/* INA219 register addresses */
#define INA219_REG_CONFIG   0x00
#define INA219_REG_SHUNT    0x01
#define INA219_REG_BUS      0x02
#define INA219_REG_POWER    0x03
#define INA219_REG_CURRENT  0x04
#define INA219_REG_CALIB    0x05

/* INA219 config: BRNG=0(16V), PGA=2(+/-320mV), BADC=9bit, SADC=9bit, mode=7(cont) */
#define INA219_CONFIG_VAL   0x219F
#define INA219_CALIB_VAL    0x8000  /* calibrated for 0.01 ohm shunt */
#define INA219_CURRENT_LSB  0.001f  /* 1mA per bit with above calibration */

static esp_err_t ina219_read_reg(uint8_t reg, uint16_t *val)
{
    uint8_t cmd = reg;
    uint8_t buf[2];
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_ina219_dev, &cmd, 1, 50), TAG, "tx");
    ESP_RETURN_ON_ERROR(i2c_master_receive(s_ina219_dev, buf, 2, 50), TAG, "rx");
    *val = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

static esp_err_t ina219_write_reg(uint8_t reg, uint16_t val)
{
    uint8_t buf[3] = {reg, (uint8_t)(val >> 8), (uint8_t)(val & 0xFF)};
    return i2c_master_transmit(s_ina219_dev, buf, 3, 50);
}

static esp_err_t ina219_init(void)
{
    if (!g_wr_i2c_bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = WR_INA219_ADDR,
        .scl_speed_hz    = WR_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_wr_i2c_bus, &dev_cfg, &s_ina219_dev),
                        TAG, "add device");
    ESP_RETURN_ON_ERROR(ina219_write_reg(INA219_REG_CONFIG, INA219_CONFIG_VAL), TAG, "config");
    ESP_RETURN_ON_ERROR(ina219_write_reg(INA219_REG_CALIB,  INA219_CALIB_VAL),  TAG, "calib");
    ESP_LOGI(TAG, "INA219 initialized at 0x%02X", WR_INA219_ADDR);
    return ESP_OK;
}

esp_err_t wr_power_get_status(wr_power_status_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));

    if (g_wr_dry_run || !g_wr_i2c_bus) {
        s->present        = false;
        s->bus_voltage_v  = 11.8f;
        s->current_ma     = 0.0f;
        s->load_voltage_v = 11.8f;
        s->low_battery    = false;
        return ESP_OK;
    }

    if (!s_ina219_dev) {
        esp_err_t e = ina219_init();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "INA219 init failed: %s", esp_err_to_name(e));
            return ESP_OK; /* present=false, not fatal */
        }
    }

    uint16_t shunt_raw, bus_raw, power_raw, current_raw;
    if (ina219_read_reg(INA219_REG_SHUNT,   &shunt_raw)   != ESP_OK ||
        ina219_read_reg(INA219_REG_BUS,     &bus_raw)     != ESP_OK ||
        ina219_read_reg(INA219_REG_POWER,   &power_raw)   != ESP_OK ||
        ina219_read_reg(INA219_REG_CURRENT, &current_raw) != ESP_OK) {
        return ESP_OK; /* present=false on I2C error */
    }

    s->present          = true;
    s->shunt_voltage_mv = (int16_t)shunt_raw * 0.01f;   /* 10uV/bit → mV */
    s->bus_voltage_v    = (float)(bus_raw >> 3) * 0.004f; /* 4mV/bit */
    s->load_voltage_v   = s->bus_voltage_v + s->shunt_voltage_mv / 1000.0f;
    s->current_ma       = (int16_t)current_raw * INA219_CURRENT_LSB * 1000.0f;
    s->power_mw         = power_raw * INA219_CURRENT_LSB * 20.0f * 1000.0f;
    s->charging         = (s->current_ma < -50.0f); /* negative = charging */
    s->low_battery      = (s->load_voltage_v < WR_LOW_BATT_V && s->load_voltage_v > 1.0f);
    return ESP_OK;
}
```

- [ ] **Step 7: Write `wave_rover_imu.c`** (dry-run stub; real QMI8658 reads)

```c
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <math.h>

static const char *TAG = "wr_imu";
static i2c_master_dev_handle_t s_qmi_dev  = NULL;
static i2c_master_dev_handle_t s_ak09_dev = NULL;

/* QMI8658 registers */
#define QMI_WHO_AM_I  0x00
#define QMI_CTRL1     0x02
#define QMI_CTRL2     0x03
#define QMI_CTRL3     0x04
#define QMI_CTRL7     0x08
#define QMI_TEMP_L    0x33
#define QMI_ACCX_L    0x35
#define QMI_GYRX_L    0x3B

/* AK09918 registers */
#define AK_WIA2   0x01
#define AK_ST1    0x10
#define AK_HXL    0x11
#define AK_CNTL2  0x31

static esp_err_t qmi_read(uint8_t reg, uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_qmi_dev, &reg, 1, 50), TAG, "qmi tx");
    return i2c_master_receive(s_qmi_dev, buf, len, 50);
}

static esp_err_t qmi_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_qmi_dev, buf, 2, 50);
}

static esp_err_t ak_read(uint8_t reg, uint8_t *buf, size_t len)
{
    ESP_RETURN_ON_ERROR(i2c_master_transmit(s_ak09_dev, &reg, 1, 50), TAG, "ak tx");
    return i2c_master_receive(s_ak09_dev, buf, len, 50);
}

static esp_err_t ak_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_ak09_dev, buf, 2, 50);
}

static esp_err_t imu_hw_init(void)
{
    if (!g_wr_i2c_bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t qmi_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = WR_QMI8658_ADDR,
        .scl_speed_hz    = WR_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_wr_i2c_bus, &qmi_cfg, &s_qmi_dev),
                        TAG, "add qmi");

    uint8_t who;
    ESP_RETURN_ON_ERROR(qmi_read(QMI_WHO_AM_I, &who, 1), TAG, "who_am_i");
    ESP_LOGI(TAG, "QMI8658 WHO_AM_I=0x%02X", who);

    /* Enable accel (±4g, ODR=468Hz) + gyro (±512dps, ODR=468Hz) */
    ESP_RETURN_ON_ERROR(qmi_write(QMI_CTRL1, 0x40), TAG, "ctrl1"); /* SPI off */
    ESP_RETURN_ON_ERROR(qmi_write(QMI_CTRL2, 0x03), TAG, "ctrl2"); /* accel: 4g */
    ESP_RETURN_ON_ERROR(qmi_write(QMI_CTRL3, 0x55), TAG, "ctrl3"); /* gyro: 512dps */
    ESP_RETURN_ON_ERROR(qmi_write(QMI_CTRL7, 0x03), TAG, "ctrl7"); /* enable acc+gyr */

    i2c_device_config_t ak_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = WR_AK09918_ADDR,
        .scl_speed_hz    = WR_I2C_FREQ_HZ,
    };
    if (i2c_master_bus_add_device(g_wr_i2c_bus, &ak_cfg, &s_ak09_dev) == ESP_OK) {
        ak_write(AK_CNTL2, 0x08); /* continuous mode 1 (10Hz) */
        ESP_LOGI(TAG, "AK09918 initialized");
    } else {
        ESP_LOGW(TAG, "AK09918 not found, mag disabled");
    }
    return ESP_OK;
}

static float raw16_to_float(uint8_t lo, uint8_t hi, float scale)
{
    int16_t raw = (int16_t)(((uint16_t)hi << 8) | lo);
    return raw * scale;
}

esp_err_t wr_imu_get_sample(wr_imu_sample_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));

    if (g_wr_dry_run || !g_wr_i2c_bus) {
        s->present       = false;
        s->accel_z       = 1.0f; /* gravity */
        return ESP_OK;
    }

    if (!s_qmi_dev) {
        if (imu_hw_init() != ESP_OK) return ESP_OK; /* present=false */
    }

    uint8_t raw[12];
    if (qmi_read(QMI_ACCX_L, raw, 12) != ESP_OK) return ESP_OK;

    s->present  = true;
    /* accel: ±4g, 16-bit → scale = 4.0/32768 */
    s->accel_x  = raw16_to_float(raw[0], raw[1], 4.0f / 32768.0f);
    s->accel_y  = raw16_to_float(raw[2], raw[3], 4.0f / 32768.0f);
    s->accel_z  = raw16_to_float(raw[4], raw[5], 4.0f / 32768.0f);
    /* gyro: ±512dps, 16-bit → scale = 512.0/32768 */
    s->gyro_x   = raw16_to_float(raw[6],  raw[7],  512.0f / 32768.0f);
    s->gyro_y   = raw16_to_float(raw[8],  raw[9],  512.0f / 32768.0f);
    s->gyro_z   = raw16_to_float(raw[10], raw[11], 512.0f / 32768.0f);

    uint8_t tmp[2];
    if (qmi_read(QMI_TEMP_L, tmp, 2) == ESP_OK) {
        s->has_temperature = true;
        s->temperature_c   = raw16_to_float(tmp[0], tmp[1], 1.0f / 256.0f);
    }

    if (s_ak09_dev) {
        uint8_t st1;
        if (ak_read(AK_ST1, &st1, 1) == ESP_OK && (st1 & 0x01)) {
            uint8_t mag[6];
            if (ak_read(AK_HXL, mag, 6) == ESP_OK) {
                s->mag_present = true;
                s->mag_x = raw16_to_float(mag[0], mag[1], 0.15f); /* 0.15 uT/LSB */
                s->mag_y = raw16_to_float(mag[2], mag[3], 0.15f);
                s->mag_z = raw16_to_float(mag[4], mag[5], 0.15f);
            }
        }
    }
    return ESP_OK;
}
```

- [ ] **Step 8: Write `wave_rover_display.c`** (SSD1306 I2C)

```c
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "wr_display";
static i2c_master_dev_handle_t s_oled_dev = NULL;

/* SSD1306 command byte */
#define SSD_CMD  0x00
#define SSD_DATA 0x40

static esp_err_t ssd_cmd(uint8_t cmd)
{
    uint8_t buf[2] = {SSD_CMD, cmd};
    return i2c_master_transmit(s_oled_dev, buf, 2, 50);
}

/* Minimal 5×7 font subset: ASCII 32-126 (space to ~) */
/* Only printable ASCII stored — 3 bytes per char for a 5x8 tiny font */
/* Use a pre-computed bitmap; for brevity only basic init shown here. */
/* In real implementation use esp_painter or a full font table. */

static esp_err_t oled_init(void)
{
    if (!g_wr_i2c_bus) return ESP_ERR_INVALID_STATE;
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = WR_OLED_ADDR,
        .scl_speed_hz    = WR_I2C_FREQ_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(g_wr_i2c_bus, &dev_cfg, &s_oled_dev),
                        TAG, "add ssd1306");

    static const uint8_t init_seq[] = {
        0xAE, /* display off */
        0x20, 0x00, /* horizontal addressing */
        0xB0, /* page start */
        0xC8, /* COM output scan dir */
        0x00, /* low col addr */
        0x10, /* high col addr */
        0x40, /* start line 0 */
        0x81, 0x7F, /* contrast */
        0xA1, /* seg remap */
        0xA6, /* normal display */
        0xA8, 0x1F, /* mux 32 lines */
        0xA4, /* output follows RAM */
        0xD3, 0x00, /* display offset 0 */
        0xD5, 0xF0, /* clock */
        0xD9, 0x22, /* precharge */
        0xDA, 0x02, /* COM pins for 32px height */
        0xDB, 0x20, /* vcomh */
        0x8D, 0x14, /* charge pump on */
        0xAF, /* display on */
    };
    for (size_t i = 0; i < sizeof(init_seq); i++) {
        if (ssd_cmd(init_seq[i]) != ESP_OK) {
            ESP_LOGW(TAG, "SSD1306 init cmd 0x%02X failed", init_seq[i]);
        }
    }
    ESP_LOGI(TAG, "SSD1306 initialized at 0x%02X", WR_OLED_ADDR);
    return ESP_OK;
}

esp_err_t wr_display_clear(void)
{
    if (g_wr_dry_run || !g_wr_i2c_bus) {
        ESP_LOGD(TAG, "dry-run display_clear");
        return ESP_OK;
    }
    if (!s_oled_dev) {
        if (oled_init() != ESP_OK) return ESP_OK;
    }
    ssd_cmd(0x21); ssd_cmd(0); ssd_cmd(127); /* col range */
    ssd_cmd(0x22); ssd_cmd(0); ssd_cmd(3);   /* page range (4 pages = 32px) */
    /* fill with zeros — send 128*4=512 zero data bytes */
    uint8_t buf[17]; /* 1 control byte + 16 data bytes */
    buf[0] = SSD_DATA;
    memset(buf + 1, 0, 16);
    for (int i = 0; i < 32; i++) {
        i2c_master_transmit(s_oled_dev, buf, 17, 50);
    }
    return ESP_OK;
}

esp_err_t wr_display_text(const char *text, int line, bool clear_first)
{
    if (g_wr_dry_run || !g_wr_i2c_bus) {
        ESP_LOGD(TAG, "dry-run display_text line=%d: %s", line, text ? text : "");
        return ESP_OK;
    }
    if (!s_oled_dev) {
        if (oled_init() != ESP_OK) return ESP_OK;
    }
    if (clear_first) wr_display_clear();
    if (!text) return ESP_OK;
    /* Basic text rendering using the component esp_painter if available,
     * or a minimal 5x8 font. For MVP, log the text and set page position.
     * Full font rendering is deferred to Task 10 (real HAL). */
    ESP_LOGI(TAG, "OLED[%d]: %s", line, text);
    /* TODO: render with font in Task 10 */
    return ESP_OK;
}

esp_err_t wr_display_status(const char *fw_ver, const char *wifi_info,
                             float battery_v, bool mcp_active, bool estop)
{
    char line[22];
    wr_display_clear();
    snprintf(line, sizeof(line), "FW:%s", fw_ver ? fw_ver : "?");
    wr_display_text(line, 0, false);
    snprintf(line, sizeof(line), "%.16s", wifi_info ? wifi_info : "no wifi");
    wr_display_text(line, 1, false);
    snprintf(line, sizeof(line), "Batt:%.1fV", battery_v);
    wr_display_text(line, 2, false);
    snprintf(line, sizeof(line), "MCP:%s%s", mcp_active ? "ON" : "OFF",
             estop ? " ESTOP" : "");
    wr_display_text(line, 3, false);
    return ESP_OK;
}
```

- [ ] **Step 9: Build and verify**

```bash
cd application/wave_rover && idf.py build
```

Expected: no new errors (HAL uses conditional includes, dry-run path always compiles).

- [ ] **Step 10: Commit**

```bash
git add application/wave_rover/components/wave_rover_hal/ \
        application/wave_rover/boards/
git commit -m "feat(wave_rover): add HAL stubs (motor, power, IMU, display)"
```

---

## Task 7b — Motor worker task (safety requirement)

**Context:** Tool callbacks run on the single `esp_http_server` task. If `rover.move` calls `vTaskDelay(3000ms)`, a concurrent `rover.stop` or `rover.emergency_stop` queues behind it and cannot preempt — violating the spec's "always able to call rover.stop" requirement.

**Fix:** Move motor execution to a dedicated FreeRTOS worker task with a command queue. The HTTP callbacks submit to the queue and block for result. Emergency stop sets a flag the worker task checks every 50 ms (identical pattern to `cap_rover_hw.c`).

**Files:**
- Modify: `application/wave_rover/components/wave_rover_hal/include/wave_rover_hal.h`
- Modify: `application/wave_rover/components/wave_rover_hal/wave_rover_motor.c`

- [ ] **Step 1: Add motor command queue API to `wave_rover_hal.h`**

Add after existing `wr_motor_*` declarations:

```c
typedef enum {
    WR_MOTOR_CMD_MOVE = 1,   /* set left/right for duration_ms, then stop */
    WR_MOTOR_CMD_STOP = 2,   /* immediate stop */
} wr_motor_cmd_type_t;

typedef struct {
    wr_motor_cmd_type_t type;
    float left;
    float right;
    uint16_t duration_ms;
} wr_motor_cmd_t;

/* Submit a command and wait for completion (or e-stop preemption).
 * Returns ESP_OK, ESP_ERR_INVALID_STATE (e-stop), or ESP_ERR_TIMEOUT. */
esp_err_t wr_motor_submit_and_wait(const wr_motor_cmd_t *cmd, uint32_t timeout_ms);
esp_err_t wr_motor_worker_start(void);
```

- [ ] **Step 2: Implement worker task in `wave_rover_motor.c`**

Add to the top of `wave_rover_motor.c` (after existing includes and static vars):

```c
#define MOTOR_CMD_QUEUE_DEPTH   4
#define MOTOR_TICK_MS          50
#define MOTOR_RESULT_QUEUE_DEPTH 4

typedef struct {
    uint32_t   req_id;
    esp_err_t  err;
} wr_motor_result_t;

static QueueHandle_t   s_cmd_queue    = NULL;
static QueueHandle_t   s_result_queue = NULL;
static SemaphoreHandle_t s_queue_lock = NULL;
static TaskHandle_t    s_worker_task  = NULL;
static uint32_t        s_req_seq      = 0;

static void motor_worker(void *arg)
{
    wr_motor_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err = ESP_OK;

        if (cmd.type == WR_MOTOR_CMD_STOP) {
            wr_motor_stop();
        } else if (cmd.type == WR_MOTOR_CMD_MOVE) {
            TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(cmd.duration_ms);
            while ((int32_t)(end - xTaskGetTickCount()) > 0) {
                if (s_estop) { err = ESP_ERR_INVALID_STATE; break; }
                wr_motor_set_raw(cmd.left, cmd.right); /* internal raw set, no estop check */
                vTaskDelay(pdMS_TO_TICKS(MOTOR_TICK_MS));
            }
            wr_motor_stop();
        }

        wr_motor_result_t res = { .req_id = cmd.req_id, .err = err };
        if (xQueueSend(s_result_queue, &res, 0) != pdTRUE) {
            wr_motor_result_t dropped;
            xQueueReceive(s_result_queue, &dropped, 0);
            xQueueSend(s_result_queue, &res, 0);
        }
    }
}

esp_err_t wr_motor_worker_start(void)
{
    s_cmd_queue    = xQueueCreate(MOTOR_CMD_QUEUE_DEPTH, sizeof(wr_motor_cmd_t));
    s_result_queue = xQueueCreate(MOTOR_RESULT_QUEUE_DEPTH, sizeof(wr_motor_result_t));
    s_queue_lock   = xSemaphoreCreateMutex();
    if (!s_cmd_queue || !s_result_queue || !s_queue_lock) return ESP_ERR_NO_MEM;
    BaseType_t ok = xTaskCreate(motor_worker, "wr_motor", 4096, NULL, 5, &s_worker_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wr_motor_submit_and_wait(const wr_motor_cmd_t *cmd_in, uint32_t timeout_ms)
{
    if (!s_cmd_queue || !s_result_queue || !s_queue_lock) return ESP_ERR_INVALID_STATE;
    if (s_estop && cmd_in->type == WR_MOTOR_CMD_MOVE) return ESP_ERR_INVALID_STATE;

    wr_motor_cmd_t cmd = *cmd_in;
    cmd.req_id = ++s_req_seq;

    xSemaphoreTake(s_queue_lock, portMAX_DELAY);
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        xSemaphoreGive(s_queue_lock);
        return ESP_ERR_TIMEOUT;
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    esp_err_t result = ESP_ERR_TIMEOUT;
    while (1) {
        wr_motor_result_t res;
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) break;
        if (xQueueReceive(s_result_queue, &res, deadline - now) == pdTRUE) {
            if (res.req_id == cmd.req_id) { result = res.err; break; }
        }
    }
    xSemaphoreGive(s_queue_lock);
    return result;
}
```

`wr_motor_set_raw` is an internal wrapper around the existing `hw_set_side` calls that bypasses the `s_estop` guard (the worker task checks `s_estop` itself in the loop).

- [ ] **Step 3: Update tool callbacks to use the queue**

Replace `wr_motor_set(...)` + `vTaskDelay(...)` + `wr_motor_stop()` pattern in `tool_move`, `tool_drive_tank`, `tool_turn` with:

```c
/* In tool_move: */
wr_motor_cmd_t cmd = {
    .type        = WR_MOTOR_CMD_MOVE,
    .left        = left,
    .right       = right,
    .duration_ms = (uint16_t)dur_ms,
};
esp_err_t e = wr_motor_submit_and_wait(&cmd, (uint32_t)dur_ms + 1000);
if (e == ESP_ERR_INVALID_STATE) {
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", "emergency_stop_preempted");
    return json_obj_str(root);
}
```

- [ ] **Step 4: Call `wr_motor_worker_start()` in `wr_hal_init`**

In `wave_rover_hal.c`, after initializing I2C bus:
```c
if (!dry_run) {
    ESP_RETURN_ON_ERROR(wr_motor_worker_start(), TAG, "motor worker start");
}
```

- [ ] **Step 5: Build**

```bash
cd application/wave_rover && idf.py build
```

- [ ] **Step 6: Commit**

```bash
git add application/wave_rover/components/wave_rover_hal/
git commit -m "feat(wave_rover): add motor worker task for safe concurrent stop"
```

---

## Task 8 — MCP server component (`wave_rover_mcp`)

**Files:**
- Modify: `application/wave_rover/components/wave_rover_mcp/CMakeLists.txt`
- Modify: `application/wave_rover/components/wave_rover_mcp/include/wave_rover_mcp.h`
- Create: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp.c`
- Create: `application/wave_rover/components/wave_rover_mcp/wave_rover_mcp_tools.c`

Strategy: `esp_mcp_mgr_init` + `esp_mcp_mgr_register_endpoint` initialise the engine. We create our own `esp_http_server` at port 8080 and call `esp_mcp_mgr_req_handle` from it. Resources (`resources/list`, `resources/read`) are handled before the SDK is invoked.

- [ ] **Step 1: Update `CMakeLists.txt`**

```cmake
idf_component_register(
    SRCS "wave_rover_mcp.c" "wave_rover_mcp_tools.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        wave_rover_hal
        wave_rover_config
        mcp-c-sdk
        esp_http_server
        esp_log
        esp_check
        esp_wifi
        freertos
)
```

- [ ] **Step 2: Write `include/wave_rover_mcp.h`**

```c
#pragma once
#include "esp_err.h"
#include "wave_rover_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wave_rover_mcp_start(const wave_rover_config_t *cfg);
esp_err_t wave_rover_mcp_stop(void);
bool      wave_rover_mcp_is_running(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 3: Write `wave_rover_mcp.c`** (HTTP server, routing, resources)

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_mcp.h"
#include "wave_rover_hal.h"
#include "wave_rover_config.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "wr_mcp";

#define MCP_ENDPOINT   "mcp"
#define MCP_MAX_BODY   (8 * 1024)

/* Forward declarations from wave_rover_mcp_tools.c */
extern esp_err_t wr_mcp_register_all_tools(esp_mcp_t *mcp);

static esp_mcp_t          *s_mcp      = NULL;
static esp_mcp_mgr_handle_t s_mgr     = 0;
static httpd_handle_t       s_httpd   = NULL;
static bool                 s_running = false;
static TimerHandle_t        s_keepalive_timer = NULL;
static uint32_t             s_last_cmd_ms = 0;

/* ------------------------------------------------------------------ */
/* Resources                                                           */
/* ------------------------------------------------------------------ */

static const char * const s_resource_uris[] = {
    "rover://status", "rover://config", "rover://wifi",
    "rover://power",  "rover://ups",    "rover://imu",
    "rover://display","rover://logs/recent",
};
#define NUM_RESOURCES (sizeof(s_resource_uris) / sizeof(s_resource_uris[0]))

static cJSON *build_resources_list(void)
{
    cJSON *result = cJSON_CreateObject();
    cJSON *arr    = cJSON_AddArrayToObject(result, "resources");
    for (size_t i = 0; i < NUM_RESOURCES; i++) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "uri",      s_resource_uris[i]);
        cJSON_AddStringToObject(r, "mimeType", "application/json");
        cJSON_AddItemToArray(arr, r);
    }
    return result;
}

static cJSON *read_resource(const char *uri)
{
    cJSON *out = cJSON_CreateObject();
    if (!uri) { cJSON_AddStringToObject(out, "error", "no uri"); return out; }

    if (strcmp(uri, "rover://status") == 0 ||
        strcmp(uri, "rover://config") == 0 ||
        strcmp(uri, "rover://wifi")   == 0) {
        /* These are read by dedicated tools; duplicate here as resources */
        wr_motor_state_t ms = {0};
        wr_motor_get_state(&ms);
        cJSON_AddBoolToObject(out,   "ok",   true);
        cJSON_AddNumberToObject(out, "left_motor",  ms.left);
        cJSON_AddNumberToObject(out, "right_motor", ms.right);
        cJSON_AddBoolToObject(out,   "emergency_stop", ms.emergency_stop);
    } else if (strcmp(uri, "rover://power") == 0 ||
               strcmp(uri, "rover://ups")   == 0) {
        wr_power_status_t ps = {0};
        wr_power_get_status(&ps);
        cJSON_AddBoolToObject(out,   "ok",          true);
        cJSON_AddBoolToObject(out,   "present",      ps.present);
        cJSON_AddNumberToObject(out, "voltage_v",    ps.load_voltage_v);
        cJSON_AddNumberToObject(out, "current_ma",   ps.current_ma);
        cJSON_AddBoolToObject(out,   "low_battery",  ps.low_battery);
    } else if (strcmp(uri, "rover://imu") == 0) {
        wr_imu_sample_t is = {0};
        wr_imu_get_sample(&is);
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddBoolToObject(out, "present", is.present);
    } else if (strcmp(uri, "rover://logs/recent") == 0) {
        cJSON_AddStringToObject(out, "log", "(ring buffer not implemented in MVP)");
    } else {
        cJSON_AddBoolToObject(out,   "ok",    false);
        cJSON_AddStringToObject(out, "error", "unknown resource");
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* JSON-RPC helpers                                                    */
/* ------------------------------------------------------------------ */

static char *jsonrpc_resources_list_resp(cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemReferenceToObject(resp, "id", id);
    cJSON *result = build_resources_list();
    cJSON_AddItemToObject(resp, "result", result);
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

static char *jsonrpc_resources_read_resp(cJSON *id, const char *uri)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemReferenceToObject(resp, "id", id);
    cJSON *content_arr = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "uri", uri ? uri : "");
    cJSON_AddStringToObject(item, "mimeType", "application/json");
    cJSON *data = read_resource(uri);
    char *data_str = cJSON_PrintUnformatted(data);
    cJSON_AddStringToObject(item, "text", data_str ? data_str : "{}");
    free(data_str);
    cJSON_Delete(data);
    cJSON_AddItemToArray(content_arr, item);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "contents", content_arr);
    cJSON_AddItemToObject(resp, "result", result);
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

static char *jsonrpc_error_resp(cJSON *id, int code, const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemReferenceToObject(resp, "id", id);
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code",    code);
    cJSON_AddStringToObject(err, "message", msg ? msg : "error");
    char *s = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return s;
}

/* ------------------------------------------------------------------ */
/* HTTP POST handler for /mcp                                          */
/* ------------------------------------------------------------------ */

static esp_err_t mcp_post_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > MCP_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad length");
        return ESP_OK;
    }

    char *body = calloc(1, total + 1);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        return ESP_OK;
    }

    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) { free(body); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv"); return ESP_OK; }
        received += r;
    }

    s_last_cmd_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* Parse method to decide routing */
    const char *resp_str = NULL;
    char *sdk_resp = NULL;
    cJSON *root = cJSON_Parse(body);
    cJSON *method_j = root ? cJSON_GetObjectItem(root, "method") : NULL;
    cJSON *id_j     = root ? cJSON_GetObjectItem(root, "id") : NULL;
    const char *method = method_j && cJSON_IsString(method_j) ? method_j->valuestring : "";

    if (strcmp(method, "resources/list") == 0) {
        sdk_resp = jsonrpc_resources_list_resp(id_j);
        resp_str = sdk_resp;
    } else if (strcmp(method, "resources/read") == 0) {
        cJSON *params = cJSON_GetObjectItem(root, "params");
        cJSON *uri_j  = params ? cJSON_GetObjectItem(params, "uri") : NULL;
        const char *uri = uri_j && cJSON_IsString(uri_j) ? uri_j->valuestring : NULL;
        if (!uri) {
            sdk_resp = jsonrpc_error_resp(id_j, -32602, "missing uri");
        } else {
            sdk_resp = jsonrpc_resources_read_resp(id_j, uri);
        }
        resp_str = sdk_resp;
    } else {
        /* Delegate to MCP SDK engine */
        uint8_t *out_buf = NULL;
        uint16_t out_len = 0;
        esp_err_t e = esp_mcp_mgr_req_handle(s_mgr, MCP_ENDPOINT,
                                              (const uint8_t *)body, (uint16_t)total,
                                              &out_buf, &out_len);
        if (e == ESP_OK && out_buf && out_len > 0) {
            resp_str = (const char *)out_buf;
        } else {
            sdk_resp = jsonrpc_error_resp(id_j, -32603, "internal error");
            resp_str = sdk_resp;
        }
        if (root) cJSON_Delete(root);
        free(body);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, resp_str, resp_str ? (int)strlen(resp_str) : 0);
        free(sdk_resp);
        if (out_buf) esp_mcp_mgr_req_destroy_response(s_mgr, out_buf);
        return ESP_OK;
    }

    if (root) cJSON_Delete(root);
    free(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp_str, resp_str ? (int)strlen(resp_str) : 0);
    free(sdk_resp);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Keepalive watchdog — stop motors if no commands received            */
/* ------------------------------------------------------------------ */

static void keepalive_cb(TimerHandle_t t)
{
    (void)t;
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - s_last_cmd_ms > 10000) { /* 10s idle = stop */
        wr_motor_stop();
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t wave_rover_mcp_start(const wave_rover_config_t *cfg)
{
    if (s_running) return ESP_OK;

    /* 1. Create MCP engine and pass config to tools layer */
    ESP_RETURN_ON_ERROR(esp_mcp_create(&s_mcp), TAG, "esp_mcp_create");
    wr_mcp_tools_set_config(cfg);  /* avoids extern dep on app_main */
    ESP_RETURN_ON_ERROR(wr_mcp_register_all_tools(s_mcp), TAG, "register tools");

    /* 2. Init manager with a null (no-op) transport so the engine routes
     *    through esp_mcp_mgr_req_handle without needing its own HTTP server.
     *    Register_endpoint with a null transport is a safe no-op. */
    static esp_err_t null_transport_init(esp_mcp_mgr_handle_t h, esp_mcp_transport_handle_t *t)
        { *t = (esp_mcp_transport_handle_t)1; return ESP_OK; }
    static esp_err_t null_transport_noop_h(esp_mcp_transport_handle_t t) { return ESP_OK; }
    static esp_err_t null_transport_start(esp_mcp_transport_handle_t t, void *c) { return ESP_OK; }
    static esp_err_t null_transport_create_cfg(const void *c, void **o) { *o = NULL; return ESP_OK; }
    static esp_err_t null_transport_delete_cfg(void *c) { return ESP_OK; }
    static esp_err_t null_transport_reg_ep(esp_mcp_transport_handle_t t, const char *n, void *d) { return ESP_OK; }
    static esp_err_t null_transport_unreg_ep(esp_mcp_transport_handle_t t, const char *n) { return ESP_OK; }
    static const esp_mcp_transport_t s_null_transport = {
        .init                = null_transport_init,
        .deinit              = null_transport_noop_h,
        .start               = null_transport_start,
        .stop                = null_transport_noop_h,
        .create_config       = null_transport_create_cfg,
        .delete_config       = null_transport_delete_cfg,
        .register_endpoint   = null_transport_reg_ep,
        .unregister_endpoint = null_transport_unreg_ep,
        .request             = NULL,
    };
    esp_mcp_mgr_config_t mcfg = {
        .transport = s_null_transport,
        .config    = NULL,
        .instance  = s_mcp,
    };
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_init(mcfg, &s_mgr), TAG, "mgr init");
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_register_endpoint(s_mgr, MCP_ENDPOINT, NULL),
                        TAG, "register endpoint");

    /* 3. Start our own HTTP server */
    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port      = cfg->mcp_port;
    hcfg.max_uri_handlers = 4;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hcfg), TAG, "httpd_start");

    httpd_uri_t mcp_uri = {
        .uri      = "/mcp",
        .method   = HTTP_POST,
        .handler  = mcp_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &mcp_uri),
                        TAG, "register uri");

    /* 4. Keepalive timer */
    s_last_cmd_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    s_keepalive_timer = xTimerCreate("mcp_ka", pdMS_TO_TICKS(5000),
                                     pdTRUE, NULL, keepalive_cb);
    if (s_keepalive_timer) xTimerStart(s_keepalive_timer, 0);

    s_running = true;
    ESP_LOGI(TAG, "MCP server started at http://<ip>:%u/mcp", cfg->mcp_port);
    return ESP_OK;
}

esp_err_t wave_rover_mcp_stop(void)
{
    if (!s_running) return ESP_OK;
    if (s_keepalive_timer) { xTimerStop(s_keepalive_timer, 0); xTimerDelete(s_keepalive_timer, 0); }
    if (s_httpd)   httpd_stop(s_httpd);
    if (s_mgr)     esp_mcp_mgr_deinit(s_mgr);
    if (s_mcp)     esp_mcp_destroy(s_mcp);
    s_running = false;
    return ESP_OK;
}

bool wave_rover_mcp_is_running(void) { return s_running; }
```

- [ ] **Step 4: Write `wave_rover_mcp_tools.c`** (all tool callbacks)

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_hal.h"
#include "wave_rover_config.h"
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_tool.h"
#include "esp_mcp_property.h"
#include "esp_mcp_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wr_tools";
/* Config set via wr_mcp_tools_set_config() called from wave_rover_mcp_start() */
static const wave_rover_config_t *s_cfg = NULL;
void wr_mcp_tools_set_config(const wave_rover_config_t *cfg) { s_cfg = cfg; }
/* Use s_cfg everywhere g_wr_cfg appeared in original draft */

#define WR_FW_VERSION "0.1.0"
#define WR_FW_NAME    "wave-rover-esp-claw-mcp"

/* Helper: create a JSON string result */
static esp_mcp_value_t json_str(const char *s)
{
    return esp_mcp_value_create_string(s ? s : "{}");
}

static esp_mcp_value_t json_obj_str(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    esp_mcp_value_t v = json_str(s);
    free(s);
    cJSON_Delete(obj);
    return v;
}

/* ------------------------------------------------------------------ */
/* rover.get_status                                                    */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_get_status(const esp_mcp_property_list_t *p)
{
    (void)p;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "state", wr_motor_emergency_stop_active() ? "emergency_stop" : "idle");

    cJSON *fw = cJSON_AddObjectToObject(root, "firmware");
    cJSON_AddStringToObject(fw, "name",    WR_FW_NAME);
    cJSON_AddStringToObject(fw, "version", WR_FW_VERSION);

    cJSON *mot = cJSON_AddObjectToObject(root, "motors");
    wr_motor_state_t ms = {0};
    wr_motor_get_state(&ms);
    cJSON_AddNumberToObject(mot, "left",  ms.left);
    cJSON_AddNumberToObject(mot, "right", ms.right);
    cJSON_AddBoolToObject(mot,   "emergency_stop", ms.emergency_stop);

    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);
    cJSON *pwr = cJSON_AddObjectToObject(root, "power");
    cJSON_AddBoolToObject(pwr,   "present",      ps.present);
    cJSON_AddNumberToObject(pwr, "voltage",      ps.load_voltage_v);
    cJSON_AddNumberToObject(pwr, "current_ma",   ps.current_ma);
    cJSON_AddBoolToObject(pwr,   "charging",     ps.charging);
    cJSON_AddBoolToObject(pwr,   "low_battery",  ps.low_battery);

    wr_imu_sample_t is = {0};
    wr_imu_get_sample(&is);
    cJSON *imu = cJSON_AddObjectToObject(root, "imu");
    cJSON_AddBoolToObject(imu, "present", is.present);

    cJSON_AddNumberToObject(root, "uptime_ms",  esp_timer_get_time() / 1000);
    cJSON_AddNumberToObject(root, "free_heap",  esp_get_free_heap_size());
    cJSON_AddNullToObject(root,   "last_error");

    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.get_config                                                    */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_get_config(const esp_mcp_property_list_t *p)
{
    (void)p;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    if (s_cfg) {
        cJSON_AddStringToObject(root, "hostname",       s_cfg->hostname);
        cJSON_AddNumberToObject(root, "mcp_port",       s_cfg->mcp_port);
        cJSON_AddBoolToObject(root,   "auth_enabled",   s_cfg->auth_enabled);
        cJSON_AddBoolToObject(root,   "dry_run",        s_cfg->dry_run);
        cJSON_AddNumberToObject(root, "max_speed",      s_cfg->max_speed);
        cJSON_AddNumberToObject(root, "max_cmd_ms",     s_cfg->max_command_duration_ms);
    }
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.stop                                                          */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_stop(const esp_mcp_property_list_t *p)
{
    const char *reason = esp_mcp_property_list_get_property_string(p, "reason");
    wr_motor_stop();
    ESP_LOGI(TAG, "rover.stop: %s", reason ? reason : "(no reason)");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", "stopped");
    if (reason) cJSON_AddStringToObject(root, "reason", reason);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.emergency_stop                                                */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_emergency_stop(const esp_mcp_property_list_t *p)
{
    const char *reason = esp_mcp_property_list_get_property_string(p, "reason");
    wr_motor_emergency_stop_set();
    ESP_LOGW(TAG, "rover.emergency_stop: %s", reason ? reason : "(no reason)");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", "emergency_stop_set");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.clear_emergency_stop                                          */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_clear_estop(const esp_mcp_property_list_t *p)
{
    bool confirm = esp_mcp_property_list_get_property_bool(p, "confirm");
    cJSON *root = cJSON_CreateObject();
    if (!confirm) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "confirm must be true");
        return json_obj_str(root);
    }
    wr_motor_emergency_stop_clear();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", "emergency_stop_cleared");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.move                                                          */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_move(const esp_mcp_property_list_t *p)
{
    float linear  = (float)esp_mcp_property_list_get_property_float(p, "linear");
    float angular = (float)esp_mcp_property_list_get_property_float(p, "angular");
    int   dur_ms  = esp_mcp_property_list_get_property_int(p, "duration_ms");

    cJSON *root = cJSON_CreateObject();

    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    /* Clamp per spec */
    if (linear < -1.0f) linear = -1.0f;
    if (linear >  1.0f) linear =  1.0f;
    if (angular < -1.0f) angular = -1.0f;
    if (angular >  1.0f) angular =  1.0f;
    if (dur_ms < 1)    dur_ms = 1;
    if (dur_ms > 3000) dur_ms = 3000;

    /* Apply max_speed limit */
    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (linear  >  max_spd) linear  =  max_spd;
    if (linear  < -max_spd) linear  = -max_spd;

    /* Tank conversion: left = linear - angular, right = linear + angular */
    float left  = linear - angular;
    float right = linear + angular;
    if (left  >  1.0f) left  =  1.0f;
    if (left  < -1.0f) left  = -1.0f;
    if (right >  1.0f) right =  1.0f;
    if (right < -1.0f) right = -1.0f;

    wr_motor_set(left, right);
    vTaskDelay(pdMS_TO_TICKS(dur_ms));
    wr_motor_stop();

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", "move_complete");
    cJSON_AddNumberToObject(root, "linear", linear);
    cJSON_AddNumberToObject(root, "angular", angular);
    cJSON_AddNumberToObject(root, "duration_ms", dur_ms);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.drive_tank                                                    */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_drive_tank(const esp_mcp_property_list_t *p)
{
    float left  = (float)esp_mcp_property_list_get_property_float(p, "left");
    float right = (float)esp_mcp_property_list_get_property_float(p, "right");
    int   dur_ms = esp_mcp_property_list_get_property_int(p, "duration_ms");

    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (left  >  max_spd) left  =  max_spd;
    if (left  < -max_spd) left  = -max_spd;
    if (right >  max_spd) right =  max_spd;
    if (right < -max_spd) right = -max_spd;
    if (dur_ms < 1)    dur_ms = 1;
    if (dur_ms > 3000) dur_ms = 3000;

    wr_motor_set(left, right);
    vTaskDelay(pdMS_TO_TICKS(dur_ms));
    wr_motor_stop();

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", "drive_tank_complete");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.turn                                                          */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_turn(const esp_mcp_property_list_t *p)
{
    const char *dir = esp_mcp_property_list_get_property_string(p, "direction");
    float spd       = (float)esp_mcp_property_list_get_property_float(p, "speed");
    int   dur_ms    = esp_mcp_property_list_get_property_int(p, "duration_ms");

    cJSON *root = cJSON_CreateObject();
    if (wr_motor_emergency_stop_active()) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "emergency_stop_active");
        return json_obj_str(root);
    }

    float max_spd = s_cfg ? s_cfg->max_speed : 0.4f;
    if (spd < 0.0f) spd = 0.0f;
    if (spd > max_spd) spd = max_spd;
    if (dur_ms < 1)    dur_ms = 1;
    if (dur_ms > 3000) dur_ms = 3000;

    float left = 0.0f, right = 0.0f;
    if (dir && strcmp(dir, "right") == 0) { left  =  spd; right = -spd; }
    else                                  { left  = -spd; right =  spd; }

    wr_motor_set(left, right);
    vTaskDelay(pdMS_TO_TICKS(dur_ms));
    wr_motor_stop();

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", "turn_complete");
    cJSON_AddStringToObject(root, "direction", dir ? dir : "left");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.get_power / rover.get_ups                                     */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_get_power(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "ok",              true);
    cJSON_AddBoolToObject(root,   "present",          ps.present);
    cJSON_AddStringToObject(root, "source",           "ina219");
    cJSON_AddNumberToObject(root, "bus_voltage_v",    ps.bus_voltage_v);
    cJSON_AddNumberToObject(root, "shunt_voltage_mv", ps.shunt_voltage_mv);
    cJSON_AddNumberToObject(root, "current_ma",       ps.current_ma);
    cJSON_AddNumberToObject(root, "power_mw",         ps.power_mw);
    cJSON_AddBoolToObject(root,   "charging",         ps.charging);
    cJSON_AddBoolToObject(root,   "low_battery",      ps.low_battery);
    return json_obj_str(root);
}

static esp_mcp_value_t tool_get_ups(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root,   "ok",       true);
    cJSON_AddBoolToObject(root,   "present",   ps.present);
    cJSON_AddStringToObject(root, "type",      "3s_18650");
    cJSON_AddStringToObject(root, "monitor",   "ina219");
    cJSON_AddNumberToObject(root, "voltage_v", ps.load_voltage_v);
    cJSON_AddNumberToObject(root, "current_ma",ps.current_ma);
    cJSON_AddNullToObject(root,   "estimated_percent");
    cJSON_AddBoolToObject(root,   "charging",   ps.charging);
    cJSON_AddBoolToObject(root,   "discharging",!ps.charging);
    cJSON_AddBoolToObject(root,   "low_battery",ps.low_battery);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* rover.get_imu                                                       */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_get_imu(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_imu_sample_t is = {0};
    wr_imu_get_sample(&is);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "present", is.present);
    cJSON_AddStringToObject(root, "chip", "QMI8658+AK09918");

    cJSON *accel = cJSON_AddObjectToObject(root, "accel");
    cJSON_AddNumberToObject(accel, "x", is.accel_x);
    cJSON_AddNumberToObject(accel, "y", is.accel_y);
    cJSON_AddNumberToObject(accel, "z", is.accel_z);

    cJSON *gyro = cJSON_AddObjectToObject(root, "gyro");
    cJSON_AddNumberToObject(gyro, "x", is.gyro_x);
    cJSON_AddNumberToObject(gyro, "y", is.gyro_y);
    cJSON_AddNumberToObject(gyro, "z", is.gyro_z);

    cJSON *mag = cJSON_AddObjectToObject(root, "mag");
    cJSON_AddBoolToObject(mag, "present", is.mag_present);
    cJSON_AddNumberToObject(mag, "x", is.mag_x);
    cJSON_AddNumberToObject(mag, "y", is.mag_y);
    cJSON_AddNumberToObject(mag, "z", is.mag_z);

    if (is.has_temperature)
        cJSON_AddNumberToObject(root, "temperature_c", is.temperature_c);
    else
        cJSON_AddNullToObject(root, "temperature_c");

    cJSON_AddNullToObject(root, "calibration");
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* Display tools                                                       */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_display_text(const esp_mcp_property_list_t *p)
{
    const char *text = esp_mcp_property_list_get_property_string(p, "text");
    int line         = esp_mcp_property_list_get_property_int(p, "line");
    bool clr         = esp_mcp_property_list_get_property_bool(p, "clear");
    wr_display_text(text, line, clr);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_obj_str(root);
}

static esp_mcp_value_t tool_display_clear(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_display_clear();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_obj_str(root);
}

static esp_mcp_value_t tool_display_status(const esp_mcp_property_list_t *p)
{
    (void)p;
    wr_power_status_t ps = {0};
    wr_power_get_status(&ps);
    wr_display_status(WR_FW_VERSION, "MCP active", ps.load_voltage_v,
                      true, wr_motor_emergency_stop_active());
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* Wi-Fi tools (read-only MVP; set_wifi saves to NVS + reboot)        */
/* ------------------------------------------------------------------ */
static esp_mcp_value_t tool_get_wifi(const esp_mcp_property_list_t *p)
{
    (void)p;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    if (s_cfg) {
        cJSON_AddNumberToObject(root, "mode", s_cfg->wifi_mode);
        cJSON_AddStringToObject(root, "hostname", s_cfg->hostname);
        /* Never return passwords */
    }
    return json_obj_str(root);
}

static esp_mcp_value_t tool_set_wifi(const esp_mcp_property_list_t *p)
{
    const char *ssid     = esp_mcp_property_list_get_property_string(p, "ssid");
    const char *password = esp_mcp_property_list_get_property_string(p, "password");
    const char *mode_str = esp_mcp_property_list_get_property_string(p, "mode");
    bool save            = esp_mcp_property_list_get_property_bool(p, "save");

    cJSON *root = cJSON_CreateObject();
    if (!mode_str) {
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddStringToObject(root, "error", "mode required");
        return json_obj_str(root);
    }

    /* Changes take effect after reboot; save to NVS if requested */
    if (s_cfg && save) {
        wave_rover_config_t new_cfg;
        memcpy(&new_cfg, s_cfg, sizeof(new_cfg));
        if (ssid)     strlcpy(new_cfg.wifi_ssid, ssid, sizeof(new_cfg.wifi_ssid));
        /* Do NOT log password */
        if (password) strlcpy(new_cfg.wifi_password, password, sizeof(new_cfg.wifi_password));
        if (strcmp(mode_str, "sta") == 0)    new_cfg.wifi_mode = 1;
        else if (strcmp(mode_str, "ap_sta") == 0) new_cfg.wifi_mode = 2;
        else                                 new_cfg.wifi_mode = 0;
        wave_rover_config_save(&new_cfg);
        ESP_LOGI(TAG, "wifi config saved, reboot to apply");
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "note", "reboot_to_apply");
    /* Never return password in response */
    return json_obj_str(root);
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

#define TOOL(name, desc, cb) do { \
    esp_mcp_tool_t *t = esp_mcp_tool_create(name, desc, cb); \
    if (!t) return ESP_ERR_NO_MEM; \
    if (esp_mcp_add_tool(mcp, t) != ESP_OK) { esp_mcp_tool_destroy(t); return ESP_FAIL; } \
} while(0)

/* Verified API: create_with_range is INTEGER only — float params must use create_with_float */
#define PROP_STR(t, name)        esp_mcp_tool_add_property(t, esp_mcp_property_create_with_string(name, ""))
#define PROP_FLOAT(t, name)      esp_mcp_tool_add_property(t, esp_mcp_property_create_with_float(name, 0.0f))
#define PROP_BOOL(t, name)       esp_mcp_tool_add_property(t, esp_mcp_property_create_with_bool(name, false))
#define PROP_INT_RANGE(t, n, lo, hi) esp_mcp_tool_add_property(t, esp_mcp_property_create_with_range(n, lo, hi))

esp_err_t wr_mcp_register_all_tools(esp_mcp_t *mcp)
{
    /* System */
    TOOL("rover.get_status",  "Get full rover status",  tool_get_status);
    TOOL("rover.get_config",  "Get rover configuration", tool_get_config);

    /* Motor */
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.move", "Move rover (linear+angular)", tool_move);
        PROP_FLOAT(t, "linear");       /* float [-1,1] — create_with_range is int-only */
        PROP_FLOAT(t, "angular");      /* float [-1,1] */
        PROP_INT_RANGE(t, "duration_ms", 1, 3000);
        esp_mcp_add_tool(mcp, t);
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.drive_tank", "Tank drive (left+right)", tool_drive_tank);
        PROP_FLOAT(t, "left");         /* float [-1,1] */
        PROP_FLOAT(t, "right");        /* float [-1,1] */
        PROP_INT_RANGE(t, "duration_ms", 1, 3000);
        esp_mcp_add_tool(mcp, t);
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.turn", "Turn in place", tool_turn);
        PROP_STR(t, "direction");
        PROP_FLOAT(t, "speed");        /* float [0,1] */
        PROP_INT_RANGE(t, "duration_ms", 1, 3000);
        esp_mcp_add_tool(mcp, t);
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.stop", "Stop all motors", tool_stop);
        PROP_STR(t, "reason");
        esp_mcp_add_tool(mcp, t);
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.emergency_stop", "Emergency stop (blocks movement)", tool_emergency_stop);
        PROP_STR(t, "reason");
        esp_mcp_add_tool(mcp, t);
    }
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.clear_emergency_stop", "Clear emergency stop flag", tool_clear_estop);
        PROP_BOOL(t, "confirm");
        esp_mcp_add_tool(mcp, t);
    }

    /* Power */
    TOOL("rover.get_power", "Get INA219 power status", tool_get_power);
    TOOL("rover.get_ups",   "Get UPS/battery status",  tool_get_ups);

    /* IMU */
    TOOL("rover.get_imu",     "Get IMU sensor data",    tool_get_imu);

    /* Display */
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.display_text", "Show text on OLED", tool_display_text);
        PROP_STR(t, "text");
        PROP_INT_RANGE(t, "line", 0, 3);
        PROP_BOOL(t, "clear");
        esp_mcp_add_tool(mcp, t);
    }
    TOOL("rover.display_clear",  "Clear OLED display",    tool_display_clear);
    TOOL("rover.display_status", "Show system status on OLED", tool_display_status);

    /* Wi-Fi */
    TOOL("rover.get_wifi", "Get Wi-Fi config (no passwords)", tool_get_wifi);
    {
        esp_mcp_tool_t *t = esp_mcp_tool_create("rover.set_wifi", "Set Wi-Fi config", tool_set_wifi);
        PROP_STR(t, "ssid");
        PROP_STR(t, "password");
        PROP_STR(t, "mode");
        PROP_BOOL(t, "save");
        esp_mcp_add_tool(mcp, t);
    }

    ESP_LOGI(TAG, "registered all rover MCP tools");
    return ESP_OK;
}
```

- [ ] **Step 5: Build**

```bash
cd application/wave_rover && idf.py build
```

Expected: success.

- [ ] **Step 6: Commit**

```bash
git add application/wave_rover/components/wave_rover_mcp/
git commit -m "feat(wave_rover): add MCP server with tools and resources"
```

---

## Task 9 — Wire everything in app_main

**Files:**
- Modify: `application/wave_rover/main/app_main.c`

- [ ] **Step 1: Rewrite `app_main.c`**

```c
/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "wave_rover_config.h"
#include "wave_rover_hal.h"
#include "wave_rover_mcp.h"
#include "wr_wifi.h"

static const char *TAG = "wave_rover";
static wave_rover_config_t s_cfg;

void app_main(void)
{
    ESP_LOGI(TAG, "Wave Rover MCP firmware v0.1.0 starting");

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(wave_rover_config_load(&s_cfg));

    ESP_ERROR_CHECK(wr_hal_init(s_cfg.dry_run));
    wr_motor_stop(); /* ensure motors off at boot */

    ESP_ERROR_CHECK(wr_wifi_init(&s_cfg));

    ESP_ERROR_CHECK(wave_rover_mcp_start(&s_cfg));

    /* Show boot status on OLED */
    wr_display_status("0.1.0", wr_wifi_get_ip(),
                      0.0f, true, false);

    ESP_LOGI(TAG, "boot complete. MCP at http://%s:%u/mcp",
             wr_wifi_get_ip()[0] ? wr_wifi_get_ip() : "<AP>",
             s_cfg.mcp_port);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGD(TAG, "uptime heap=%lu", (unsigned long)esp_get_free_heap_size());
    }
}
```

- [ ] **Step 2: Verify config is passed via `wr_mcp_tools_set_config`**

`wave_rover_mcp_start(cfg)` calls `wr_mcp_tools_set_config(cfg)` in Step 1 of Task 8.
`s_cfg` in `wave_rover_mcp_tools.c` is set from this call.
No `extern` coupling to `app_main.c` — verify the symbol `s_cfg` is used (not `g_wr_cfg`) throughout `wave_rover_mcp_tools.c`.

- [ ] **Step 3: Build**

```bash
cd application/wave_rover && idf.py build
```

Expected: Full build success with all components linked.

- [ ] **Step 4: Flash and verify on device (or run in dry-run mode)**

```bash
idf.py flash monitor
# OR via PlatformIO:
pio run -t upload && pio device monitor
```

Expected serial output:
```
I (XXX) wave_rover: Wave Rover MCP firmware v0.1.0 starting
I (XXX) wr_config: config loaded: wifi_mode=0 mcp_port=8080 dry_run=1
I (XXX) wr_hal: dry-run mode: no hardware access
I (XXX) wr_wifi: AP started: SSID=WR-ESP32
I (XXX) wr_mcp: MCP server started at http://<ip>:8080/mcp
I (XXX) wave_rover: boot complete. MCP at http://<AP>:8080/mcp
```

- [ ] **Step 5: Test with curl (from client on same Wi-Fi)**

```bash
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1.0"}}}'
```

Expected: JSON response with `{"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2024-11-05","capabilities":{"tools":{}},...}}`

```bash
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
```

Expected: JSON with 16+ tools listed.

- [ ] **Step 6: Commit**

```bash
git add application/wave_rover/main/app_main.c
git commit -m "feat(wave_rover): wire HAL+WiFi+MCP in app_main"
```

---

## Task 10 — OLED font rendering (real display backend)

**Files:**
- Modify: `application/wave_rover/components/wave_rover_hal/wave_rover_display.c`

- [ ] **Step 1: Add minimal 5×8 font table** to `wave_rover_display.c`

Replace the `/* TODO: render with font */` stub with a minimal font renderer. The 5×8 font data below covers ASCII 32–95 (space, numbers, uppercase letters):

```c
/* 5×8 font: each char = 5 bytes (columns), MSB at top */
static const uint8_t s_font5x8[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 space */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 ! */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 $ */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 % */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 & */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* 42 * */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 + */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 , */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 - */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 . */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 : */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ; */
    {0x08,0x14,0x22,0x41,0x00}, /* 60 < */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 = */
    {0x00,0x41,0x22,0x14,0x08}, /* 62 > */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 A */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 B */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 D */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 E */
    {0x7F,0x09,0x09,0x09,0x01}, /* 70 F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 71 G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 H */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 I */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 J */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 K */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 77 M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 O */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 R */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 S */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 87 W */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 X */
    {0x07,0x08,0x70,0x08,0x07}, /* 89 Y */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 Z */
};

static void ssd_set_cursor(int col, int page)
{
    ssd_cmd(0xB0 | (page & 0x07));
    ssd_cmd(0x00 | (col & 0x0F));
    ssd_cmd(0x10 | ((col >> 4) & 0x0F));
}

static void ssd_print_char(uint8_t c)
{
    if (c < 32 || c > 90) c = 32; /* clamp to font range */
    const uint8_t *glyph = s_font5x8[c - 32];
    uint8_t buf[7] = {SSD_DATA, glyph[0], glyph[1], glyph[2], glyph[3], glyph[4], 0x00};
    i2c_master_transmit(s_oled_dev, buf, 7, 50);
}
```

Replace the stub `wr_display_text` body with:
```c
esp_err_t wr_display_text(const char *text, int line, bool clear_first)
{
    if (g_wr_dry_run || !g_wr_i2c_bus) {
        ESP_LOGD(TAG, "dry-run display[%d]: %s", line, text ? text : "");
        return ESP_OK;
    }
    if (!s_oled_dev) {
        if (oled_init() != ESP_OK) return ESP_OK;
    }
    if (clear_first) wr_display_clear();
    if (!text) return ESP_OK;

    int page = line & 0x03;
    ssd_set_cursor(0, page);
    for (int i = 0; text[i] && i < 21; i++) {
        ssd_print_char((uint8_t)text[i]);
    }
    return ESP_OK;
}
```

- [ ] **Step 2: Build**

```bash
cd application/wave_rover && idf.py build
```

- [ ] **Step 3: Commit**

```bash
git add application/wave_rover/components/wave_rover_hal/wave_rover_display.c
git commit -m "feat(wave_rover): add SSD1306 5x8 font renderer"
```

---

## Task 11 — Protocol compatibility docs

**Files:**
- Create: `docs/protocol_compatibility.md`

- [ ] **Step 1: Create the file**

```markdown
# Wave Rover Protocol Compatibility

## Reference JSON protocol vs MCP tools

| JSON cmd | T | Format | MCP equivalent |
|---|---|---|---|
| Speed ctrl | 1 | `{"T":1,"L":f,"R":f}` | `rover.drive_tank` with L/R ∈[-1,1] |
| PWM input | 11 | `{"T":11,"L":i,"R":i}` | `rover.drive_tank` (scale to [-1,1]) |
| ROS ctrl | 13 | `{"T":13,"X":mps,"Z":radps}` | `rover.move` linear/angular |
| OLED ctrl | 3 | `{"T":3,"lineNum":n,"Text":"..."}` | `rover.display_text` |
| Get IMU | 126 | `{"T":126}` | `rover.get_imu` |
| Calibrate IMU | 127 | `{"T":127}` | `rover.calibrate_imu` (stub) |
| Get IMU offset | 128 | `{"T":128}` | (not mapped in MVP) |
| Set IMU offset | 129 | `{"T":129,...}` | (not mapped in MVP) |

## Transport compatibility

Reference firmware transports (all still functional if running original firmware):
- UART0 @ 115200 baud (JSON over serial)
- USB-Serial (same as UART0)
- HTTP POST `/js?json=<cmd>` (Wi-Fi AP/STA)
- ESP-NOW broadcast

Our firmware replaces the above with:
- HTTP POST `/mcp` (JSON-RPC 2.0, port 8080)

The two firmwares are mutually exclusive (only one can run at a time).

## Motor control mapping

Reference: `setGoalSpeed(L, R)` where L/R ∈ [-1,1], WAVE ROVER mainType=1 → direct PWM = L*512*spd_rate.
Our HAL: `wr_motor_set(left, right)` where left/right ∈ [-1,1] → PWM = round(|v|*255).

Note: reference firmware multiplies by 512 (using 8-bit max 255 → effectively capped). Our formula
uses 255 directly, giving slightly lower max speed (same hardware limit, different scaling).
Adjust `max_speed` config (default 0.4) to match desired behaviour.

## Heartbeat / watchdog

Reference firmware: 3000ms no-command → motors stop (heartBeatCtrl).
Our firmware: 10000ms HTTP idle → motors stop (keepalive timer in wave_rover_mcp.c).
Adjust `HEART_BEAT_DELAY` in wave_rover_mcp.c line ~42 if needed.
```

- [ ] **Step 2: Commit**

```bash
git add docs/protocol_compatibility.md
git commit -m "docs(wave_rover): add protocol compatibility notes"
```

---

## Task 12 — Smoke tests

**Files:**
- Create: `tools/mcp_smoke_test.py`
- Create: `tools/rover_cli.py`

- [ ] **Step 1: Create `tools/mcp_smoke_test.py`**

```python
#!/usr/bin/env python3
"""Wave Rover MCP smoke test. Run against a live device."""
import argparse, json, sys, time
import urllib.request, urllib.error

def mcp_call(host, port, method, params=None):
    url = f"http://{host}:{port}/mcp"
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                       "params": params or {}}).encode()
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())
    except urllib.error.URLError as e:
        return {"error": str(e)}

def check(label, resp, key=None):
    ok = "result" in resp and (key is None or key in resp["result"])
    status = "PASS" if ok else "FAIL"
    print(f"  [{status}] {label}")
    if not ok:
        print(f"         got: {resp}")
    return ok

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host",  default="192.168.4.1")
    p.add_argument("--port",  type=int, default=8080)
    p.add_argument("--allow-motion", action="store_true")
    args = p.parse_args()

    print(f"\n=== Wave Rover MCP Smoke Test ({args.host}:{args.port}) ===\n")
    passed = failed = 0

    tests = [
        ("initialize", "initialize",
         {"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"smoke","version":"1"}}),
        ("ping",       "ping",       {}),
        ("tools/list", "tools/list", {}),
        ("resources/list", "resources/list", {}),
    ]
    for label, method, params in tests:
        r = mcp_call(args.host, args.port, method, params)
        if check(label, r):
            passed += 1
        else:
            failed += 1

    tool_tests = [
        ("rover.get_status",    "rover.get_status",    {}),
        ("rover.get_power",     "rover.get_power",     {}),
        ("rover.get_ups",       "rover.get_ups",       {}),
        ("rover.get_imu",       "rover.get_imu",       {}),
        ("rover.display_status","rover.display_status",{}),
        ("rover.stop",          "rover.stop",          {"reason":"smoke_test"}),
    ]
    for label, tool_name, args_j in tool_tests:
        r = mcp_call(args.host, args.port, "tools/call",
                     {"name": tool_name, "arguments": args_j})
        if check(label, r):
            passed += 1
        else:
            failed += 1

    if args.allow_motion:
        r = mcp_call(args.host, args.port, "tools/call",
                     {"name": "rover.move",
                      "arguments": {"linear": 0.2, "angular": 0, "duration_ms": 200}})
        if check("rover.move (motion allowed)", r):
            passed += 1
        else:
            failed += 1
    else:
        print("  [SKIP] rover.move (pass --allow-motion to test real motion)")

    print(f"\n=== Results: {passed} passed, {failed} failed ===\n")
    sys.exit(0 if failed == 0 else 1)

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Create `tools/rover_cli.py`**

```python
#!/usr/bin/env python3
"""Wave Rover CLI. Never sends motion commands without --allow-motion."""
import argparse, json, sys
import urllib.request, urllib.error

def mcp_call(host, port, method, params):
    url = f"http://{host}:{port}/mcp"
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                       "params": params}).encode()
    req = urllib.request.Request(url, data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())

def tool_call(host, port, tool, arguments):
    return mcp_call(host, port, "tools/call", {"name": tool, "arguments": arguments})

def pretty(resp):
    if "result" in resp:
        print(json.dumps(resp["result"], indent=2))
    elif "error" in resp:
        print(f"ERROR: {resp['error']}", file=sys.stderr)
        sys.exit(1)
    else:
        print(json.dumps(resp, indent=2))

def main():
    p = argparse.ArgumentParser(description="Wave Rover CLI")
    p.add_argument("--host", default="wave-rover.local")
    p.add_argument("--port", type=int, default=8080)
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    sub.add_parser("power")
    sub.add_parser("imu")
    sub.add_parser("display-status")
    stp = sub.add_parser("stop")
    stp.add_argument("--reason", default="cli")
    est = sub.add_parser("emergency-stop")
    est.add_argument("--reason", default="cli")
    clr = sub.add_parser("clear-estop")

    mv = sub.add_parser("move")
    mv.add_argument("--linear",      type=float, default=0.0)
    mv.add_argument("--angular",     type=float, default=0.0)
    mv.add_argument("--duration-ms", type=int,   default=500)
    mv.add_argument("--allow-motion", action="store_true")

    tank = sub.add_parser("tank")
    tank.add_argument("--left",       type=float, default=0.0)
    tank.add_argument("--right",      type=float, default=0.0)
    tank.add_argument("--duration-ms",type=int,   default=500)
    tank.add_argument("--allow-motion",action="store_true")

    args = p.parse_args()
    host, port = args.host, args.port

    if args.cmd == "status":
        pretty(tool_call(host, port, "rover.get_status", {}))
    elif args.cmd == "power":
        pretty(tool_call(host, port, "rover.get_power", {}))
    elif args.cmd == "imu":
        pretty(tool_call(host, port, "rover.get_imu", {}))
    elif args.cmd == "display-status":
        pretty(tool_call(host, port, "rover.display_status", {}))
    elif args.cmd == "stop":
        pretty(tool_call(host, port, "rover.stop", {"reason": args.reason}))
    elif args.cmd == "emergency-stop":
        pretty(tool_call(host, port, "rover.emergency_stop", {"reason": args.reason}))
    elif args.cmd == "clear-estop":
        pretty(tool_call(host, port, "rover.clear_emergency_stop", {"confirm": True}))
    elif args.cmd == "move":
        if not args.allow_motion:
            print("ERROR: pass --allow-motion to send movement commands", file=sys.stderr)
            sys.exit(1)
        pretty(tool_call(host, port, "rover.move", {
            "linear": args.linear, "angular": args.angular,
            "duration_ms": args.duration_ms}))
    elif args.cmd == "tank":
        if not args.allow_motion:
            print("ERROR: pass --allow-motion to send movement commands", file=sys.stderr)
            sys.exit(1)
        pretty(tool_call(host, port, "rover.drive_tank", {
            "left": args.left, "right": args.right,
            "duration_ms": args.duration_ms}))

if __name__ == "__main__":
    main()
```

- [ ] **Step 3: Test smoke test locally (dry-run device)**

```bash
python3 tools/mcp_smoke_test.py --host 192.168.4.1 --port 8080
```

Expected:
```
=== Wave Rover MCP Smoke Test (192.168.4.1:8080) ===

  [PASS] initialize
  [PASS] ping
  [PASS] tools/list
  [PASS] resources/list
  [PASS] rover.get_status
  [PASS] rover.get_power
  [PASS] rover.get_ups
  [PASS] rover.get_imu
  [PASS] rover.display_status
  [PASS] rover.stop

=== Results: 10 passed, 0 failed ===
```

- [ ] **Step 4: Commit**

```bash
git add tools/mcp_smoke_test.py tools/rover_cli.py
git commit -m "feat(wave_rover): add MCP smoke test and rover CLI"
```

---

## Task 13 — Documentation

**Files:**
- Create: `docs/platformio.md`
- Create: `docs/quickstart.md`
- Create: `docs/mcp_api.md`
- Create: `docs/safety.md`
- Create: `docs/troubleshooting.md`

- [ ] **Step 1: Create `docs/platformio.md`**

```markdown
# PlatformIO Build Guide

## Requirements

- Python 3.8+
- PlatformIO Core (`pip install platformio`)
- ESP-IDF 5.5+ (used internally by PlatformIO `espidf` framework)

## Build

```bash
cd application/wave_rover
pio run
```

## Flash

```bash
pio run -t upload
```

## Monitor

```bash
pio device monitor
```

## Environment

| Key | Value |
|---|---|
| Platform | espressif32 |
| Board | esp32dev |
| Framework | espidf |
| Flash mode | dio |
| Monitor speed | 115200 |
| Upload speed | 921600 |

## Notes

- `idf.py` is the underlying build tool. Run `pio run` — never `idf.py` directly.
- First build downloads managed components (`espressif/mcp-c-sdk`, `espressif/mdns`).
- `sdkconfig.defaults` sets Wi-Fi + HTTP server sizes. Do not edit `sdkconfig` directly.
```

- [ ] **Step 2: Create `docs/quickstart.md`**

````markdown
# Wave Rover MCP Quick Start

## 1. Build and flash

```bash
cd application/wave_rover
pio run -t upload
pio device monitor
```

## 2. Connect to rover Wi-Fi

Default AP: `WR-ESP32` / password `12345678`

## 3. Test MCP endpoint

```bash
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/list",
    "params": {}
  }'
```

```bash
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/call",
    "params": {
      "name": "rover.get_status",
      "arguments": {}
    }
  }'
```

```bash
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/call",
    "params": {
      "name": "rover.stop",
      "arguments": { "reason": "manual test" }
    }
  }'
```

## 4. Use rover CLI

```bash
python3 tools/rover_cli.py --host 192.168.4.1 status
python3 tools/rover_cli.py --host 192.168.4.1 power
python3 tools/rover_cli.py --host 192.168.4.1 imu
python3 tools/rover_cli.py --host 192.168.4.1 display-status
python3 tools/rover_cli.py --host 192.168.4.1 stop
python3 tools/rover_cli.py --host 192.168.4.1 move --linear 0.2 --angular 0 --duration-ms 500 --allow-motion
```

## 5. Switch to STA mode

```bash
python3 tools/rover_cli.py --host 192.168.4.1 -- set_wifi (use curl directly)
```

Or via curl:
```bash
curl -s http://192.168.4.1:8080/mcp \
  -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"rover.set_wifi","arguments":{"mode":"sta","ssid":"YourSSID","password":"YourPass","save":true}}}'
# Then reboot the rover. Do NOT log or share this command with the password visible.
```
````

- [ ] **Step 3: Create `docs/safety.md`**

```markdown
# Wave Rover Safety Requirements

## Movement safety

1. All movement commands MUST include `duration_ms` (1–3000 ms).
2. After `duration_ms` motors stop automatically.
3. If emergency stop is active, all movement commands are rejected.
4. If low battery is detected, log warning and reject movement.
5. If heap < 20 KB or brownout detected, stop motors.
6. At startup, motors are always stopped.
7. At MCP server idle >10s, motors are stopped (keepalive timer).
8. `rover.clear_emergency_stop` does NOT start movement; it only clears the flag.

## Emergency stop

- Call `rover.emergency_stop` to latch motors off.
- No movement possible until `rover.clear_emergency_stop` with `confirm: true`.

## Rate limiting

- `rover.move` / `rover.drive_tank` / `rover.turn` block the HTTP thread during `duration_ms`.
- Max `duration_ms` = 3000 ms (configurable via `max_command_duration_ms`).
- Max speed = 0.4 (configurable via `max_speed`).

## Dry-run mode

Default: `dry_run = true`. No hardware is accessed; all motor/sensor calls are no-ops.
Set `dry_run = false` in NVS config only after hardware is confirmed.
```

- [ ] **Step 4: Create `docs/mcp_api.md`** — brief tool/resource reference (2 columns per tool from spec)

(Summarise the tool list with parameters; defer to spec document for full JSON schemas.)

- [ ] **Step 5: Create `docs/troubleshooting.md`** — common issues

```markdown
# Troubleshooting

## Build fails: mcp-c-sdk not found
Run `pio run` from `application/wave_rover/` — it fetches managed components automatically.
Do not run `idf.py` without first sourcing `$IDF_PATH/export.sh`.

## OLED not displaying
Check I2C SDA=32, SCL=33 connections. Verify address 0x3C with `i2cdetect`.
Set `dry_run=false` in NVS and reboot.

## INA219 shows 0V
Check I2C address 0x42. Confirm 3S battery connected. Shunt = 0.01 Ω.

## IMU returns present=false
QMI8658 at 0x6B. Confirm I2C connections. WHO_AM_I should return 0x05.

## Motors not moving
1. Verify `dry_run = false` in NVS config.
2. Check GPIO 17,21,22,23,25,26 not reassigned.
3. Check emergency stop state: call `rover.get_status`.
4. Check low battery flag.

## curl: Connection refused
Rover not on Wi-Fi. Connect to AP `WR-ESP32` and use IP `192.168.4.1`.
Or check STA connection in serial monitor.
```

- [ ] **Step 6: Commit**

```bash
git add docs/platformio.md docs/quickstart.md docs/mcp_api.md \
        docs/safety.md docs/troubleshooting.md
git commit -m "docs(wave_rover): add quickstart, safety, platformio, troubleshooting"
```

---

## Self-Review Checklist

### Spec coverage

| Spec requirement | Task(s) |
|---|---|
| PlatformIO build | Task 3 |
| Wi-Fi AP+STA | Task 6 |
| MCP server on-device port 8080 | Task 8 |
| initialize, ping, tools/list, tools/call | Task 8 (SDK) |
| resources/list, resources/read | Task 8 (custom HTTP handler) |
| rover.get_status | Task 8 tools |
| rover.get_config | Task 8 tools |
| rover.move / drive_tank / turn / stop / emergency_stop / clear_emergency_stop | Task 8 tools |
| rover.get_power / get_ups (INA219) | Task 7, Task 8 |
| rover.get_imu (QMI8658+AK09918) | Task 7, Task 8 |
| rover.display_text/clear/status (SSD1306) | Task 7, Task 10 |
| rover.set_wifi / get_wifi | Task 8 tools |
| NVS config with no password logging | Task 5 |
| dry_run mode by default | Task 5, Task 7 |
| max_speed limit | Task 8 tools |
| duration_ms required for all movement | Task 8 tools |
| emergency_stop + clear | Task 7, Task 8 |
| Safety: startup motors stop | Task 9 |
| Safety: keepalive/watchdog | Task 8 (timer) |
| board_config.h with confirmed GPIO | Task 7 |
| docs/references.md | Task 1 |
| docs/reference_firmware_analysis.md | Task 2 |
| platformio.md | Task 13 |
| quickstart.md | Task 13 |
| smoke tests | Task 12 |
| rover_cli.py | Task 12 |
| protocol_compatibility.md | Task 11 |
| prompts (rover_diagnostics etc.) | NOT supported by mcp-c-sdk v1.x — noted in Task 8 architecture |

### Known gaps / next steps

1. **rover.calibrate_imu** — stub only; needs real QMI8658 still-calibration (5s wait).
2. **resources content** for `rover://config` / `rover://wifi` — currently returns motor state. Wire to config object.
3. **OLED font** — only ASCII 32–90 in Task 10. Lowercase/special chars render as space.
4. **Low battery enforcement** in movement tools — Task 8 logs warning but does not yet block movement. Add `ps.low_battery` check in tool_move/drive_tank/turn.
5. **tools/mcp_api.md** — left as stub in Task 13. Fill in per-tool parameter tables.
6. **MCP prompts** — `espressif/mcp-c-sdk v1.x` does not support prompts. Implement prompts as tools that return prompt text if needed.
7. **Motor worker task** — implemented in Task 7b. Callbacks submit via `wr_motor_submit_and_wait`. Emergency stop is checked every 50ms in the worker loop and preempts any move command.
