/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "claw_session_mgr.h"
typedef esp_err_t (*claw_session_visit_fn)(const claw_session_mgr_alias_map_t *map, void *ctx);
esp_err_t claw_session_mgr_visit(claw_session_visit_fn visit, void *ctx);
esp_err_t claw_session_catalog_init(const char *root);
esp_err_t claw_session_file_replace(const char *temporary, const char *path);
