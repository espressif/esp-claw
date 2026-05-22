/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "claw_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *skills_root_dir;
    const char *session_state_root_dir;
    size_t max_skill_files;
    size_t max_file_bytes;
} claw_skill_config_t;

typedef enum {
    CLAW_SKILL_MANAGE_MODE_READONLY = 0,
    CLAW_SKILL_MANAGE_MODE_RUNTIME,
} claw_skill_manage_mode_t;

typedef struct {
    const char *id;
    const char *file;
    const char *summary;
    const char *const *cap_groups;
    size_t cap_group_count;
    claw_skill_manage_mode_t manage_mode;
} claw_skill_catalog_entry_t;

esp_err_t claw_skill_init(const claw_skill_config_t *config);

esp_err_t claw_skill_reload_registry(void);
const char *claw_skill_get_skills_root_dir(void);

/* Renders the skill catalog used by the prompt layer. */
esp_err_t claw_skill_read_skills_list(char *buf, size_t size);
esp_err_t claw_skill_render_catalog_json(char *buf, size_t size);
esp_err_t claw_skill_get_catalog_entry(const char *skill_id, claw_skill_catalog_entry_t *out_entry);
esp_err_t claw_skill_read_document(const char *skill_id, char *buf, size_t size);

/* Loads the active skill ids for one session from persistent state. */
esp_err_t claw_skill_load_active_skill_ids(const char *session_id,
                                           char ***out_skill_ids,
                                           size_t *out_skill_count);

/* Loads the active capability groups implied by active skills for one session. */
esp_err_t claw_skill_load_active_cap_groups(const char *session_id,
                                            char ***out_group_ids,
                                            size_t *out_group_count);

/* Changes only the active skill state for one session. */
esp_err_t claw_skill_activate_for_session(const char *session_id, const char *skill_id);

/* Prompt provider for the stable skill catalog. */
extern const claw_core_context_provider_t claw_skill_skills_list_provider;

#ifdef __cplusplus
}
#endif
