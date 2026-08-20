/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Logical storage roots
 *
 *         Each root is resolved once at boot to a physical mount point, so the
 *         rest of the firmware can compose paths without knowing whether the
 *         backing medium is internal flash or an SD card.
 */
typedef enum {
    CLAW_PATH_DATA = 0,     /**< Writable data root (flash storage partition or SD card mount point) */
    CLAW_PATH_SYSTEM,       /**< Read-only firmware-baked root (e.g. /system) */
    CLAW_PATH_ROOT_MAX,     /**< Number of logical roots */
} claw_path_root_t;

/**
 * @brief  Filesystem-independent space query callback
 *
 *         Applications provide this because the physical backend (LittleFS,
 *         FAT on SD, etc.) is selected before the agent framework starts.
 */
typedef esp_err_t (*claw_path_space_provider_t)(void *ctx,
                                                uint64_t *total_bytes,
                                                uint64_t *free_bytes);

/**
 * @brief  Set the physical path of a logical root
 *
 *         Intended to be called once during early boot, before any reader runs.
 *
 * @param[in]  root  Logical root to set
 * @param[in]  path  Absolute mount-point path (copied internally)
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if root is out of range or path is empty
 *         - ESP_ERR_INVALID_SIZE if path is too long to store
 */
esp_err_t claw_paths_set(claw_path_root_t root, const char *path);

/**
 * @brief  Get the physical path of a logical root
 *
 * @param[in]  root  Logical root to query
 *
 * @return
 *         - Pointer to the stored path on success
 *         - NULL if root is out of range or has not been set
 */
const char *claw_paths_get(claw_path_root_t root);

/**
 * @brief  Register the space query provider for a logical root
 *
 *         Intended to be called during early boot after the backing filesystem
 *         has been mounted. Passing NULL clears the provider.
 */
esp_err_t claw_paths_set_space_provider(claw_path_root_t root,
                                        claw_path_space_provider_t provider,
                                        void *ctx);

/**
 * @brief  Query total and free space without exposing the filesystem type
 */
esp_err_t claw_paths_get_space(claw_path_root_t root,
                               uint64_t *total_bytes,
                               uint64_t *free_bytes);

/**
 * @brief  Compose a path under a logical root
 *
 *         Writes "<root>/<subpath>" into out, or just "<root>" when subpath is
 *         NULL or empty.
 *
 * @param[in]   root      Logical root to compose under
 * @param[in]   subpath   Path relative to the root (may be NULL or empty)
 * @param[out]  out       Destination buffer
 * @param[in]   out_size  Size of out in bytes
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if the root has not been set
 *         - ESP_ERR_INVALID_ARG if out is NULL or out_size is 0
 *         - ESP_ERR_INVALID_SIZE if the composed path does not fit in out
 */
esp_err_t claw_paths_join(claw_path_root_t root, const char *subpath, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
