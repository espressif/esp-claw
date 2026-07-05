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
 * @brief  Register the file-access capability group with the capability registry
 *
 *         Sandbox roots are read live from claw_paths: CLAW_PATH_DATA is the
 *         writable workspace and CLAW_PATH_SYSTEM is the read-only firmware
 *         root. At least one of them must be set in claw_paths before this call.
 *         All tool paths must be absolute and fall under one of those roots.
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if no sandbox root is configured in claw_paths
 *         - other errors from the capability registry
 */
esp_err_t cap_files_register_group(void);

#ifdef __cplusplus
}
#endif
