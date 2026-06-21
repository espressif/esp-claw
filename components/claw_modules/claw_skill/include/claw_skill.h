/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "claw_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Configuration for claw_skill_init()
 */
typedef struct {
    const char *session_state_root_dir;  /**< Directory holding the per-session active-skill state files */
    size_t      max_file_bytes;          /**< Maximum size of a single skill document that may be read */
} claw_skill_config_t;

/**
 * @brief  How a skill may be managed at runtime, declared in its SKILL.md metadata
 */
typedef enum {
    CLAW_SKILL_MANAGE_MODE_READONLY = 0,  /**< Skill is fixed and cannot be modified at runtime */
    CLAW_SKILL_MANAGE_MODE_RUNTIME,       /**< Skill may be registered/unregistered at runtime */
} claw_skill_manage_mode_t;

/**
 * @brief  Read-only view of a single skill in the registry catalog
 */
typedef struct {
    const char               *id;               /**< Unique skill id (the "name" field of SKILL.md) */
    const char               *file;             /**< Document path relative to its root, "<id>/SKILL.md" */
    const char               *summary;          /**< Short description shown in the skills catalog */
    const char *const        *cap_groups;       /**< Capability groups unlocked while the skill is active */
    size_t                    cap_group_count;  /**< Number of entries in cap_groups */
    claw_skill_manage_mode_t  manage_mode;      /**< Management mode of the skill */
} claw_skill_catalog_entry_t;

/**
 * @brief  Initialize the skill registry
 *
 *         The registry starts empty; skills directories are registered
 *         afterwards with claw_skill_add_directory().
 *
 * @param[in]  config  Session-state directory and per-document size limit
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if config or its session directory is missing
 *         - ESP_ERR_NO_MEM if allocation fails
 *         - other errors while creating the session-state directory
 */
esp_err_t claw_skill_init(const claw_skill_config_t *config);

/**
 * @brief  Register a skills directory and reload the registry
 *
 *         Call once per directory. Directories added earlier take priority:
 *         a skill id found in an earlier directory shadows the same id in a
 *         later one. Registering the same directory more than once is
 *         idempotent and does not trigger a reload.
 *
 * @param[in]  dir  Absolute path of the directory to scan for skills
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if called before claw_skill_init()
 *         - ESP_ERR_INVALID_ARG if dir is NULL or empty
 *         - ESP_ERR_NO_MEM if allocation fails
 *         - other errors while reloading the registry
 */
esp_err_t claw_skill_add_directory(const char *dir);

/**
 * @brief  Rescan every registered directory and rebuild the registry
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if called before claw_skill_init()
 *         - other errors while scanning the directories
 */
esp_err_t claw_skill_reload_registry(void);

/**
 * @brief  Render the skill catalog as a plain-text list for the prompt layer
 *
 * @param[out]  buf   Destination buffer
 * @param[in]   size  Size of buf in bytes
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if not initialized or arguments are invalid
 */
esp_err_t claw_skill_read_skills_list(char *buf, size_t size);

/**
 * @brief  Render the skill catalog as a JSON document
 *
 * @param[out]  buf   Destination buffer
 * @param[in]   size  Size of buf in bytes
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if not initialized or arguments are invalid
 *         - ESP_ERR_NO_MEM if allocation fails
 *         - ESP_ERR_INVALID_SIZE if the rendered JSON does not fit in buf
 */
esp_err_t claw_skill_render_catalog_json(char *buf, size_t size);

/**
 * @brief  Look up a single catalog entry by skill id
 *
 * @param[in]   skill_id   Id of the skill to look up
 * @param[out]  out_entry  Receives a read-only view of the skill
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if out_entry is NULL
 *         - ESP_ERR_NOT_FOUND if no skill has the given id
 */
esp_err_t claw_skill_get_catalog_entry(const char *skill_id, claw_skill_catalog_entry_t *out_entry);

/**
 * @brief  Read a skill's SKILL.md, expanding {CUR_SKILL_DIR} placeholders
 *
 * @param[in]   skill_id  Id of the skill whose document to read
 * @param[out]  buf       Destination buffer
 * @param[in]   size      Size of buf in bytes
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if not initialized
 *         - ESP_ERR_INVALID_ARG if arguments are invalid
 *         - ESP_ERR_NOT_FOUND if no skill has the given id
 *         - ESP_ERR_INVALID_SIZE if the document does not fit in buf
 *         - other errors while reading the document
 */
esp_err_t claw_skill_read_document(const char *skill_id, char *buf, size_t size);

/**
 * @brief  Load the active skill ids for one session from persistent state
 *
 * @param[in]   session_id       Session whose active skills to load
 * @param[out]  out_skill_ids    Receives a newly allocated array of skill ids
 * @param[out]  out_skill_count  Receives the number of ids returned
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if output pointers are NULL
 *         - ESP_ERR_INVALID_STATE if not initialized or session id is invalid
 *         - ESP_ERR_NOT_FOUND if the session has no active skills
 *         - other errors while reading the state file
 *
 * @note  The caller owns *out_skill_ids and each string and must free them.
 */
esp_err_t claw_skill_load_active_skill_ids(const char *session_id,
                                           char ***out_skill_ids,
                                           size_t *out_skill_count);

/**
 * @brief  Load the capability groups implied by a session's active skills
 *
 * @param[in]   session_id       Session whose capability groups to resolve
 * @param[out]  out_group_ids    Receives a newly allocated array of group ids
 * @param[out]  out_group_count  Receives the number of group ids returned
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if output pointers are NULL
 *         - ESP_ERR_NOT_FOUND if no capability groups are active
 *         - other errors while loading the active skills
 *
 * @note  The caller owns *out_group_ids and each string and must free them.
 */
esp_err_t claw_skill_load_active_cap_groups(const char *session_id,
                                            char ***out_group_ids,
                                            size_t *out_group_count);

/**
 * @brief  Mark a skill active for one session
 *
 *         Updates only the persistent active-skill state; the registry itself
 *         is left unchanged.
 *
 * @param[in]  session_id  Session to update
 * @param[in]  skill_id    Skill to activate
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if not initialized or arguments are invalid
 *         - ESP_ERR_NOT_FOUND if no skill has the given id
 *         - other errors while persisting the state
 */
esp_err_t claw_skill_activate_for_session(const char *session_id, const char *skill_id);
esp_err_t claw_skill_delete_session_state(const char *session_id,
                                          bool *out_deleted_any);

/**
 * @brief  Prompt context provider that injects the stable skill catalog
 */
extern const claw_core_context_provider_t claw_skill_skills_list_provider;

#ifdef __cplusplus
}
#endif
