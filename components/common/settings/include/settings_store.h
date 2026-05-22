/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *namespace_name;
} settings_store_config_t;

esp_err_t settings_store_init(const settings_store_config_t *config);
esp_err_t settings_store_get_string(const char *key,
                                    char *buf,
                                    size_t buf_size,
                                    const char *default_value);
esp_err_t settings_store_has_key(const char *key, bool *exists);
esp_err_t settings_store_set_string(const char *key, const char *value);
esp_err_t settings_store_erase_key(const char *key);
esp_err_t settings_store_commit(void);

/**
 * @brief Optional bus locks invoked around backup file I/O.
 *
 * Supplied so callers can serialize SD-card writes against other consumers
 * sharing the same SPI host (LCD, etc.). Both may be NULL to disable
 * locking. The lock callback returns true if the lock was acquired.
 */
typedef bool (*settings_store_bus_lock_fn)(void);
typedef void (*settings_store_bus_unlock_fn)(bool was_locked);

/**
 * @brief Configure an on-disk backup of the current NVS namespace.
 *
 * After this is called every successful `settings_store_set_string()` will
 * also write the entire namespace to @p file_path as a JSON object. Pass
 * NULL or empty @p file_path to disable backups.
 *
 * The directory portion of @p file_path is created if missing.
 */
esp_err_t settings_store_set_backup_path(const char *file_path,
                                         settings_store_bus_lock_fn lock,
                                         settings_store_bus_unlock_fn unlock);

/** Force a dump of the current namespace to the configured backup path. */
esp_err_t settings_store_dump_to_backup(void);

/**
 * @brief Restore every key in the configured backup file into NVS.
 *
 * Existing NVS entries for the same keys are overwritten. Returns
 * ESP_ERR_NOT_FOUND if no backup file exists yet, ESP_ERR_INVALID_STATE if
 * no backup path is configured.
 */
esp_err_t settings_store_restore_from_backup(void);

/**
 * @brief Returns true if the configured NVS namespace currently has no
 *        entries (i.e. fresh after firmware re-flash + erase).
 */
esp_err_t settings_store_is_namespace_empty(bool *out_empty);

#ifdef __cplusplus
}
#endif
