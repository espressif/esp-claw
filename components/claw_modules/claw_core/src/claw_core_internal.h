/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "claw_core.h"
#include "claw_core_llm.h"
#include "claw_utils_string.h"
#include "claw_utils_time.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLAW_CORE_DEFAULT_STACK_SIZE      (8 * 1024)
#define CLAW_CORE_DEFAULT_PRIORITY        5
#define CLAW_CORE_DEFAULT_CORE            tskNO_AFFINITY
#define CLAW_CORE_DEFAULT_REQUEST_Q       4
#define CLAW_CORE_DEFAULT_RESPONSE_Q      4
#define CLAW_CORE_DEFAULT_TOOL_ITERATIONS 10
#define CLAW_CORE_INSERT_QUEUE_LEN        4
#define CLAW_CORE_INFLIGHT_SESSION_ID_SIZE 128
#ifndef CLAW_CORE_LOG_SNIPPET_LEN
#define CLAW_CORE_LOG_SNIPPET_LEN         96
#endif
#define CLAW_CORE_TOOL_SUMMARY_MAX_LEN    768
#define CLAW_CORE_OBS_CSV_MAX             384
#define CLAW_CORE_MAX_COMPLETION_OBSERVERS 4

typedef struct {
    claw_core_request_t view;
    char *owned_session_id;
    char *owned_user_text;
    char *owned_source_channel;
    char *owned_source_chat_id;
    char *owned_source_sender_id;
    char *owned_source_message_id;
    char *owned_source_cap;
    char *owned_target_channel;
    char *owned_target_chat_id;
} claw_core_request_item_t;

typedef struct {
    claw_core_response_t view;
} claw_core_response_item_t;

typedef struct claw_core_pending_response {
    claw_core_response_item_t item;
    struct claw_core_pending_response *next;
} claw_core_pending_response_t;

typedef struct {
    bool valid;
    claw_core_context_kind_t kind;
    char *content;
} claw_core_cached_context_t;

typedef enum {
    CLAW_CORE_CONTROL_ABORT_REASON_NONE = 0,
    CLAW_CORE_CONTROL_ABORT_REASON_CANCEL,
    CLAW_CORE_CONTROL_ABORT_REASON_USER_INTERRUPT,
} claw_core_control_abort_reason_t;

struct claw_core_state {
    bool initialized;
    bool started;
    volatile bool stop_requested;
    uint32_t instance_id;
    char log_tag[32];
    char *system_prompt;
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
    claw_core_context_provider_t *context_providers;
    size_t context_provider_count;
    size_t context_provider_capacity;
    claw_core_llm_config_t llm_config;
    claw_llm_runtime_t *llm_runtime;
    SemaphoreHandle_t llm_lock;
    uint32_t task_stack_size;
    UBaseType_t task_priority;
    BaseType_t task_core;
    uint32_t max_tool_iterations;
    /* Waiting queue: entries start new agent loops when the core task is idle. */
    QueueHandle_t request_queue;
    QueueHandle_t response_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t response_lock;
    claw_core_pending_response_t *pending_head;
    claw_core_pending_response_t *pending_tail;
    SemaphoreHandle_t inflight_lock;
    uint32_t inflight_request_id;
    char inflight_session_id[CLAW_CORE_INFLIGHT_SESSION_ID_SIZE];
    claw_core_agent_loop_phase_t agent_loop_phase;
    volatile bool inflight_abort;
    claw_core_control_abort_reason_t inflight_abort_reason;
    claw_core_request_item_t insert_queue[CLAW_CORE_INSERT_QUEUE_LEN];
    size_t insert_queue_head;
    size_t insert_queue_count;
    struct {
        claw_core_completion_observer_fn fn;
        void *user_ctx;
    } completion_observers[CLAW_CORE_MAX_COMPLETION_OBSERVERS];
    size_t completion_observer_count;
};

typedef struct claw_core_state claw_core_state_t;

const char *claw_core_log_snippet(const char *text);
int claw_core_log_snippet_len(const char *text);
const char *claw_core_log_snippet_suffix(const char *text);
const char *claw_core_context_kind_to_string(claw_core_context_kind_t kind);
esp_err_t claw_core_append_tool_summary_line(char *summary,
                                             size_t summary_size,
                                             const char *tool_name,
                                             bool ok);
void claw_core_obs_csv_append(char *csv, size_t csv_size, const char *name, bool dedup);
void claw_core_log_tool_call_names(uint32_t request_id, const claw_core_llm_response_t *response);
void claw_core_check_timezone(void);
void claw_core_llm_config_free(claw_core_llm_config_t *config);
esp_err_t claw_core_llm_config_copy(claw_core_llm_config_t *dst,
                                    const claw_core_llm_config_t *src);
