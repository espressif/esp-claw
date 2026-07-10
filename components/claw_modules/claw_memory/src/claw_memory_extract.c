/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "claw_memory";

extern esp_err_t claw_memory_forget(const char *memory_id);
static bool text_contains_token(const char *haystack, const char *needle)
{
    return (haystack && needle && needle[0]) &&
           ((strstr(haystack, needle) != NULL) || text_contains_ascii_ci(haystack, needle));
}

static claw_memory_message_intent_t claw_memory_parse_message_intent(const char *value)
{
    if (!value || !value[0]) {
        return CLAW_MEMORY_MESSAGE_INTENT_NONE;
    }
    if (strcmp(value, "forget") == 0) {
        return CLAW_MEMORY_MESSAGE_INTENT_FORGET;
    }
    if (strcmp(value, "replace") == 0) {
        return CLAW_MEMORY_MESSAGE_INTENT_REPLACE;
    }
    return CLAW_MEMORY_MESSAGE_INTENT_NONE;
}

static esp_err_t claw_memory_llm_chat_with_runtime(claw_llm_runtime_t *runtime,
                                                   const char *system_prompt,
                                                   const char *user_text,
                                                   char **out_text,
                                                   char **out_error_message)
{
    claw_llm_response_t response = {0};
    cJSON *messages = NULL;
    cJSON *user_msg = NULL;
    esp_err_t err;

    if (out_text) {
        *out_text = NULL;
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!system_prompt || !user_text || !out_text || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!runtime) {
        *out_error_message = dup_printf("auto_extract requires an explicit runtime");
        return ESP_ERR_INVALID_ARG;
    }

    messages = cJSON_CreateArray();
    user_msg = cJSON_CreateObject();
    if (!messages || !user_msg) {
        cJSON_Delete(messages);
        cJSON_Delete(user_msg);
        *out_error_message = dup_printf("Out of memory building messages");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_text);
    cJSON_AddItemToArray(messages, user_msg);

    err = claw_llm_runtime_chat(runtime,
                                &(claw_llm_chat_request_t) {
                                    .system_prompt = system_prompt,
                                    .messages = messages,
                                    .tools_json = NULL,
                                },
                                &response,
                                out_error_message);
    cJSON_Delete(messages);
    if (err != ESP_OK) {
        claw_llm_response_free(&response);
        return err;
    }
    if (response.tool_call_count > 0) {
        claw_llm_response_free(&response);
        *out_error_message = dup_printf("LLM returned unsupported tool calls");
        return ESP_ERR_NOT_SUPPORTED;
    }

    *out_text = response.text;
    response.text = NULL;
    claw_llm_response_free(&response);
    return ESP_OK;
}

static void claw_memory_build_semantic_key(const claw_memory_item_t *item,
                                           char *dst,
                                           size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    normalize_text_for_key(item ? item->content : NULL, dst, dst_size);
}

bool claw_memory_items_semantically_match(const claw_memory_item_t *existing,
                                          const claw_memory_item_t *incoming)
{
    char existing_key[160];
    char incoming_key[160];
    size_t existing_len;
    size_t incoming_len;

    if (!existing || !incoming || existing->deleted) {
        return false;
    }

    claw_memory_build_semantic_key(existing, existing_key, sizeof(existing_key));
    claw_memory_build_semantic_key(incoming, incoming_key, sizeof(incoming_key));
    if (!existing_key[0] || !incoming_key[0]) {
        return false;
    }
    if (strcmp(existing_key, incoming_key) == 0) {
        return true;
    }

    existing_len = strlen(existing_key);
    incoming_len = strlen(incoming_key);
    if (existing_len >= 12 && incoming_len >= 12) {
        return strstr(existing_key, incoming_key) != NULL ||
               strstr(incoming_key, existing_key) != NULL;
    }
    return false;
}

bool claw_memory_items_equivalent_for_update(const claw_memory_item_t *existing,
                                             const claw_memory_item_t *replacement)
{
    if (!existing || !replacement) {
        return false;
    }

    return strcmp(existing->source, replacement->source) == 0 &&
           strcmp(existing->content, replacement->content) == 0 &&
           strcmp(existing->tags, replacement->tags) == 0 &&
           strcmp(existing->keywords, replacement->keywords) == 0;
}

