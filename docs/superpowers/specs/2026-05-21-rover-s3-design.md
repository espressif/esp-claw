# rover_s3 Design Spec

**Date:** 2026-05-21
**Target hardware:** M5Stack StickS3 (ESP32-S3-PICO-1-N8R8, 8 MB PSRAM, 8 MB flash, ST7789P3 135×240, BMI270, ES8311 codec + MEMS mic + speaker, GPIO 11/12 buttons)
**Predecessor:** `application/rover_demo` (M5StickC Plus, ESP32-PICO-D4, no PSRAM)

---

## Goal

Port the AI rover to StickS3 with full voice control: always-listening wake word, hybrid on-device / cloud STT, and TTS responses through the built-in speaker. Telegram interface is retained in parallel.

---

## Architecture

### New directories

```
application/rover_s3/                     ← new application
components/claw_capabilities/cap_voice/   ← new reusable capability
```

`cap_rover` and `cap_unitv` are reused unchanged.

### Dual input channels → single event router

```
Microphone → cap_voice  → claw_event_t("voice_input")  ┐
Telegram   → cap_im_tg  → claw_event_t("tg_message")   ┤→ claw_event_router
                                                         ↓
                                                     claw_core (LLM agent)
                                                         ↓
                                              cap_rover / cap_unitv
```

Reply channel is carried in `claw_event_t.target_channel`. Voice commands get TTS responses; Telegram commands get `tg_send_message` responses.

---

## cap_voice Component

### Source layout

```
components/claw_capabilities/cap_voice/
  CMakeLists.txt
  idf_component.yml
  include/cap_voice.h
  src/
    cap_voice.c              ← public API, capability group registration
    cap_voice_audio.c        ← I2S capture + ES8311 via esp_codec_dev, I2S playback
    cap_voice_pipeline.c     ← main FreeRTOS task: WakeNet → VAD → MultiNet/Whisper
    cap_voice_whisper.c      ← HTTP multipart/form-data → Whisper API → text
    cap_voice_tts.c          ← OpenAI TTS API → PCM buffer → playback
  Kconfig                    ← wake word model selection, buffer sizes
  skills/
    cap_voice/SKILL.md
    skills_list.json
```

### Audio driver

Raw I2S without ESP-ADF. ES8311 codec managed via `esp_codec_dev` (Espressif managed component). Capture: 16 kHz, 16-bit, mono (required by WakeNet/MultiNet/Whisper). Playback: 24 kHz, 16-bit, mono (matches OpenAI TTS PCM output). ES8311 sample rate is switched between capture and playback modes.

Wake word: **"Hi ESP"** (built-in WakeNet v2 English model). Configurable via Kconfig `CONFIG_CAP_VOICE_WAKEWORD_MODEL`.

### Voice pipeline state machine

The pipeline runs in a dedicated FreeRTOS task at priority 5.

```
IDLE
  └─ I2S stream → WakeNet (always running)
  └─ wake word confidence > wake_sensitivity threshold
       ↓
LISTENING
  └─ record into ring buffer (max 10 s)
  └─ VAD: ≥ 1.5 s silence detected
       ↓
RECOGNIZING
  ├─ MultiNet inference (< 50 ms, on-device)
  │    confidence > multinet_threshold (default 0.85)
  │       → publish claw_event_t { content_type="direct", text=<command> }
  │    confidence ≤ threshold
  │       ↓
  └─ Whisper API
       → encode buffer as 16 kHz WAV
       → HTTP POST multipart/form-data to /v1/audio/transcriptions
       → publish claw_event_t { source_channel="voice", text=<transcript> }
       ↓
SPEAKING (entered when TTS response arrives)
  └─ play PCM through ES8311
  └─ return to IDLE
```

Display state mirrors pipeline: `IDLE` → `LISTENING` → `THINKING` → `SPEAKING`.

### MultiNet fast-path commands (v1)

| Spoken phrase | Direct cap call |
|---|---|
| stop | `cap_rover.rover_stop` |
| forward | `cap_rover.rover_move` (x=0.5, duration=1 s) |
| back | `cap_rover.rover_move` (x=-0.5, duration=1 s) |
| left | `cap_rover.rover_move` (y=0.5, duration=1 s) |
| right | `cap_rover.rover_move` (y=-0.5, duration=1 s) |
| open gripper | `cap_rover.rover_gripper_open` |
| close gripper | `cap_rover.rover_gripper_close` |

Fast-path events carry `content_type="direct"` which the router maps directly to a cap call, bypassing the LLM agent.

### LLM-path (Whisper fallback)

Transcribed text is published as a normal `claw_event_t` with `source_channel="voice"`. The router submits it to `claw_core`. The LLM agent uses `cap_rover`, `cap_unitv`, and `cap_skill` tools exactly as in rover_demo. The agent's text response is sent to `cap_voice_tts` for playback.

### TTS

OpenAI `/v1/audio/speech` endpoint. Response format: `pcm` at 24 kHz (OpenAI default for PCM). Streamed into a PSRAM-backed heap buffer then played through ES8311 at 24 kHz in one shot. Voice configurable via `tts_voice` NVS key (default: `alloy`).

### LLM-callable tools exposed by cap_voice

| Tool ID | Description |
|---|---|
| `voice_say` | Speak a string through TTS immediately |
| `voice_set_voice` | Change TTS voice for current session |

