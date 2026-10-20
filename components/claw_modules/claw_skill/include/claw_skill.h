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

#define CLAW_SKILL_ID_MAX_LEN 63

/**
 * @brief  Configuration for claw_skill_init()
 */
typedef struct {
    const char *session_state_root_dir;  /**< Directory holding the per-session active-skill state files */
    size_t      max_file_bytes;          /**< Maximum size of a single skill document that may be read */
} claw_skill_config_t;

/**
 * @brief  How a skill may be managed at runtime, determined by its storage root
 */
typedef enum {
    CLAW_SKILL_MANAGE_MODE_READONLY = 0,  /**< Skill is fixed and cannot be modified at runtime */
    CLAW_SKILL_MANAGE_MODE_RUNTIME,       /**< Skill may be published or removed at runtime */
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
    const char               *skill_dir;        /**< Absolute directory path that owns the skill payload */
} claw_skill_catalog_entry_t;

typedef esp_err_t (*claw_skill_catalog_cb_t)(const claw_skill_catalog_entry_t *entry, void *user_ctx);

/** Callback published after a new registry snapshot becomes visible. */
typedef void (*claw_skill_registry_changed_cb_t)(void *user_ctx);

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
 *         - ESP_ERR_INVALID_SIZE if the session directory path is too long
 *         - ESP_ERR_INVALID_STATE if already initialized with another config
 *         - ESP_ERR_NO_MEM if allocation fails
 *         - other errors while creating the session-state directory
 */
esp_err_t claw_skill_init(const claw_skill_config_t *config);

/**
 * @brief  Register a skills directory
 *
 *         Call once per directory. Directories added earlier take priority:
 *         a skill id found in an earlier directory shadows the same id in a
 *         later one. Registering the same directory more than once is
 *         idempotent. Call claw_skill_reload_registry() after registering all
 *         directories.
 *
 * @param[in]  dir  Absolute path of the directory to scan for skills
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if called before claw_skill_init()
 *         - ESP_ERR_INVALID_ARG if dir is NULL or empty
 *         - ESP_ERR_NO_MEM if allocation fails
 *         - other errors while registering the directory
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
 * @brief Register a listener for successful registry snapshot changes
 *
 * The callback runs after the registry lock is released. Registering the same
 * callback/context pair more than once is idempotent.
 */
esp_err_t claw_skill_register_registry_changed_cb(
    claw_skill_registry_changed_cb_t callback,
    void *user_ctx);

/**
 * @brief  Iterate over every catalog entry in registry order
 *
 * @param[in]  cb        Callback invoked once per skill
 * @param[in]  user_ctx  Caller context passed to cb
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_STATE if not initialized
 *         - ESP_ERR_INVALID_ARG if cb is NULL
 *         - any error returned by cb
 *
 * @note  The callback receives a read-only view valid only for the callback.
 * @note  Iteration is serialized with registry reload. The callback must not
 *        call another API that takes the skill registry lock.
 */
esp_err_t claw_skill_foreach_catalog_entry(claw_skill_catalog_cb_t cb, void *user_ctx);

/** Reload and verify a skill under the primary writable root. */
esp_err_t claw_skill_publish(const char *skill_id);

/** Recursively remove a runtime skill directory and reload the registry. */
esp_err_t claw_skill_remove(const char *skill_id);

/** Return whether a skill id contains only supported characters and fits the runtime limit. */
bool claw_skill_id_is_valid(const char *skill_id);

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
 *         Reads the expanded skill document, then updates the persistent
 *         active-skill state. The registry itself is left unchanged.
 *
 * @param[in]   session_id    Session to update
 * @param[in]   skill_id      Skill to activate
 * @param[out]  document      Destination for the expanded SKILL.md
 * @param[in]   document_size Size of document in bytes
 *
 * @return
 *         - ESP_OK on success
 *         - ESP_ERR_INVALID_ARG if not initialized or arguments are invalid
 *         - ESP_ERR_NOT_FOUND if no skill has the given id
 *         - ESP_ERR_INVALID_SIZE if the document does not fit
 *         - other errors while persisting the state
 */
esp_err_t claw_skill_activate_for_session(const char *session_id, const char *skill_id, char *document, size_t document_size);

esp_err_t claw_skill_delete_session_state(const char *session_id,  bool *out_deleted_any);

#ifdef __cplusplus
}
#endif
