/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "claw_cap.h"
#include "claw_core.h"
#include "claw_session_mgr.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLAW_AGENT_MGR_ROOT_AGENT_ID "0"

typedef enum {
    CLAW_AGENT_MGR_STATUS_EMPTY = 0,
    CLAW_AGENT_MGR_STATUS_RUNNING,
    CLAW_AGENT_MGR_STATUS_IDLE,
    CLAW_AGENT_MGR_STATUS_COMPLETED,
    CLAW_AGENT_MGR_STATUS_CLOSED,
    CLAW_AGENT_MGR_STATUS_ERROR,
} claw_agent_mgr_status_t;

typedef struct {
    const char *agent_type;
    const char *system_prompt;
} claw_agent_mgr_subagent_type_prompt_t;

typedef struct {
    const claw_core_config_t *core_config;
    const claw_core_context_provider_t *base_context_providers;
    size_t base_context_provider_count;
    const char *root_agent_system_prompt; /* optional role overlay appended to core_config->system_prompt */
    const char *subagent_system_prompt; /* optional role overlay appended to core_config->system_prompt */
    const claw_agent_mgr_subagent_type_prompt_t *subagent_type_prompts;
    size_t subagent_type_prompt_count;
} claw_agent_mgr_config_t;

typedef struct {
    claw_session_policy_t session_policy;
    const char *user_text;
    const char *source_cap;
    const char *event_type;
    const char *source_channel;
    const char *source_chat_id;
    const char *source_sender_id;
    const char *source_message_id;
    const char *event_id;
    const char *target_channel;
    const char *target_chat_id;
    uint32_t flags;
    uint32_t request_id;
} claw_agent_mgr_root_input_t;

typedef struct {
    char agent_id[CLAW_SESSION_MGR_ID_SIZE];
    char session_id[CLAW_SESSION_MGR_ID_SIZE];
    char parent_session_id[CLAW_SESSION_MGR_ID_SIZE];
    char agent_type[32];
    claw_agent_mgr_status_t status;
    claw_core_agent_loop_phase_t phase;
    uint32_t last_request_id;
    char last_error[96];
} claw_agent_mgr_agent_info_t;

esp_err_t claw_agent_mgr_init(const claw_agent_mgr_config_t *config);
esp_err_t claw_agent_mgr_create_root_agent(const char **out_agent_id);
claw_core_handle_t claw_agent_mgr_get_root_core(void);
esp_err_t claw_agent_mgr_submit_root(const claw_agent_mgr_root_input_t *input,
                                     uint32_t timeout_ms);
esp_err_t claw_agent_mgr_submit_root_text(const char *text,
                                          const char *session_id,
                                          uint32_t flags,
                                          uint32_t timeout_ms,
                                          uint32_t *out_request_id);
esp_err_t claw_agent_mgr_receive_root_for(uint32_t request_id,
                                          claw_core_response_t *response,
                                          uint32_t timeout_ms);

esp_err_t claw_agent_mgr_spawn_subagent(const claw_cap_call_context_t *parent_ctx,
                                        const char *prompt,
                                        const char *agent_type,
                                        bool background,
                                        char *out_agent_id,
                                        size_t out_agent_id_size);
esp_err_t claw_agent_mgr_send_subagent_input(const claw_cap_call_context_t *ctx,
                                             const char *agent_id,
                                             const char *input,
                                             bool interrupt);
esp_err_t claw_agent_mgr_inspect_agent(const claw_cap_call_context_t *ctx,
                                       const char *agent_id,
                                       claw_agent_mgr_agent_info_t *out_info);
esp_err_t claw_agent_mgr_list_agents(const claw_cap_call_context_t *ctx,
                                     claw_agent_mgr_agent_info_t *out_infos,
                                     size_t max_infos,
                                     size_t *out_count);
esp_err_t claw_agent_mgr_close_agent(const claw_cap_call_context_t *ctx,
                                     const char *agent_id);
esp_err_t claw_agent_mgr_delete_agent(const claw_cap_call_context_t *ctx,
                                      const char *agent_id);
const char *claw_agent_mgr_status_to_string(claw_agent_mgr_status_t status);

#ifdef __cplusplus
}
#endif
