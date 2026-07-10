/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cJSON.h"
#include "claw_memory.h"
#include "llm/claw_llm_runtime.h"

#define CLAW_MEMORY_DEFAULT_MAX_MESSAGE_CHARS    4096
#define CLAW_MEMORY_DEFAULT_MAX_TOOL_ITERATIONS  10
#define CLAW_MEMORY_MAX_PATH                     192
#define CLAW_MEMORY_MAX_SUMMARIES                3
#define CLAW_MEMORY_MAX_LABEL_CHARS              8
#define CLAW_MEMORY_MAX_LABEL_TEXT               40
#define CLAW_MEMORY_MAX_ACTIVE_ITEMS             128
#define CLAW_MEMORY_COMPACT_CHANGE_THRESHOLD     5
#define CLAW_MEMORY_COMPACT_SIZE_THRESHOLD       (32 * 1024)
#define CLAW_MEMORY_SESSION_SIZE_LIMIT           (150 * 1024)
#define CLAW_MEMORY_RECALL_DEFAULT_LIMIT         8
#define CLAW_MEMORY_RECORDS_FILE                 "memory_records.jsonl"
#define CLAW_MEMORY_INDEX_FILE                   "memory_index.json"
#define CLAW_MEMORY_DIGEST_FILE                  "memory_digest.log"
#define CLAW_MEMORY_MARKDOWN_FILE                "MEMORY.md"
#define CLAW_MEMORY_SOUL_FILE                    "soul.md"
#define CLAW_MEMORY_IDENTITY_FILE                "identity.md"
#define CLAW_MEMORY_USER_FILE                    "user.md"
#define CLAW_MEMORY_AUTO_EXTRACT_MAX_ITEMS       3
typedef enum {
    CLAW_MEMORY_BACKEND_FORMAT_UNKNOWN = 0,
    CLAW_MEMORY_BACKEND_FORMAT_OPENAI = 1,
    CLAW_MEMORY_BACKEND_FORMAT_ANTHROPIC = 2,
} claw_memory_backend_format_t;

typedef struct {
    int initialized;
    char session_root_dir[CLAW_MEMORY_MAX_PATH];
    char memory_root_dir[CLAW_MEMORY_MAX_PATH];
    char markdown_path[CLAW_MEMORY_MAX_PATH];
    char records_path[CLAW_MEMORY_MAX_PATH];
    char index_path[CLAW_MEMORY_MAX_PATH];
    char digest_path[CLAW_MEMORY_MAX_PATH];
    char soul_path[CLAW_MEMORY_MAX_PATH];
    char identity_path[CLAW_MEMORY_MAX_PATH];
    char user_path[CLAW_MEMORY_MAX_PATH];
    size_t max_message_chars;
    uint32_t max_tool_iterations;
    claw_memory_backend_format_t backend_format;
    uint32_t write_changes_since_compact;
    uint32_t next_memory_seq;
} claw_memory_state_t;

typedef struct {
    claw_memory_item_t *items;
    size_t count;
    size_t capacity;
} claw_memory_item_list_t;

typedef enum {
    CLAW_MEMORY_MESSAGE_INTENT_NONE = 0,
    CLAW_MEMORY_MESSAGE_INTENT_FORGET,
    CLAW_MEMORY_MESSAGE_INTENT_REPLACE,
} claw_memory_message_intent_t;

extern claw_memory_state_t s_memory;

void safe_copy(char *dst, size_t dst_size, const char *src);
char *dup_printf(const char *fmt, ...);
size_t claw_memory_text_buffer_size(size_t max_chars);
char *claw_memory_session_path_dup(const char *session_id);
void claw_memory_normalize_session_text(const char *src,
                                        char *dst,
                                        size_t dst_size,
                                        size_t max_chars);
claw_memory_backend_format_t claw_memory_backend_format_from_type(const char *backend_type);
esp_err_t claw_memory_write_session_raw_record(FILE *file,
                                               const char *json_text,
                                               uint32_t *out_offset,
                                               uint32_t *out_length);
