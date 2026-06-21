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

esp_err_t app_capabilities_init(const app_claw_config_t *config,
                                const app_claw_storage_paths_t *paths);
esp_err_t app_capabilities_get_compiled_groups(const app_capability_group_info_t **groups,
                                               size_t *count);

#ifdef __cplusplus
}
#endif
