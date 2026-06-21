/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app_claw.h"
#include "app_storage_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *group_id;
    const char *display_name;
    bool llm_visible_by_default;
} app_capability_group_info_t;

typedef esp_err_t (*app_capability_prepare_fn)(const app_claw_config_t *config,
                                               const app_claw_storage_paths_t *paths);
typedef esp_err_t (*app_capability_register_fn)(const app_claw_config_t *config,
                                                const app_claw_storage_paths_t *paths);

typedef struct {
    const char *group_id;
    const char *display_name;
    bool llm_visible_by_default;
    app_capability_prepare_fn prepare;
    app_capability_register_fn reg;
} app_capability_external_group_t;

/**
 * @brief Register an application-provided capability group for app_claw startup.
 *
 *        Call this before app_claw_start(). The group participates in
 *        enabled_cap_groups and llm_visible_cap_groups using group_id, alongside
 *        built-in app_claw groups.
 */
esp_err_t app_capabilities_register_external_group(const app_capability_external_group_t *group);
esp_err_t app_capabilities_init(const app_claw_config_t *config,
                                const app_claw_storage_paths_t *paths);
esp_err_t app_capabilities_get_compiled_groups(const app_capability_group_info_t **groups,
                                               size_t *count);

#ifdef __cplusplus
}
#endif