static bool claw_memory_csv_contains_term(const char *csv_text, const char *term)
{
    char copy[160];
    char *token;
    char *saveptr = NULL;

    if (!csv_text || !csv_text[0] || !term || !term[0]) {
        return false;
    }

    safe_copy(copy, sizeof(copy), csv_text);
    token = strtok_r(copy, ",;/|", &saveptr);
    while (token) {
        trim_whitespace(token);
        if (token[0] && strcmp(token, term) == 0) {
            return true;
        }
        token = strtok_r(NULL, ",;/|", &saveptr);
    }
    return false;
}

static bool claw_memory_csv_has_overlap(const char *lhs, const char *rhs)
{
    char copy[160];
    char *token;
    char *saveptr = NULL;

    if (!lhs || !lhs[0] || !rhs || !rhs[0]) {
        return false;
    }

    safe_copy(copy, sizeof(copy), lhs);
    token = strtok_r(copy, ",;/|", &saveptr);
    while (token) {
        trim_whitespace(token);
        if (token[0] && claw_memory_csv_contains_term(rhs, token)) {
            return true;
        }
        token = strtok_r(NULL, ",;/|", &saveptr);
    }
    return false;
}

static bool claw_memory_items_conflict_for_replacement(const claw_memory_item_t *existing,
                                                       const claw_memory_item_t *incoming)
{
    if (!existing || !incoming || existing->deleted) {
        return false;
    }
    if (existing->id[0] && incoming->id[0] && strcmp(existing->id, incoming->id) == 0) {
        return false;
    }
    if (claw_memory_items_semantically_match(existing, incoming)) {
        return false;
    }
    if (!claw_memory_csv_has_overlap(existing->tags, incoming->tags) &&
        !claw_memory_csv_has_overlap(existing->keywords, incoming->keywords) &&
        !claw_memory_csv_has_overlap(existing->tags, incoming->keywords) &&
        !claw_memory_csv_has_overlap(existing->keywords, incoming->tags)) {
        return false;
    }
    return true;
}

esp_err_t claw_memory_replace_conflicting_items(const claw_memory_item_t *incoming,
                                                claw_memory_message_intent_t message_intent)
{
    claw_memory_item_list_t items = {0};
    size_t i;
    esp_err_t err;

    if (!incoming || !incoming->id[0] || message_intent != CLAW_MEMORY_MESSAGE_INTENT_REPLACE) {
        return ESP_OK;
    }

    err = claw_memory_load_current_items(&items);
    if (err != ESP_OK) {
        return err;
    }

    for (i = 0; i < items.count; i++) {
        if (!claw_memory_items_conflict_for_replacement(&items.items[i], incoming)) {
            continue;
        }
        err = claw_memory_forget(items.items[i].id);
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            claw_memory_item_list_free(&items);
            return err;
        }
    }

    claw_memory_item_list_free(&items);
    return ESP_OK;
}

static bool claw_memory_term_is_grounded(const char *term,
                                         const char *content,
                                         const char *tags)
{
    if (!term || !term[0]) {
        return false;
    }
    return text_contains_token(content, term) || text_contains_token(tags, term);
}

static size_t claw_memory_append_csv_terms(char *dst,
                                           size_t dst_size,
                                           size_t current_count,
                                           size_t max_terms,
                                           const char *src,
                                           const char *content,
                                           const char *tags,
                                           bool require_grounding)
{
    char copy[192];
    char *token;
    char *saveptr = NULL;

    if (!dst || dst_size == 0 || !src || !src[0] || current_count >= max_terms) {
        return current_count;
    }

    safe_copy(copy, sizeof(copy), src);
    token = strtok_r(copy, ",;/|", &saveptr);
    while (token && current_count < max_terms) {
        size_t off;
        int written;

        trim_whitespace(token);
        if (!token[0] || claw_memory_csv_contains_term(dst, token)) {
            token = strtok_r(NULL, ",;/|", &saveptr);
            continue;
        }
        if (require_grounding && !claw_memory_term_is_grounded(token, content, tags)) {
            token = strtok_r(NULL, ",;/|", &saveptr);
            continue;
        }

        off = strlen(dst);
        written = snprintf(dst + off,
                           dst_size - off,
                           "%s%s",
                           dst[0] ? "," : "",
                           token);
        if (written < 0 || (size_t)written >= dst_size - off) {
            break;
        }
        current_count++;
        token = strtok_r(NULL, ",;/|", &saveptr);
    }

    return current_count;
}

