/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLAW_CORE_RESPONSE_STATUS_OK = 0,
    CLAW_CORE_RESPONSE_STATUS_ERROR = 1,
} claw_core_response_status_t;

typedef enum {
    CLAW_CORE_COMPLETION_DONE = 0,
} claw_core_completion_type_t;

typedef enum {
    CLAW_CORE_AGENT_LOOP_PHASE_IDLE = 0,
    CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_BUILD_ITERATION_CONTEXT,
    CLAW_CORE_AGENT_LOOP_PHASE_BUILDING_ITERATION_CONTEXT,
    CLAW_CORE_AGENT_LOOP_PHASE_BEFORE_LLM_HTTP,
    CLAW_CORE_AGENT_LOOP_PHASE_IN_LLM_HTTP,
    CLAW_CORE_AGENT_LOOP_PHASE_AFTER_LLM_BEFORE_TOOL,
    CLAW_CORE_AGENT_LOOP_PHASE_RUNNING_TOOL,
    CLAW_CORE_AGENT_LOOP_PHASE_FINALIZING,
} claw_core_agent_loop_phase_t;

#define CLAW_CORE_REQUEST_FLAG_PUBLISH_OUT_MESSAGE (1U << 0)
#define CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE (1U << 1)
#define CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT      (1U << 2)
#define CLAW_CORE_REQUEST_FLAG_PUBLISH_STAGE_MESSAGE  (1U << 3)

#define CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY (1U << 0)

typedef struct claw_core_state *claw_core_handle_t;

typedef struct {
    uint32_t request_id;
    uint32_t flags;
    const char *session_id;
    const char *user_text;
    const char *source_channel;
    const char *source_chat_id;
    const char *source_sender_id;
    const char *source_message_id;
    const char *source_cap;
    const char *target_channel;
    const char *target_chat_id;
} claw_core_request_t;

typedef enum {
    CLAW_CORE_CONTEXT_RECORD_USER = 1,
    CLAW_CORE_CONTEXT_RECORD_ASSISTANT_FINAL = 2,
    CLAW_CORE_CONTEXT_RECORD_ASSISTANT_TOOL = 3,
    CLAW_CORE_CONTEXT_RECORD_TOOL_RESULT = 4,
} claw_core_context_record_type_t;

typedef struct {
    claw_core_context_record_type_t type;
    const char *message_json;
    const char *text;
} claw_core_context_record_t;

typedef struct {
    const char *session_id;
    const claw_core_request_t *request;
    const claw_core_context_record_t *records;
    size_t record_count;
    bool turn_completed;
} claw_core_context_persist_batch_t;

typedef esp_err_t (*claw_core_persist_context_fn)(
    const claw_core_context_persist_batch_t *batch,
    void *user_ctx);

typedef esp_err_t (*claw_core_request_start_fn)(const claw_core_request_t *request,
                                                void *user_ctx);

typedef esp_err_t (*claw_core_request_gate_fn)(const claw_core_request_t *request,
                                               char *reject_message,
                                               size_t reject_message_size,
                                               void *user_ctx);

typedef esp_err_t (*claw_core_stage_note_fn)(const claw_core_request_t *request,
                                             char **out_note,
                                             void *user_ctx);

typedef struct claw_core_response claw_core_response_t;

typedef enum {
    CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT = 0,
    CLAW_CORE_CONTEXT_KIND_MESSAGES = 1,
    CLAW_CORE_CONTEXT_KIND_TOOLS = 2,
} claw_core_context_kind_t;

typedef struct {
    claw_core_context_kind_t kind;
    char *content;
} claw_core_context_t;

typedef esp_err_t (*claw_core_context_provider_collect_fn)(
    const claw_core_request_t *request,
    claw_core_context_t *out_context,
    void *user_ctx);

typedef struct {
    const char *name;
    claw_core_context_provider_collect_fn collect;
    void *user_ctx;
    uint32_t flags;
} claw_core_context_provider_t;

typedef esp_err_t (*claw_core_call_cap_fn)(const char *cap_name,
                                           const char *input_json,
                                           const claw_core_request_t *request,
                                           char **out_output,
                                           void *user_ctx);

typedef struct {
    uint32_t instance_id;
    const char *api_key;
    const char *backend_type;
    const char *model;
    const char *base_url;
    const char *auth_type;
    const char *max_tokens_field;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
    bool supports_tools;
    bool supports_vision;
    bool image_remote_url_only;
    const char *system_prompt;
    claw_core_persist_context_fn persist_context;
    void *persist_context_user_ctx;
    claw_core_request_gate_fn request_gate;
    void *request_gate_user_ctx;
    claw_core_request_start_fn on_request_start;
    void *on_request_start_user_ctx;
    claw_core_stage_note_fn collect_stage_note;
    void *collect_stage_note_user_ctx;
    claw_core_call_cap_fn call_cap;
    void *cap_user_ctx;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    BaseType_t task_core;
    uint32_t max_tool_iterations;
    uint32_t request_queue_len;
    uint32_t response_queue_len;
    size_t max_context_providers;
} claw_core_config_t;

struct claw_core_response {
    uint32_t request_id;
    claw_core_response_status_t status;
    claw_core_completion_type_t completion_type;
    char *target_channel;
    char *target_chat_id;
    char *text;
    char *error_message;
};

typedef struct {
    uint32_t request_id;
    const char *session_id;            /* may be NULL */
    const char *final_text;            /* may be NULL or empty */
    const char *context_providers_csv; /* providers that injected non-empty content */
    const char *tool_calls_csv;        /* tool calls invoked across all rounds */
} claw_core_completion_summary_t;

typedef void (*claw_core_completion_observer_fn)(const claw_core_completion_summary_t *summary,
                                                 void *user_ctx);

esp_err_t claw_core_create(const claw_core_config_t *config, claw_core_handle_t *out_core);
esp_err_t claw_core_start(claw_core_handle_t core);
esp_err_t claw_core_stop(claw_core_handle_t core, uint32_t timeout_ms);
esp_err_t claw_core_destroy(claw_core_handle_t core);
esp_err_t claw_core_add_context_provider(claw_core_handle_t core,
                                         const claw_core_context_provider_t *provider);
esp_err_t claw_core_add_completion_observer(claw_core_handle_t core,
                                            claw_core_completion_observer_fn observer,
                                            void *user_ctx);
esp_err_t claw_core_publish_stage_text(const claw_core_request_t *request, const char *text);
esp_err_t claw_core_submit(claw_core_handle_t core,
                           const claw_core_request_t *request,
                           uint32_t timeout_ms);
esp_err_t claw_core_cancel_request(claw_core_handle_t core, uint32_t request_id);
claw_core_agent_loop_phase_t claw_core_get_agent_loop_phase(claw_core_handle_t core);
esp_err_t claw_core_receive(claw_core_handle_t core,
                            claw_core_response_t *response,
                            uint32_t timeout_ms);
esp_err_t claw_core_receive_for(claw_core_handle_t core,
                                uint32_t request_id,
                                claw_core_response_t *response,
                                uint32_t timeout_ms);
void claw_core_response_free(claw_core_response_t *response);

#ifdef __cplusplus
}
#endif
