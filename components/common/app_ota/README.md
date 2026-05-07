# app_ota Usage Guide

`app_ota` is an application OTA component built on top of `espressif/esp_ota_service`.
It provides two boot-time upgrade paths:

- HTTP OTA: after STA is connected, pull firmware via manifest + firmware URL.
- Filesystem OTA: at boot, read firmware from an absolute local path (for example `/fatfs/ota/firmware.bin` or `/sdcard/firmware.bin`).

Component dependency is declared in `idf_component.yml`:

- `espressif/esp_ota_service: "*"`

## 1. Public API

Header: `include/app_ota.h`

- `app_ota_fs_boot_flow_at(const char *firmware_abs_path)`
  - Run one FS OTA attempt using a specific absolute path (for example `/sdcard/firmware.bin`).
- `app_ota_fs_boot_flow(void)`
  - Run one FS OTA attempt using `CONFIG_APP_OTA_FS_FIRMWARE_ABS_PATH`.
- `app_ota_http_boot_flow(bool sta_station_connected_to_ap)`
  - Handle rollback pending-verify status, then run one HTTP OTA attempt when config is valid.

Note: on successful upgrade the device reboots. With rollback enabled, confirm/rollback depends on network state.

## 2. Menuconfig Options

Path: `Component config -> App OTA (esp_ota_service)`

### HTTP OTA Key Options

- `CONFIG_APP_OTA_HTTP_ENABLE`
- `CONFIG_APP_OTA_HTTP_RUN_AT_BOOT`
- `CONFIG_APP_OTA_HTTP_MANIFEST_URL`
- `CONFIG_APP_OTA_HTTP_FIRMWARE_URL`
- `CONFIG_APP_OTA_HTTP_CHUNK_TIMEOUT_MS`

Notes:

- HTTP OTA runs only when `ENABLE=y` and both URLs are non-empty.
- Version comparison is handled by manifest checker and requires candidate version > running version.

### Filesystem OTA Key Options

- `CONFIG_APP_OTA_FS_ENABLE`
- `CONFIG_APP_OTA_FS_RUN_AT_BOOT`
- `CONFIG_APP_OTA_FS_FIRMWARE_ABS_PATH` (default `/fatfs/ota/firmware.bin`)
- `CONFIG_APP_OTA_FS_TRY_SDCARD_AT_BOOT`
- `CONFIG_APP_OTA_FS_SDCARD_FIRMWARE_ABS_PATH` (default `/sdcard/firmware.bin`)
- `CONFIG_APP_OTA_FS_READ_BUF_BYTES`
- `CONFIG_APP_OTA_FS_REQUIRE_NEWER_SEMVER`
- `CONFIG_APP_OTA_FS_CHECK_PROJECT_NAME`

## 3. Recommended Integration Order

Reference call sequence in `application/edge_agent/main/main.c`:

1. After `esp_board_manager_init()`, if SD-first path is enabled, call
   `app_ota_fs_boot_flow_at(CONFIG_APP_OTA_FS_SDCARD_FIRMWARE_ABS_PATH)`.
2. After `init_fatfs()`, call `app_ota_fs_boot_flow()` for SPI FAT path.
3. After Wi-Fi starts and STA status is known, call `app_ota_http_boot_flow(sta_connected)`.

This implements a "local-media first, network OTA second" strategy.

## 4. HTTP OTA Manifest

Manifest should include at least:

- `version` (semver string, for example `1.2.3`)
- `project_name` (optional check)
- `size` (firmware size)
- `sha256` (optional but recommended)

Firmware URL must point to downloadable app binary (for example `edge_agent.bin`).

## 5. SDCard OTA Preparation

For SD boot OTA, make sure:

- Upgrade firmware has been manually copied to SD card and named `firmware.bin` at `/sdcard/firmware.bin` (or match your configured path).
- `CONFIG_APP_OTA_FS_TRY_SDCARD_AT_BOOT=y`.
- Configured path matches actual mount/path on target.

## 6. Manual Step-by-Step Validation

### 6.1 SDCard OTA (Manual)

1. Build upgrade firmware (higher semver)
   - Set `version.txt` to upgrade version (for example `1.2.4`), then:
   - `idf.py -B build_sd_upgrade build`
   - Output: `build_sd_upgrade/edge_agent.bin`
2. Manually copy upgrade firmware to SD card
   - Copy `build_sd_upgrade/edge_agent.bin` to SD card and rename it to `firmware.bin`.
   - Ensure it is accessible by `CONFIG_APP_OTA_FS_SDCARD_FIRMWARE_ABS_PATH` (default `/sdcard/firmware.bin`).
3. Build and flash baseline firmware (lower semver)
   - Set `version.txt` to baseline version (for example `1.2.3`), then:
   - `idf.py -B build_sd_base build flash -p /dev/ttyUSB3`
   - For first-time verification, it is recommended to run
     `idf.py -B build_sd_base erase-flash -p /dev/ttyUSB3` once.
4. Check serial logs
   - Look for: SD firmware detected, OTA starts, `OTA finished`, reboot, and upgraded version string.

### 6.2 HTTP OTA (Using esp_ota_service tools, Manual)

1. Build upgrade firmware (higher semver)
   - Set `version.txt` to upgrade version (for example `1.2.4`):
   - `idf.py -B build_http_upgrade build`
   - Output: `build_http_upgrade/edge_agent.bin`
2. Prepare `firmware_samples` in `esp_ota_service/tools`
   - Directory: `application/edge_agent/managed_components/espressif__esp_ota_service/tools/firmware_samples`
   - Copy firmware there, for example `edge_agent_v1.2.4.bin`
   - Create/update `manifest.json` with at least `version`, `size`, `sha256`, and `url`
3. Start HTTP server with OTA service tool script
   - Script: `ota_http_serve.py`
   - Example:
     ```bash
     python3 application/edge_agent/managed_components/espressif__esp_ota_service/tools/ota_http_serve.py \
       --bin edge_agent_v1.2.4.bin \
       --port 18070 \
       --update-manifest-url
     ```
4. Build and flash baseline firmware (lower semver)
   - Set `version.txt` to baseline version (for example `1.2.3`)
   - In menuconfig/sdconfig set:
     - `CONFIG_APP_OTA_HTTP_ENABLE=y`
     - `CONFIG_APP_OTA_HTTP_RUN_AT_BOOT=y`
     - `CONFIG_APP_OTA_HTTP_MANIFEST_URL=http://<HOST_IP>:18070/manifest.json`
     - `CONFIG_APP_OTA_HTTP_FIRMWARE_URL=http://<HOST_IP>:18070/firmware.bin`
   - Then run: `idf.py -B build_http_base build flash -p /dev/ttyUSB3`
5. Check serial logs
   - Look for: HTTP download succeeds, version check passes, `OTA finished`, reboot, and upgraded version.

Tip: `erase-flash` is optional, but recommended for first-time verification or when switching configs to reduce NVS state interference.