void claw_memory_normalize_item_metadata(claw_memory_item_t *item)
{
    char normalized_tags[sizeof(item->tags)] = {0};
    char normalized_keywords[sizeof(item->keywords)] = {0};
    size_t keyword_count = 0;

    if (!item) {
        return;
    }

    claw_memory_append_csv_terms(normalized_tags,
                                 sizeof(normalized_tags),
                                 0,
                                 CLAW_MEMORY_MAX_SUMMARIES,
                                 item->tags,
                                 item->content,
                                 item->tags,
                                 false);
    safe_copy(item->tags, sizeof(item->tags), normalized_tags);

    keyword_count = claw_memory_append_csv_terms(normalized_keywords,
                                                 sizeof(normalized_keywords),
                                                 0,
                                                 CLAW_MEMORY_MAX_SUMMARIES,
                                                 item->keywords,
                                                 item->content,
                                                 item->tags,
                                                 true);
    if (keyword_count < CLAW_MEMORY_MAX_SUMMARIES) {
        claw_memory_append_csv_terms(normalized_keywords,
                                     sizeof(normalized_keywords),
                                     keyword_count,
                                     CLAW_MEMORY_MAX_SUMMARIES,
                                     item->tags,
                                     item->content,
                                     item->tags,
                                     false);
    }
    safe_copy(item->keywords, sizeof(item->keywords), normalized_keywords);
}

void claw_memory_make_id(char *dst, size_t dst_size)
{
    uint32_t now = claw_memory_now_sec();

    s_memory.next_memory_seq++;
    snprintf(dst, dst_size, "mem-%" PRIu32 "-%04" PRIu32, now, s_memory.next_memory_seq % 10000);
}

void claw_memory_build_item_key(const claw_memory_item_t *item,
                                char *key,
                                size_t key_size)
{
    char normalized[160];
    char normalized_prefix[37];

    normalize_text_for_key(item ? item->content : NULL, normalized, sizeof(normalized));
    if (!normalized[0]) {
        snprintf(key, key_size, "memory-%" PRIu32, claw_memory_now_sec());
        return;
    }

    utf8_copy_chars(normalized_prefix, sizeof(normalized_prefix), normalized, 36);
    snprintf(key, key_size, "%s", normalized_prefix);
}

static bool claw_memory_label_exists(char labels[][CLAW_MEMORY_MAX_LABEL_TEXT],
                                     size_t count,
                                     const char *candidate)
{
    size_t i;

    if (!candidate || !candidate[0]) {
        return true;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(labels[i], candidate) == 0) {
            return true;
        }
    }
    return false;
}

void claw_memory_collect_summary_labels(const claw_memory_item_t *item,
                                        char labels[][CLAW_MEMORY_MAX_LABEL_TEXT],
                                        size_t *label_count)
{
    size_t count = 0;
    char tag_copy[128];
    char *token;
    char *saveptr = NULL;

    if (!labels || !label_count || !item) {
        return;
    }
    *label_count = 0;

    safe_copy(tag_copy, sizeof(tag_copy), item->tags);
    token = strtok_r(tag_copy, ",;/|", &saveptr);
    while (token && count < CLAW_MEMORY_MAX_SUMMARIES) {
        char trimmed[CLAW_MEMORY_MAX_LABEL_TEXT];
        char limited[CLAW_MEMORY_MAX_LABEL_TEXT];

        safe_copy(trimmed, sizeof(trimmed), token);
        trim_whitespace(trimmed);
        utf8_copy_chars(limited, sizeof(limited), trimmed, CLAW_MEMORY_MAX_LABEL_CHARS);
        if (limited[0] && !claw_memory_label_exists(labels, count, limited)) {
            safe_copy(labels[count++], CLAW_MEMORY_MAX_LABEL_TEXT, limited);
        }
        token = strtok_r(NULL, ",;/|", &saveptr);
    }
    *label_count = count;
}

