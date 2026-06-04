/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLAW_SESSION_MGR_PATH_SIZE    256
#define CLAW_SESSION_MGR_KEY_SIZE     160
#define CLAW_SESSION_MGR_ID_SIZE      256
#define CLAW_SESSION_MGR_ALIAS_MAX    32
#define CLAW_SESSION_MGR_MAX_SESSIONS 32

typedef enum {
    CLAW_SESSION_POLICY_CHAT = 0,
    CLAW_SESSION_POLICY_TRIGGER = 1,
    CLAW_SESSION_POLICY_GLOBAL = 2,
    CLAW_SESSION_POLICY_EPHEMERAL = 3,
    CLAW_SESSION_POLICY_NOSAVE = 4,
} claw_session_policy_t;

typedef struct {
    uint32_t agent_id;
    claw_session_policy_t session_policy;
    const char *source_cap;
    const char *event_type;
    const char *source_channel;
    const char *chat_id;
    const char *message_id;
    const char *event_id;
} claw_session_build_context_t;

typedef struct {
    char chat_key[CLAW_SESSION_MGR_KEY_SIZE];
    char current_alias[CLAW_SESSION_MGR_ALIAS_MAX + 1];
    size_t session_count;
    char sessions[CLAW_SESSION_MGR_MAX_SESSIONS][CLAW_SESSION_MGR_ALIAS_MAX + 1];
} claw_session_mgr_alias_map_t;

typedef esp_err_t (*claw_session_mgr_delete_session_fn_t)(const char *session_id,
                                                          bool *out_deleted_any,
                                                          void *user_ctx);

bool claw_session_mgr_alias_is_valid(const char *alias);
bool claw_session_mgr_is_configured(void);
esp_err_t claw_session_mgr_set_session_root_dir(const char *session_root_dir);
esp_err_t claw_session_mgr_set_delete_session_handler(claw_session_mgr_delete_session_fn_t fn,
                                                      void *user_ctx);
esp_err_t claw_session_mgr_build_session_id(const claw_session_build_context_t *ctx,
                                            char *buf,
                                            size_t buf_size,
                                            size_t *out_len);
esp_err_t claw_session_mgr_alloc_subagent_session_id(const char *parent_session_id,
                                                     char *buf,
                                                     size_t buf_size,
                                                     size_t *out_len);
esp_err_t claw_session_mgr_subagent_id_is_known(const char *parent_session_id,
                                                const char *subagent_id,
                                                bool *out_known);
esp_err_t claw_session_mgr_list_subagent_sessions(const char *parent_session_id,
                                                  char (*out_ids)[CLAW_SESSION_MGR_ID_SIZE],
                                                  size_t max_ids,
                                                  size_t *out_count);
esp_err_t claw_session_mgr_delete_subagent_session(const char *parent_session_id,
                                                   const char *subagent_id,
                                                   bool *out_deleted_any);
esp_err_t claw_session_mgr_new_chat_session(uint32_t agent_id,
                                            const char *source_channel,
                                            const char *chat_id,
                                            const char *requested_alias,
                                            bool has_requested_alias,
                                            char *out_alias,
                                            size_t out_alias_size);
esp_err_t claw_session_mgr_list_chat_sessions(uint32_t agent_id,
                                             const char *source_channel,
                                             const char *chat_id,
                                             claw_session_mgr_alias_map_t *out_map);
esp_err_t claw_session_mgr_switch_chat_session(uint32_t agent_id,
                                               const char *source_channel,
                                               const char *chat_id,
                                               const char *alias,
                                               char *out_alias,
                                               size_t out_alias_size);
esp_err_t claw_session_mgr_delete_chat_session(uint32_t agent_id,
                                               const char *source_channel,
                                               const char *chat_id,
                                               const char *alias,
                                               char *out_alias,
                                               size_t out_alias_size);

#ifdef __cplusplus
}
#endif
