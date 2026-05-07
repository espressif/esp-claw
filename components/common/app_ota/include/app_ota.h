/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Runs after Wi-Fi manager has settled (STA/AP). When rollback is enabled, confirms or rolls back a
 * pending-verify boot; when HTTP OTA is enabled and URLs are set, may start a manifest-driven session.
 *
 * @param sta_station_connected_to_ap Whether STA has associated and has usable IP context (for confirm vs rollback).
 */
esp_err_t app_ota_http_boot_flow(bool sta_station_connected_to_ap);

/**
 * After FATFS (e.g. /fatfs) is mounted, optionally streams app firmware from a VFS path (typically
 * CONFIG_APP_OTA_FS_FIRMWARE_ABS_PATH) via esp_ota_service_source_fs. May reboot without returning.
 */
esp_err_t app_ota_fs_boot_flow(void);

/**
 * Same as app_ota_fs_boot_flow() but uses an explicit absolute VFS path (e.g. /sdcard/firmware.bin).
 * Used for SD-staged images before SPI flash FAT is mounted when CONFIG_APP_OTA_FS_TRY_SDCARD_AT_BOOT is enabled.
 */
esp_err_t app_ota_fs_boot_flow_at(const char *firmware_abs_path);

#ifdef __cplusplus
}
#endif
