/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "claw_core.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
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
} claw_memory_llm_config_t;

typedef struct {
    const char *session_root_dir;
    const char *memory_root_dir;
    size_t max_message_chars;
    uint32_t max_tool_iterations;
    claw_memory_llm_config_t llm;
    bool enable_async_extract_stage_note;
} claw_memory_config_t;

typedef struct {
    char id[40];
    char source[16];
    char content[256];
    uint16_t summary_ids[3];
    uint8_t summary_id_count;
    char tags[96];
    char keywords[128];
    uint32_t created_at;
    uint32_t updated_at;
    uint16_t access_count;
    uint8_t deleted;
} claw_memory_item_t;

typedef struct {
    const char *const *summary_labels;
    size_t summary_label_count;
    size_t limit;
} claw_memory_query_t;

esp_err_t claw_memory_init(const claw_memory_config_t *config);
esp_err_t claw_memory_register_group(void);
esp_err_t claw_memory_store(const claw_memory_item_t *item);
esp_err_t claw_memory_store_with_result(claw_memory_item_t *item, bool *out_changed);
esp_err_t claw_memory_recall(const claw_memory_query_t *query, char **out_json);
esp_err_t claw_memory_update(const claw_memory_item_t *item);
esp_err_t claw_memory_update_with_result(claw_memory_item_t *item, bool *out_changed);
esp_err_t claw_memory_forget(const char *memory_id);
esp_err_t claw_memory_forget_with_result(const char *memory_id,
                                         claw_memory_item_t *out_item,
                                         bool *out_changed);
esp_err_t claw_memory_list(char **out_json);
esp_err_t claw_memory_note_session_summary(const char *session_id,
                                           const char *summary_list);
esp_err_t claw_memory_item_primary_summary_label(const claw_memory_item_t *item,
                                                 char *buf,
                                                 size_t size);
esp_err_t claw_memory_persist_context_callback(const claw_core_context_persist_batch_t *batch,
                                               void *user_ctx);
esp_err_t claw_memory_delete_session_history(const char *session_id,
                                             bool *out_deleted_any);
esp_err_t claw_memory_request_gate_callback(const claw_core_request_t *request,
                                            char *reject_message,
                                            size_t reject_message_size,
                                            void *user_ctx);
esp_err_t claw_memory_request_start_callback(const claw_core_request_t *request,
                                             void *user_ctx);
esp_err_t claw_memory_request_mark_manual_write(uint32_t request_id);
esp_err_t claw_memory_stage_note_callback(const claw_core_request_t *request,
                                          char **out_note,
                                          void *user_ctx);

extern const claw_core_context_provider_t claw_memory_profile_provider;
extern const claw_core_context_provider_t claw_memory_long_term_provider;
extern const claw_core_context_provider_t claw_memory_long_term_lightweight_provider;
extern const claw_core_context_provider_t claw_memory_session_history_provider;

#ifdef __cplusplus
}
#endif
