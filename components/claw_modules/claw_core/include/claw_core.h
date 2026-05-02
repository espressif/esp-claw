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

#define CLAW_CORE_REQUEST_FLAG_PUBLISH_OUT_MESSAGE (1U << 0)
#define CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE (1U << 1)

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

typedef esp_err_t (*claw_core_append_session_turn_fn)(const char *session_id,
                                                      const char *user_text,
                                                      const char *assistant_text,
                                                      void *user_ctx);

typedef esp_err_t (*claw_core_request_start_fn)(const claw_core_request_t *request,
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
} claw_core_context_provider_t;

typedef esp_err_t (*claw_core_call_cap_fn)(const char *cap_name,
                                           const char *input_json,
                                           const claw_core_request_t *request,
                                           char **out_output,
                                           void *user_ctx);

typedef struct {
    const char *api_key;
    const char *backend_type;
    const char *profile;
    const char *provider;
    const char *model;
    const char *base_url;
    const char *auth_type;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
    const char *system_prompt;
    claw_core_append_session_turn_fn append_session_turn;
    void *append_session_turn_user_ctx;
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

esp_err_t claw_core_init(const claw_core_config_t *config);
esp_err_t claw_core_start(void);
esp_err_t claw_core_add_context_provider(const claw_core_context_provider_t *provider);
esp_err_t claw_core_add_completion_observer(claw_core_completion_observer_fn observer,
                                            void *user_ctx);
esp_err_t claw_core_call_cap(const char *cap_name,
                             const char *input_json,
                             const claw_core_request_t *request,
                             char **out_output);
esp_err_t claw_core_submit(const claw_core_request_t *request, uint32_t timeout_ms);
esp_err_t claw_core_cancel_request(uint32_t request_id);
esp_err_t claw_core_receive(claw_core_response_t *response, uint32_t timeout_ms);
esp_err_t claw_core_receive_for(uint32_t request_id,
                                claw_core_response_t *response,
                                uint32_t timeout_ms);
void claw_core_response_free(claw_core_response_t *response);

#ifdef __cplusplus
}
#endif