esp_err_t claw_memory_item_primary_summary_label(const claw_memory_item_t *item,
                                                 char *buf,
                                                 size_t size)
{
    char labels[CLAW_MEMORY_MAX_SUMMARIES][CLAW_MEMORY_MAX_LABEL_TEXT];
    size_t label_count = 0;

    if (!item || !buf || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = '\0';
    claw_memory_collect_summary_labels(item, labels, &label_count);
    if (label_count == 0 || !labels[0][0]) {
        return ESP_ERR_NOT_FOUND;
    }

    safe_copy(buf, size, labels[0]);
    return ESP_OK;
}

cJSON *claw_memory_parse_llm_json_document(const char *text)
{
    const char *start = NULL;
    const char *end = NULL;
    char *snippet = NULL;
    cJSON *root = NULL;

    if (!text || !text[0]) {
        return NULL;
    }

    root = cJSON_Parse(text);
    if (root) {
        return root;
    }

    start = strchr(text, '{');
    end = strrchr(text, '}');
    if (start && end && end >= start) {
        size_t len = (size_t)(end - start + 1);

        snippet = calloc(1, len + 1);
        if (!snippet) {
            return NULL;
        }
        memcpy(snippet, start, len);
        root = cJSON_Parse(snippet);
        free(snippet);
        if (root) {
            return root;
        }
    }

    start = strchr(text, '[');
    end = strrchr(text, ']');
    if (start && end && end >= start) {
        size_t len = (size_t)(end - start + 1);

        snippet = calloc(1, len + 1);
        if (!snippet) {
            return NULL;
        }
        memcpy(snippet, start, len);
        root = cJSON_Parse(snippet);
        free(snippet);
    }
    return root;
}

static void claw_memory_copy_csv_field_from_json(cJSON *json,
                                                 const char *key,
                                                 char *dst,
                                                 size_t dst_size)
{
    cJSON *value = NULL;

    if (!json || !key || !dst || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    value = cJSON_GetObjectItem(json, key);
    if (cJSON_IsString(value)) {
        safe_copy(dst, dst_size, cJSON_GetStringValue(value));
        return;
    }
    if (cJSON_IsArray(value)) {
        int i;
        size_t off = 0;

        for (i = 0; i < cJSON_GetArraySize(value) && i < CLAW_MEMORY_MAX_SUMMARIES; i++) {
            const char *entry = cJSON_GetStringValue(cJSON_GetArrayItem(value, i));
            int written;

            if (!entry || !entry[0]) {
                continue;
            }
            written = snprintf(dst + off, dst_size - off, "%s%s", off ? "," : "", entry);
            if (written < 0 || (size_t)written >= dst_size - off) {
                break;
            }
            off += (size_t)written;
        }
    }
}

static esp_err_t claw_memory_store_extracted_candidate(cJSON *candidate,
                                                       claw_memory_message_intent_t message_intent,
                                                       char **out_memory_summary)
{
    claw_memory_item_t *item = NULL;
    bool changed = false;
    esp_err_t err;

    if (!candidate || !cJSON_IsObject(candidate)) {
        return ESP_ERR_INVALID_ARG;
    }

    item = calloc(1, sizeof(*item));
    if (!item) {
        return ESP_ERR_NO_MEM;
    }

    safe_copy(item->content,
              sizeof(item->content),
              cJSON_GetStringValue(cJSON_GetObjectItem(candidate, "content")));
    safe_copy(item->source,
              sizeof(item->source),
              cJSON_GetStringValue(cJSON_GetObjectItem(candidate, "source")));
    claw_memory_copy_csv_field_from_json(candidate, "tags", item->tags, sizeof(item->tags));
    if (!item->tags[0]) {
        claw_memory_copy_csv_field_from_json(candidate,
                                             "summary_labels",
                                             item->tags,
                                             sizeof(item->tags));
    }
    claw_memory_copy_csv_field_from_json(candidate, "keywords", item->keywords, sizeof(item->keywords));
    if (!item->keywords[0]) {
        claw_memory_copy_csv_field_from_json(candidate,
                                             "tags",
                                             item->keywords,
                                             sizeof(item->keywords));
    }
    if (!item->source[0]) {
        safe_copy(item->source, sizeof(item->source), "auto_llm");
    }
    trim_whitespace(item->content);
    trim_whitespace(item->tags);
    trim_whitespace(item->keywords);
    claw_memory_normalize_item_metadata(item);

    if (!item->content[0]) {
        free(item);
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_memory_store_with_result(item, &changed);
    if (err == ESP_OK && changed) {
        esp_err_t replace_err = claw_memory_replace_conflicting_items(item, message_intent);

        if (replace_err != ESP_OK) {
            free(item);
            return replace_err;
        }
    }
    if (err == ESP_OK && changed && out_memory_summary) {
        esp_err_t append_err = claw_memory_append_item_summary_labels(item, out_memory_summary);

        if (append_err != ESP_OK) {
            free(item);
            return append_err;
        }
    }
    if (err == ESP_OK && !changed) {
        char item_key[48];

        claw_memory_build_item_key(item, item_key, sizeof(item_key));
        ESP_LOGI(TAG, "auto_extract skipped duplicate key=%s", item_key);
    }
    free(item);
    return err;
}

esp_err_t claw_memory_auto_extract_prepare_with_runtime(claw_llm_runtime_t *runtime,
                                                        const char *user_text,
                                                        claw_memory_message_intent_t *out_message_intent,
                                                        char **out_llm_text)
{
    static const char *const system_prompt =
        "You extract long-term memory candidates from the user's latest message for an ESP32 agent.\n"
        "Return JSON only, with schema {\"intent\":\"none|forget|replace\",\"memories\":[{\"content\":\"...\",\"tags\":[\"...\"],\"keywords\":[\"...\"],\"source\":\"optional\"}]}.\n"
        "Rules:\n"
        "- intent=forget when the user wants the assistant to stop remembering, delete, erase, or remove a memory. In that case return an empty memories array.\n"
        "- intent=replace when the user is correcting or revising a previously stated durable fact or preference. Output only the corrected durable fact.\n"
        "- intent=none for everything else, including requests to keep remembering something.\n"
        "- Extract only durable user-related long-term memory or reusable rules grounded in the current user message.\n"
        "- Never store instructions that primarily change the assistant's own persona, identity, role, tone, speech style, or persistent behavior. Those belong in profile memory such as soul.md or identity.md, not long-term memory. For those requests return an empty memories array.\n"
        "- Usually return 0 to 3 memories.\n"
        "- Normalize content into a concise memory fact, not the raw user quote.\n"
        "- Keep wording stable and minimal so the same fact maps to the same content across turns.\n"
        "- Preserve the user's language whenever possible; do not translate just to fit a preferred vocabulary.\n"
        "- Tags should be short retrieval labels stored by the system; prefer 1 to 2, and never add filler just to reach a limit.\n"
        "- Keywords should be exact retrieval keywords stored in keyword_index; prefer 1 to 3, and fewer is better when enough.\n"
        "- Keywords must be grounded in the normalized memory fact or its tags. Do not invent related synonyms or fine-grained expansions.\n"
        "- Do not output generic labels such as profile, preference, memory, information, fact, user data, or their equivalents in any language unless they are truly the best retrieval key.\n"
        "- Prefer concise, domain-specific concepts that appear directly in the user's message, such as a role, city, food, language, product, team, or reusable preference.\n"
        "- Good tags/keywords stay close to the user's wording; bad tags/keywords are vague category expansions, loose synonyms, or inferred related concepts.\n"
        "- If the user is correcting themselves, replacing one fact with another, or revising a prior statement about the user, extract only the corrected durable fact.\n"
        "- Avoid duplicates; if the same memory appears multiple ways in this turn, output it once.\n"
        "- If you cannot produce a precise tag, skip that memory instead of using a vague tag.\n"
        "- Never extract facts that are stated only by the assistant, only by previous session history, or only by existing memory labels.\n"
        "- If the user asks to forget, delete, remove, or stop remembering something, return {\"memories\":[]}.\n"
        "- If the user is asking the assistant to behave differently from now on, adopt a role, speak in a certain style every time, or maintain a persistent persona, return {\"intent\":\"none\",\"memories\":[]}.\n"
        "- If the current user message is a question, request, or recall prompt rather than a self-disclosure, return {\"memories\":[]}.\n"
        "- Ignore transient chit-chat, assistant-only information, and short-term tasks.\n"
        "- If there is no durable memory, return {\"memories\":[]}.\n";
    char *summary_catalog = NULL;
    char *prompt = NULL;
    char *llm_text = NULL;
    char *error_message = NULL;
    cJSON *root = NULL;
    claw_memory_message_intent_t message_intent = CLAW_MEMORY_MESSAGE_INTENT_NONE;

    if (out_message_intent) {
        *out_message_intent = CLAW_MEMORY_MESSAGE_INTENT_NONE;
    }
    if (out_llm_text) {
        *out_llm_text = NULL;
    }
    if (!user_text || !user_text[0] || !out_message_intent || !out_llm_text) {
        return ESP_OK;
    }

    summary_catalog = claw_memory_summary_catalog_dup();
    prompt = dup_printf("Existing summary labels:\n%s\nCurrent user message:\n%s\n",
                        summary_catalog ? summary_catalog : "- (empty)\n",
                        user_text);
    free(summary_catalog);
    if (!prompt) {
        return ESP_ERR_NO_MEM;
    }

    if (claw_memory_llm_chat_with_runtime(runtime,
                                          system_prompt,
                                          prompt,
                                          &llm_text,
                                          &error_message) != ESP_OK) {
        ESP_LOGW(TAG,
                 "auto_extract llm failed: %s",
                 error_message ? error_message : "unknown_error");
        free(prompt);
        free(llm_text);
        free(error_message);
        return ESP_FAIL;
    }
    free(prompt);

    root = claw_memory_parse_llm_json_document(llm_text);
    if (root) {
        const char *intent_value = cJSON_GetStringValue(cJSON_GetObjectItem(root, "intent"));

        message_intent = claw_memory_parse_message_intent(intent_value);
        cJSON_Delete(root);
    } else {
        ESP_LOGW(TAG, "auto_extract llm returned invalid json for intent parse: %.96s", llm_text);
    }
    *out_message_intent = message_intent;
    if (message_intent == CLAW_MEMORY_MESSAGE_INTENT_FORGET) {
        ESP_LOGI(TAG, "auto_extract skipped explicit forget request");
    }
    *out_llm_text = llm_text;
    return ESP_OK;
}

esp_err_t claw_memory_auto_extract_apply_result(const char *llm_text,
                                                claw_memory_message_intent_t message_intent,
                                                char **out_memory_summary)
{
    cJSON *root = NULL;
    cJSON *memories = NULL;
    esp_err_t result = ESP_OK;
    int i;
    int stored = 0;

    if (message_intent == CLAW_MEMORY_MESSAGE_INTENT_FORGET || !llm_text || !llm_text[0]) {
        return ESP_OK;
    }

    root = claw_memory_parse_llm_json_document(llm_text);
    if (!root) {
        ESP_LOGW(TAG, "auto_extract llm returned invalid json: %.96s", llm_text);
        return ESP_FAIL;
    }

    memories = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "memories");
    if (!cJSON_IsArray(memories)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    for (i = 0; i < cJSON_GetArraySize(memories) && i < CLAW_MEMORY_AUTO_EXTRACT_MAX_ITEMS; i++) {
        esp_err_t err = claw_memory_store_extracted_candidate(cJSON_GetArrayItem(memories, i),
                                                              message_intent,
                                                              out_memory_summary);

        if (err == ESP_OK) {
            stored++;
        } else {
            ESP_LOGW(TAG, "auto_extract candidate[%d] skipped err=%s", i, esp_err_to_name(err));
            result = err;
        }
    }

    ESP_LOGI(TAG, "auto_extract llm candidates=%d stored=%d", cJSON_GetArraySize(memories), stored);
    cJSON_Delete(root);
    return result;
}

char *claw_memory_summary_catalog_dup(void)
{
    cJSON *index_root = NULL;
    cJSON *summaries = NULL;
    cJSON *item = NULL;
    char *content = NULL;
    size_t count = 0;
    size_t buf_size = 64;
    size_t off = 0;

    if (claw_memory_load_index(&index_root) != ESP_OK) {
        return dup_printf("- (empty)\n");
    }

    summaries = cJSON_GetObjectItem(index_root, "summaries");
    if (cJSON_IsArray(summaries)) {
        count = (size_t)cJSON_GetArraySize(summaries);
        buf_size += count * 48;
    }
    content = calloc(1, buf_size);
    if (!content) {
        cJSON_Delete(index_root);
        return NULL;
    }

    if (!cJSON_IsArray(summaries) || count == 0) {
        snprintf(content, buf_size, "- (empty)\n");
        cJSON_Delete(index_root);
        return content;
    }

    cJSON_ArrayForEach(item, summaries) {
        const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(item, "label"));

        if (!label) {
            continue;
        }
        off += snprintf(content + off, buf_size - off, "- %s\n", label);
        if (off + 8 >= buf_size) {
            break;
        }
    }

    cJSON_Delete(index_root);
    return content;
}
