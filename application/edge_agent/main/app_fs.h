/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Active base path of the writable fatfs storage.
 *
 *         The on-flash storage mount point when flash is in use, or the SD
 *         card's own mount point when an SD card is in use. Valid only after
 *         app_fs_init() has run.
 *
 * @return Mount-point string owned by this module (never NULL).
 */
const char *app_fs_storage_base_path(void);

/**
 * @brief  Base path of the read-only system filesystem.
 *
 *         Holds firmware-baked content (skills, built-in Lua scripts/docs and
 *         the recovery seed files). Valid only after app_fs_init() has run.
 *
 * @return Mount-point string owned by this module (never NULL).
 */
const char *app_fs_system_base_path(void);

/**
 * @brief  Initialize all filesystems.
 *
 *         Must be called after esp_board_manager_init() since it relies on the
 *         board manager to detect and mount an SD card if present. The system
 *         partition is mounted first, followed by the storage (SD card or flash)
 *         and then the RAMFS.
 *
 * @return
 *       - ESP_OK  On success
 *       - Others  Underlying initialization error
 */
esp_err_t app_fs_init(void);


#ifdef __cplusplus
}
#endif
