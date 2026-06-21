/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @file pet_registry.h
 * @brief Pet package discovery and selected-pet persistence API.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fixed-size field lengths used by registry snapshots.
 *
 * All string fields returned by this API are NUL-terminated.
 */
#define PET_REGISTRY_ID_LEN         48
#define PET_REGISTRY_NAME_LEN       48
#define PET_REGISTRY_TITLE_LEN      48
#define PET_REGISTRY_PROFILE_LEN    48
#define PET_REGISTRY_BASE_PATH_LEN  128
#define PET_REGISTRY_ASSET_PATH_LEN 192
#define PET_REGISTRY_MAX            16

/**
 * @brief Pet package metadata discovered from embedded or sidecar metadata.
 */
typedef struct {
    /** Stable pet id. This is the value persisted as selected pet. */
    char id[PET_REGISTRY_ID_LEN];
    /** Short display name, falling back to id when omitted. */
    char name[PET_REGISTRY_NAME_LEN];
    /** Optional human-facing title/role metadata. */
    char title[PET_REGISTRY_TITLE_LEN];
    /** Optional host profile/binding metadata. */
    char profile[PET_REGISTRY_PROFILE_LEN];
    /** Directory containing pet package assets. */
    char base_path[PET_REGISTRY_BASE_PATH_LEN];
    /** Absolute path to the mmap asset .bin file. */
    char asset_path[PET_REGISTRY_ASSET_PATH_LEN];
} pet_registry_entry_t;

/**
 * @brief Override the mounted FATFS root used for scans.
 *
 * Default is "/fatfs". Calling this clears the in-memory entry list; call
 * pet_registry_refresh() to scan the new base path.
 *
 * @param base_path Absolute FATFS mount path.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_registry_set_fatfs_base_path(const char *base_path);

/**
 * @brief Rescan pet packages under /<base>/skills/pet_module.
 *
 * A valid pet package is a mmap asset .bin file with embedded metadata or a
 * sibling .pet.json sidecar.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_registry_refresh(void);

/**
 * @brief Get the number of entries found by the latest refresh.
 *
 * @return Entry count.
 */
size_t pet_registry_count(void);

/**
 * @brief Copy one registry entry by index.
 *
 * @param index Entry index from the latest refresh result.
 * @param[out] out Destination entry.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND for an invalid index, or an ESP error code.
 */
esp_err_t pet_registry_get(size_t index, pet_registry_entry_t *out);

/**
 * @brief Copy the persisted selected pet id.
 *
 * @param[out] out Destination string buffer.
 * @param out_size Destination buffer size.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when unset, or an ESP error code.
 */
esp_err_t pet_registry_get_selected(char *out, size_t out_size);

/**
 * @brief Copy the selected pet entry.
 *
 * Refreshes once if the registry is empty.
 *
 * @param[out] out Destination entry.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when unavailable, or an ESP error code.
 */
esp_err_t pet_registry_get_selected_entry(pet_registry_entry_t *out);

/**
 * @brief Copy the selected pet mmap asset path.
 *
 * @param[out] out Destination string buffer.
 * @param out_size Destination buffer size.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when unavailable, or an ESP error code.
 */
esp_err_t pet_registry_get_selected_asset_path(char *out, size_t out_size);

/**
 * @brief Copy the first discovered pet mmap asset path.
 *
 * Refreshes once if the registry is empty.
 *
 * @param[out] out Destination string buffer.
 * @param out_size Destination buffer size.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when no pet package is available,
 *         or an ESP error code.
 */
esp_err_t pet_registry_get_default_asset_path(char *out, size_t out_size);

/**
 * @brief Check whether a selected pet id is persisted.
 *
 * This may return true even if the selected package is not currently present.
 *
 * @return true when a selected id exists, false otherwise.
 */
bool pet_registry_has_selected(void);

#ifdef __cplusplus
}
#endif