bool line_list_contains_item(const char *list, const char *item);
esp_err_t line_list_append_unique(char **list, const char *item);
esp_err_t line_list_merge_unique(char **dst, const char *src);
char *claw_memory_format_update_stage_note(const char *summary_list);
uint32_t claw_memory_now_sec(void);
size_t utf8_sequence_len(unsigned char ch);
bool utf8_sequence_valid(const unsigned char *src, size_t len);
void utf8_copy_chars(char *dst, size_t dst_size, const char *src, size_t max_chars);
bool utf8_string_is_valid(const char *src);
void trim_whitespace(char *text);
bool text_contains_ascii_ci(const char *haystack, const char *needle);
bool utf8_matches_literal(const unsigned char *src, size_t seq_len, const char *literal);
bool utf8_is_common_punctuation(const unsigned char *src, size_t seq_len);
void normalize_text_for_key(const char *src, char *dst, size_t dst_size);
esp_err_t ensure_dir_recursive(const char *path);
esp_err_t ensure_parent_dir(const char *path);
esp_err_t read_file_dup(const char *path, char **out_buf);
esp_err_t write_file_text(const char *path, const char *text);
esp_err_t append_file_text(const char *path, const char *text);
size_t file_size_bytes(const char *path);
esp_err_t ensure_file_with_default(const char *path, const char *default_text);
esp_err_t claw_memory_join_path(char *dst, size_t dst_size, const char *dir, const char *name);

esp_err_t claw_memory_auto_extract_prepare_with_runtime(claw_llm_runtime_t *runtime,
                                                        const char *user_text,
                                                        claw_memory_message_intent_t *out_message_intent,
                                                        char **out_llm_text);
esp_err_t claw_memory_auto_extract_apply_result(const char *llm_text,
                                                claw_memory_message_intent_t message_intent,
                                                char **out_memory_summary);
esp_err_t claw_memory_async_extract_init(const claw_memory_config_t *config);
esp_err_t claw_memory_async_extract_ensure_started(const claw_core_request_t *request);

void claw_memory_make_id(char *dst, size_t dst_size);
void claw_memory_build_item_key(const claw_memory_item_t *item,
                                char *key,
                                size_t key_size);
void claw_memory_normalize_item_metadata(claw_memory_item_t *item);
void claw_memory_collect_summary_labels(const claw_memory_item_t *item,
                                        char labels[][CLAW_MEMORY_MAX_LABEL_TEXT],
                                        size_t *label_count);
esp_err_t claw_memory_append_item_summary_labels(const claw_memory_item_t *item,
                                                 char **out_summary_list);
bool claw_memory_items_semantically_match(const claw_memory_item_t *existing,
                                          const claw_memory_item_t *incoming);
bool claw_memory_items_equivalent_for_update(const claw_memory_item_t *existing,
                                             const claw_memory_item_t *replacement);
esp_err_t claw_memory_replace_conflicting_items(const claw_memory_item_t *incoming,
                                                claw_memory_message_intent_t message_intent);
cJSON *claw_memory_parse_llm_json_document(const char *text);
char *claw_memory_summary_catalog_dup(void);

esp_err_t claw_memory_load_index(cJSON **out_root);
esp_err_t claw_memory_save_index(cJSON *root);
cJSON *claw_memory_find_summary_by_label(cJSON *index_root, const char *label);
cJSON *claw_memory_find_summary_by_id(cJSON *index_root, int summary_id);
esp_err_t claw_memory_ensure_summary_label(cJSON *index_root,
                                           const char *label,
                                           int preferred_id,
                                           int *out_summary_id);
void claw_memory_adjust_summary_stats(cJSON *index_root,
                                      const claw_memory_item_t *item,
                                      int ref_delta);
void claw_memory_remove_unused_summaries(cJSON *index_root);
void claw_memory_item_list_free(claw_memory_item_list_t *list);
esp_err_t claw_memory_item_list_push(claw_memory_item_list_t *list,
                                     const claw_memory_item_t *item);
int claw_memory_find_item_index(const claw_memory_item_list_t *list, const char *memory_id);
cJSON *claw_memory_item_to_json(const claw_memory_item_t *item, cJSON *index_root);
esp_err_t claw_memory_append_record(const claw_memory_item_t *item);
esp_err_t claw_memory_load_current_items(claw_memory_item_list_t *out_list);
int claw_memory_sort_by_priority_desc(const void *a, const void *b);
void claw_memory_append_digest_line(const char *action,
                                    const claw_memory_item_t *item,
                                    const char *extra);
void claw_memory_rebuild_keyword_index(cJSON *index_root,
                                       const claw_memory_item_list_t *items);
esp_err_t claw_memory_export_markdown_internal(char **out_markdown,
                                               const claw_memory_item_list_t *items,
                                               cJSON *index_root);
esp_err_t claw_memory_sync_markdown(const claw_memory_item_list_t *items, cJSON *index_root);
esp_err_t claw_memory_compact_internal(bool append_digest);
esp_err_t claw_memory_maybe_compact(void);
esp_err_t claw_memory_profile_init_defaults(void);
