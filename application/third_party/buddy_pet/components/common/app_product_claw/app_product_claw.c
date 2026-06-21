/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_product_claw.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_claw.h"
#include "app_storage_paths.h"
#include "claw_paths.h"
#include "esp_check.h"
#include "esp_log.h"

#include "cap_match_watch.h"
#include "cap_pet_buddy.h"
#if CONFIG_APP_CLAW_CAP_LUA
#include "lua_module_match_watch.h"
#include "lua_module_pet.h"
#endif

static const char *TAG = "app_product_claw";

static char *app_product_claw_trim_token(char *token)
{
    char *end;

    if (token == NULL) {
        return NULL;
    }

    while (*token && isspace((unsigned char)*token)) {
        token++;
    }

    end = token + strlen(token);
    while (end > token && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return token;
}

static bool app_product_claw_config_list_empty(const char *value)
{
    if (value == NULL) {
        return true;
    }
    for (size_t i = 0; value[i]; i++) {
        if (!isspace((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool app_product_claw_config_list_contains(const char *value, const char *target)
{
    char copy[APP_CLAW_STR_LEN];
    char *saveptr = NULL;
    char *token = NULL;

    if (target == NULL || target[0] == '\0') {
        return false;
    }
    if (app_product_claw_config_list_empty(value)) {
        return true;
    }

    strlcpy(copy, value, sizeof(copy));
    for (token = strtok_r(copy, ",", &saveptr);
            token != NULL;
            token = strtok_r(NULL, ",", &saveptr)) {
        char *trimmed = app_product_claw_trim_token(token);

        if (trimmed == NULL || trimmed[0] == '\0' ||
                strcmp(trimmed, "none") == 0 ||
                strcmp(trimmed, "__none__") == 0) {
            continue;
        }
        if (strcmp(trimmed, target) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t app_product_claw_register_extra_lua_modules(const app_claw_config_t *config,
                                                            const app_claw_storage_paths_t *paths)
{
#if CONFIG_APP_CLAW_CAP_LUA
    ESP_RETURN_ON_FALSE(paths != NULL, ESP_ERR_INVALID_ARG, TAG, "missing app paths");
#else
    (void)paths;
#endif
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "missing app config");

#if CONFIG_APP_CLAW_CAP_LUA
    if (app_product_claw_config_list_contains(config->enabled_lua_modules, "match_watch")) {
        ESP_RETURN_ON_ERROR(lua_module_match_watch_register(), TAG, "register match watch Lua module failed");
    }
    if (app_product_claw_config_list_contains(config->enabled_lua_modules, "pet")) {
        ESP_RETURN_ON_ERROR(lua_module_pet_register(paths->fatfs_base_path),
                            TAG, "register pet Lua module failed");
    }
#endif
    return ESP_OK;
}

static esp_err_t app_product_claw_register_extra_capabilities(const app_claw_config_t *config,
                                                             const app_claw_storage_paths_t *paths)
{
    (void)config;
    (void)paths;

    ESP_RETURN_ON_ERROR(cap_match_watch_register_group(), TAG, "register match watch cap failed");
    ESP_RETURN_ON_ERROR(cap_pet_buddy_register_group(), TAG, "register pet buddy cap failed");
    return ESP_OK;
}

static esp_err_t app_product_claw_init_storage_paths(app_claw_storage_paths_t *paths, const char *data_root)
{
    if (paths == NULL || data_root == NULL || data_root[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }

    memset(paths, 0, sizeof(*paths));
    if (strlcpy(paths->fatfs_base_path, data_root, sizeof(paths->fatfs_base_path)) >= sizeof(paths->fatfs_base_path) ||
        snprintf(paths->memory_session_root, sizeof(paths->memory_session_root), "%s/sessions", data_root) >= sizeof(paths->memory_session_root) ||
        snprintf(paths->memory_root_dir, sizeof(paths->memory_root_dir), "%s/memory", data_root) >= sizeof(paths->memory_root_dir) ||
        snprintf(paths->skills_root_dir, sizeof(paths->skills_root_dir), "%s/skills", data_root) >= sizeof(paths->skills_root_dir) ||
        snprintf(paths->system_skills_root_dir, sizeof(paths->system_skills_root_dir), "%s/skills", data_root) >= sizeof(paths->system_skills_root_dir) ||
        snprintf(paths->lua_root_dir, sizeof(paths->lua_root_dir), "%s/scripts", data_root) >= sizeof(paths->lua_root_dir) ||
        snprintf(paths->router_rules_path, sizeof(paths->router_rules_path), "%s/router_rules/router_rules.json", data_root) >= sizeof(paths->router_rules_path) ||
        snprintf(paths->scheduler_rules_path, sizeof(paths->scheduler_rules_path), "%s/scheduler/schedules.json", data_root) >= sizeof(paths->scheduler_rules_path) ||
        snprintf(paths->im_attachment_root, sizeof(paths->im_attachment_root), "%s/inbox", data_root) >= sizeof(paths->im_attachment_root)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void app_product_claw_fill_claw_config(app_claw_config_t *claw_config,
                                              const app_config_t *config,
                                              bool suppress_startup_event)
{
    app_config_to_claw(config, claw_config);
    claw_config->suppress_startup_event = suppress_startup_event;
    claw_config->extra_lua_modules_cb = app_product_claw_register_extra_lua_modules;
    claw_config->extra_capabilities_cb = app_product_claw_register_extra_capabilities;
}

esp_err_t app_product_claw_start(const app_config_t *config,
                                 const char *data_root,
                                 bool suppress_startup_event)
{
    /* app_claw_config_t (~4.8 KB) and app_claw_storage_paths_t (~640 B) are far too
     * large for the main task stack; allocate them on the heap. */
    app_claw_config_t *claw_config;
    app_claw_storage_paths_t *paths;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "missing app config");

    claw_config = calloc(1, sizeof(*claw_config));
    paths = calloc(1, sizeof(*paths));
    if (claw_config == NULL || paths == NULL) {
        free(claw_config);
        free(paths);
        ESP_LOGE(TAG, "no mem for claw config/paths");
        return ESP_ERR_NO_MEM;
    }

    err = app_product_claw_init_storage_paths(paths, data_root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init storage paths failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = claw_paths_set(CLAW_PATH_DATA, data_root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set DATA path failed: %s", esp_err_to_name(err));
        goto cleanup;
    }
    err = claw_paths_set(CLAW_PATH_SYSTEM, data_root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set SYSTEM path failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    app_product_claw_fill_claw_config(claw_config, config, suppress_startup_event);
    err = app_claw_start_with_paths(claw_config, paths);

cleanup:
    free(claw_config);
    free(paths);
    return err;
}

esp_err_t app_product_claw_update_config(const app_config_t *config,
                                         bool suppress_startup_event)
{
    /* app_claw_config_t (~4.8 KB) is too large for the caller's task stack; use the heap. */
    app_claw_config_t *claw_config;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "missing app config");

    claw_config = calloc(1, sizeof(*claw_config));
    if (claw_config == NULL) {
        ESP_LOGE(TAG, "no mem for claw config");
        return ESP_ERR_NO_MEM;
    }
    app_product_claw_fill_claw_config(claw_config, config, suppress_startup_event);
    err = app_claw_update_config(claw_config);
    free(claw_config);
    return err;
}
