/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "claw_core.h"
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_CAP_KIND_CALLABLE = 0,
    CLAW_CAP_KIND_EVENT_SOURCE = 1,
    CLAW_CAP_KIND_HYBRID = 2,
} claw_cap_kind_t;

typedef enum {
    CLAW_CAP_CALLER_SYSTEM = 0,
    CLAW_CAP_CALLER_AGENT = 1,
    CLAW_CAP_CALLER_CONSOLE = 2,
    CLAW_CAP_CALLER_ROOT_AGENT = CLAW_CAP_CALLER_AGENT,
    CLAW_CAP_CALLER_SUB_AGENT = 3,
} claw_cap_caller_t;

typedef enum {
    CLAW_CAP_FLAG_CALLABLE_BY_LLM = 1 << 0,
    CLAW_CAP_FLAG_EMITS_EVENTS = 1 << 1,
    CLAW_CAP_FLAG_SUPPORTS_LIFECYCLE = 1 << 2,
    CLAW_CAP_FLAG_RESTRICTED = 1 << 3,
    CLAW_CAP_FLAG_ROOT_AGENT_ONLY = 1 << 4,
} claw_cap_flags_t;

typedef enum {
    CLAW_CAP_STATE_REGISTERED = 0,
    CLAW_CAP_STATE_STARTED = 1,
    CLAW_CAP_STATE_DISABLED = 2,
    CLAW_CAP_STATE_DRAINING = 3,
    CLAW_CAP_STATE_UNLOADING = 4,
} claw_cap_state_t;

typedef struct {
    uint32_t request_id;
    const char *session_id;
    const char *agent_id;
    const char *agent_type;
    const char *parent_agent_id;
    const char *parent_session_id;
    const char *channel;
    const char *chat_id;
    const char *target_channel;
    const char *target_chat_id;
    const char *source_cap;
    const char *correlation_id;
    claw_core_handle_t core;
    claw_cap_caller_t caller;
} claw_cap_call_context_t;

typedef struct {
    uint32_t magic;
    claw_core_handle_t *core;
    claw_cap_caller_t caller;
    const char *agent_id;
    const char *agent_type;
    const char *parent_agent_id;
    const char *parent_session_id;
} claw_cap_core_call_user_ctx_t;

#define CLAW_CAP_CORE_CALL_USER_CTX_MAGIC 0x43415043U

typedef enum {
    CLAW_CAP_EVENT_ROUTE_PASS = 0,
    CLAW_CAP_EVENT_ROUTE_CONSUMED = 1,
    CLAW_CAP_EVENT_ROUTE_ERROR = 2,
} claw_cap_event_route_t;

typedef esp_err_t (*claw_cap_lifecycle_fn)(void);
typedef esp_err_t (*claw_cap_execute_fn)(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size);

typedef struct {
    const char *id;
    const char *name;
    const char *family;
    const char *description;
    claw_cap_kind_t kind;
    uint32_t cap_flags;
    const char *input_schema_json;
    claw_cap_lifecycle_fn init;
    claw_cap_lifecycle_fn start;
    claw_cap_lifecycle_fn stop;
    claw_cap_execute_fn execute;
} claw_cap_descriptor_t;

typedef struct {
    const claw_cap_descriptor_t *items;
    size_t count;
} claw_cap_list_t;

typedef struct {
    const char *group_id;
    const char *plugin_name;
    const char *version;
    const claw_cap_descriptor_t *descriptors;
    size_t descriptor_count;
    void *plugin_ctx;
    claw_cap_lifecycle_fn group_init;
    claw_cap_lifecycle_fn group_start;
    claw_cap_lifecycle_fn group_stop;
} claw_cap_group_t;

typedef struct {
    const char *group_id;
    const char *plugin_name;
    const char *version;
    claw_cap_state_t state;
    size_t descriptor_count;
} claw_cap_group_info_t;

typedef struct {
    const claw_cap_group_info_t *items;
    size_t count;
} claw_cap_group_list_t;

typedef struct {
    const char *id;
    const char *name;
    const char *group_id;
    claw_cap_state_t state;
    uint32_t active_calls;
} claw_cap_descriptor_info_t;

esp_err_t claw_cap_init(void);
esp_err_t claw_cap_register(const claw_cap_descriptor_t *descriptor);
esp_err_t claw_cap_register_group(const claw_cap_group_t *group);
esp_err_t claw_cap_start_all(void);
esp_err_t claw_cap_stop_all(void);
esp_err_t claw_cap_enable_group(const char *group_id);
esp_err_t claw_cap_disable_group(const char *group_id);
esp_err_t claw_cap_unregister_group(const char *group_id, uint32_t timeout_ms);
esp_err_t claw_cap_unregister(const char *id_or_name, uint32_t timeout_ms);
esp_err_t claw_cap_set_llm_visible_groups(const char *const *group_ids, size_t count);
esp_err_t claw_cap_set_session_llm_visible_groups(const char *session_id,
                                                  const char *const *group_ids,
                                                  size_t count);
bool claw_cap_group_exists(const char *group_id);
esp_err_t claw_cap_get_group_state(const char *group_id, claw_cap_state_t *state);
esp_err_t claw_cap_get_descriptor_state(const char *id_or_name,
                                        claw_cap_descriptor_info_t *info);
const claw_cap_descriptor_t *claw_cap_find(const char *id_or_name);
claw_cap_list_t claw_cap_list(void);
claw_cap_group_list_t claw_cap_list_groups(void);
esp_err_t claw_cap_call(const char *id_or_name,
                        const char *input_json,
                        const claw_cap_call_context_t *ctx,
                        char *output,
                        size_t output_size);
esp_err_t claw_cap_call_from_core(const char *cap_name,
                                  const char *input_json,
                                  const claw_core_request_t *request,
                                  char **out_output,
                                  void *user_ctx);
char *claw_cap_build_llm_tools_json(const claw_cap_call_context_t *ctx,
                                    bool wrap_for_responses_api);
char *claw_cap_build_catalog(void);
const char *claw_cap_state_to_string(claw_cap_state_t state);

/* Exposes all root-agent LLM-visible tools for one request. */
extern const claw_core_context_provider_t claw_cap_root_agent_tools_provider;
/* Exposes sub-agent LLM-visible tools, excluding root-only tools. */
extern const claw_core_context_provider_t claw_cap_sub_agent_tools_provider;
/* Compatibility alias for root-agent tools. */
extern const claw_core_context_provider_t claw_cap_tools_provider;

#ifdef __cplusplus
}
#endif
