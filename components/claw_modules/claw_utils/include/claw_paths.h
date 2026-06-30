/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Logical storage roots.
 *
 * Each root is resolved once at boot to a physical mount point, so reusable
 * modules can compose paths without knowing whether writable storage is backed
 * by internal flash or an SD card.
 */
typedef enum {
    CLAW_PATH_DATA = 0,
    CLAW_PATH_SYSTEM,
    CLAW_PATH_ROOT_MAX,
} claw_path_root_t;

/**
 * @brief Set the physical path of a logical root.
 *
 * Intended to be called during early boot, before readers run.
 */
esp_err_t claw_paths_set(claw_path_root_t root, const char *path);

/**
 * @brief Get the physical path of a logical root.
 *
 * Returns NULL if the root is invalid or has not been set.
 */
const char *claw_paths_get(claw_path_root_t root);

/**
 * @brief Compose a path under a logical root.
 *
 * Writes "<root>/<subpath>" into out, or just "<root>" when subpath is NULL or
 * empty.
 */
esp_err_t claw_paths_join(claw_path_root_t root, const char *subpath, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
