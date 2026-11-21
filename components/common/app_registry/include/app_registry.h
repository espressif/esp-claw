/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_REGISTRY_ID_MAX_LEN 63

typedef enum {
    APP_REGISTRY_MANAGE_MODE_READONLY = 0,
    APP_REGISTRY_MANAGE_MODE_RUNTIME,
} app_registry_manage_mode_t;

typedef struct {
    const char *app_id;
    const char *app_dir;
    const char *display_name;
    const char *entry;
    const char *icon;
    const char *args_json;
    int order;
    bool visible;
    app_registry_manage_mode_t manage_mode;
} app_registry_entry_t;

typedef esp_err_t (*app_registry_entry_cb_t)(const app_registry_entry_t *entry, void *user_ctx);
typedef void (*app_registry_changed_cb_t)(void *user_ctx);

esp_err_t app_registry_init(void);
/* Add writable DATA root first, then read-only SYSTEM roots. */
esp_err_t app_registry_add_directory(const char *dir);
esp_err_t app_registry_reload(void);
/* Registry change callbacks run after the registry lock is released. */
esp_err_t app_registry_register_changed_cb(app_registry_changed_cb_t callback, void *user_ctx);
esp_err_t app_registry_foreach(app_registry_entry_cb_t callback, void *user_ctx);
esp_err_t app_registry_publish(const char *app_id);
esp_err_t app_registry_remove(const char *app_id);
bool app_registry_id_is_valid(const char *app_id);

#ifdef __cplusplus
}
#endif