---

## application/rover_s3

### Directory layout

```
application/rover_s3/
  boards/m5sticks3/
    setup_device.cpp           ← M5Unified init: display, IMU, battery, buttons (GPIO 11, 12)
    setup_device.h
    sdkconfig.defaults.board   ← IDF_TARGET=esp32s3, octal PSRAM, display GPIO
    board_devices.yaml
    board_info.yaml
    board_peripherals.yaml
  main/
    main.c
    app_rover_s3.c/h           ← wires all caps + claw_core
    rover_s3_display.cpp/h     ← display state machine (7 states, M5Unified GFX)
    rover_s3_settings.c/h      ← NVS read/write helpers
    rover_s3_wifi.c/h          ← WiFi init + reconnect
    rover_s3_cli.c/h           ← UART console commands
    CMakeLists.txt
    Kconfig.projbuild
    idf_component.yml
  fatfs_image/
    memory/                    ← identity.md, soul.md, etc.
    skills/                    ← rover_ops.md, rover_search.md, skills_list.json
    router_rules/router_rules.json
    sessions/
    inbox/
  sdkconfig.defaults           ← S3 + PSRAM base config
  partitions.csv               ← 8 MB layout
  platformio.ini               ← env:m5sticks3
  idf_component.yml
```

### Display states

`BOOT` → `IDLE` → `LISTENING` → `THINKING` → `SPEAKING` → `EXECUTING` → `OFFLINE`

Animations are drawn with M5Unified GFX on the 135×240 ST7789P3 display. `LISTENING` shows a pulsing microphone icon; `SPEAKING` shows a waveform.

### sdkconfig.defaults (base, S3-specific)

```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_SPIRAM_BOOT_INIT=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_MBEDTLS_DYNAMIC_BUFFER=n
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=16384
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="8MB"
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_API_ENCODING_UTF_8=y
CONFIG_ESP_TASK_WDT_TIMEOUT_S=30
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y
```

### partitions.csv (8 MB)

| Name | Type | SubType | Offset | Size |
|------|------|---------|--------|------|
| nvs | data | nvs | 0x9000 | 24 K |
| phy_init | data | phy | 0xf000 | 4 K |
| factory | app | factory | 0x10000 | 3 MB |
| fatfs | data | fat | 0x310000 | 4 MB |
| coredump | data | coredump | 0x710000 | 64 K |

### platformio.ini

```ini
[platformio]
default_envs = m5sticks3
src_dir = main

[env:m5sticks3]
platform = espressif32
board = m5stack-stamps3    ; or custom board JSON
framework = espidf
monitor_speed = 115200
upload_speed = 1500000
board_build.partitions = partitions.csv
board_build.flash_mode = qio
board_build.f_flash = 80000000L
board_upload.flash_size = 8MB
extra_scripts =
    pre:scripts/pio_fatfs.py
build_flags =
    -DCORE_DEBUG_LEVEL=3
    -DBOARD_HAS_PSRAM
monitor_filters =
    esp32_exception_decoder
    time
```

---

## Router Rules

Four rules in `fatfs_image/router_rules/router_rules.json`:

1. **Voice direct command** — `source_channel=voice AND content_type=direct` → execute cap directly (no LLM)
2. **Voice LLM command** — `source_channel=voice` → `submit_to_agent`, `reply_channel=voice`
3. **Telegram command** — `source_channel=telegram` → `submit_to_agent`, `reply_channel=telegram`
4. **Outbound message** — `event_type=out_message AND target_channel=telegram` → `cap_im_tg.tg_send_message`

---

## Settings

All stored in NVS namespace `rover_s3`.

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `wifi_ssid` | str | — | WiFi SSID |
| `wifi_pass` | str | — | WiFi password |
| `tg_token` | str | — | Telegram bot token |
| `llm_base_url` | str | — | OpenAI-compatible base URL |
| `llm_api_key` | str | — | LLM API key |
| `llm_model` | str | — | LLM model name |
| `whisper_api_key` | str | =llm_api_key | Whisper API key (shares LLM key by default) |
| `tts_api_key` | str | =llm_api_key | TTS API key |
| `tts_voice` | str | `alloy` | OpenAI TTS voice |
| `wake_sensitivity` | float | `0.7` | WakeNet confidence threshold |
| `multinet_threshold` | float | `0.85` | MultiNet fast-path confidence threshold |
| `voice_enabled` | bool | `true` | Enable/disable voice pipeline |

---

## CLI Commands

Accessible via UART console (115200 baud):

| Command | Description |
|---------|-------------|
| `settings` | Print all NVS settings |
| `set <key> <value>` | Update a setting |
| `voice_test` | Speak a test phrase via TTS |
| `voice_listen` | Force LISTENING state |
| `whisper_test <path>` | Send a WAV file from FATFS to Whisper, print result |
| `rover_move` / `rover_stop` | Direct rover control (same as rover_demo) |
| `wifi_scan` | Scan and list nearby networks |
| `heap` | Print free heap and largest block |

---

## Out of Scope (v1)

- Continuous conversation without wake word (always-on VAD)
- Multiple wake words
- On-device TTS (too limited in quality)
- BLE or web config UI (can be added later from edge_agent)
- OTA updates