bool claw_core_llm_config_ready(claw_core_state_t *core,
                                char *message,
                                size_t message_size);

void claw_core_free_request_item(claw_core_request_item_t *item);
esp_err_t claw_core_ingress_submit(claw_core_state_t *core,
                                   const claw_core_request_t *request,
                                   uint32_t timeout_ms);
bool claw_core_ingress_dequeue_inserted_user_inputs(claw_core_state_t *core,
                                                    const char *session_id,
                                                    char **texts,
                                                    size_t max_count,
                                                    size_t *out_count);
void claw_core_ingress_clear_insert_queue_locked(claw_core_state_t *core);

void claw_core_free_response_item(claw_core_response_item_t *item);
esp_err_t claw_core_response_push(claw_core_state_t *core, claw_core_response_item_t *item);
esp_err_t claw_core_response_receive_for(claw_core_state_t *core,
                                         uint32_t request_id,
                                         claw_core_response_t *response,
                                         uint32_t timeout_ms);

void claw_core_free_cached_contexts(claw_core_cached_context_t *contexts, size_t count);
esp_err_t claw_core_collect_request_start_only_contexts(
    claw_core_state_t *core,
    const claw_core_request_item_t *request,
    claw_core_cached_context_t **out_contexts,
    size_t *out_count);
bool claw_core_cached_contexts_have_messages(const claw_core_cached_context_t *contexts,
                                             size_t count);
esp_err_t claw_core_build_iteration_context(claw_core_state_t *core,
                                            const claw_core_request_item_t *request,
                                            const cJSON *runtime_messages,
                                            const claw_core_cached_context_t *request_start_contexts,
                                            size_t request_start_context_count,
                                            bool inject_active_user,
                                            char **out_system_prompt,
                                            cJSON **out_messages,
                                            char **out_tools_json,
                                            char *obs_providers_csv,
                                            size_t obs_providers_csv_size);
esp_err_t claw_core_append_user_message(cJSON *messages, const char *text);
esp_err_t claw_core_append_assistant_tool_calls(cJSON *messages,
                                                const claw_core_llm_response_t *response);
esp_err_t claw_core_append_tool_results_messages(claw_core_state_t *core,
                                                 cJSON *runtime_messages,
                                                 const claw_core_llm_response_t *response,
                                                 const claw_core_request_t *request,
                                                 char *tool_summary,
                                                 size_t tool_summary_size,
                                                 char **out_tool_results_json);
void claw_core_finish_from_plain_text(uint32_t request_id,
                                      const claw_core_llm_response_t *llm_response,
                                      claw_core_response_t *response);

void claw_core_control_set_phase(claw_core_state_t *core, claw_core_agent_loop_phase_t phase);
bool claw_core_control_take_user_interrupt_http_abort(claw_core_state_t *core,
                                                      uint32_t request_id);
void claw_core_control_clear_user_interrupt_abort(claw_core_state_t *core,
                                                  uint32_t request_id);
esp_err_t claw_core_control_cancel_request(claw_core_state_t *core, uint32_t request_id);
claw_core_agent_loop_phase_t claw_core_control_get_phase(claw_core_state_t *core);

void claw_core_publish_out_message_if_requested(const claw_core_request_item_t *request,
                                                const claw_core_response_item_t *response);
void claw_core_publish_stage_tool_calls(const claw_core_request_t *request,
                                        const claw_core_llm_response_t *response,
                                        uint32_t iteration);
void claw_core_publish_stage_note_for_round(claw_core_state_t *core,
                                            const claw_core_request_t *request,
                                            uint32_t round_index);

esp_err_t claw_core_persist_context_user_messages_if_configured(claw_core_state_t *core,
                                                                const claw_core_request_t *request,
                                                                const char *const *texts,
                                                                size_t text_count,
                                                                bool *out_persisted);
esp_err_t claw_core_persist_context_tool_round_if_configured(
    claw_core_state_t *core,
    const claw_core_request_t *request,
    const char *assistant_tool_message_json,
    const char *tool_results_json);
esp_err_t claw_core_persist_context_final_if_configured(claw_core_state_t *core,
                                                        const claw_core_request_t *request,
                                                        const char *assistant_final_json,
                                                        const char *assistant_text);
void claw_core_log_context_persist_failure(const claw_core_request_t *request,
                                           const char *operation,
                                           esp_err_t err);
char *claw_core_build_context_failure_trace(const char *error_message,
                                            const char *tool_summary);

void claw_core_agent_loop_task(void *arg);

#ifdef __cplusplus
}
#endif
