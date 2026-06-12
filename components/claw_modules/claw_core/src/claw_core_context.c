/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_core_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "claw_core";

void claw_core_free_cached_contexts(claw_core_cached_context_t *contexts, size_t count)
{
    size_t i;

    if (!contexts) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(contexts[i].content);
        contexts[i].content = NULL;
        contexts[i].valid = false;
    }
    free(contexts);
}

esp_err_t claw_core_append_user_message(cJSON *messages, const char *text)
{
    cJSON *user_msg = NULL;

    if (!messages || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    user_msg = cJSON_CreateObject();
    if (!user_msg) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", text);
    cJSON_AddItemToArray(messages, user_msg);
    return ESP_OK;
}

static esp_err_t append_message_array_json(cJSON *messages, const char *json_text)
{
    cJSON *parsed = NULL;
    cJSON *item = NULL;

    if (!messages || !json_text || !json_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    parsed = cJSON_Parse(json_text);
    if (!parsed || !cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(item, parsed) {
        cJSON *dup = cJSON_Duplicate(item, true);

        if (!dup) {
            cJSON_Delete(parsed);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, dup);
    }

    cJSON_Delete(parsed);
    return ESP_OK;
}

static esp_err_t append_message_array(cJSON *messages, const cJSON *items)
{
    const cJSON *item = NULL;

    if (!messages || !items || !cJSON_IsArray((cJSON *)items)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON_ArrayForEach(item, items) {
        cJSON *dup = cJSON_Duplicate((cJSON *)item, true);

        if (!dup) {
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(messages, dup);
    }

    return ESP_OK;
}

static esp_err_t append_tool_array_json(cJSON *tools, const char *json_text)
{
    cJSON *parsed = NULL;
    cJSON *item = NULL;

    if (!tools || !json_text || !json_text[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    parsed = cJSON_Parse(json_text);
    if (!parsed || !cJSON_IsArray(parsed)) {
        cJSON_Delete(parsed);
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(item, parsed) {
        cJSON *dup = cJSON_Duplicate(item, true);

        if (!dup) {
            cJSON_Delete(parsed);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(tools, dup);
    }

    cJSON_Delete(parsed);
    return ESP_OK;
}

static char *build_current_turn_prompt(const claw_core_request_t *request)
{
#define CLAW_CORE_TURN_PROMPT_FMT \
    "## Current Turn Context\n" \
    "- source_channel: %s\n" \
    "- source_chat_id: %s\n"
#define CLAW_CORE_TURN_PROMPT_ARGS(req) \
    (req)->source_channel ? (req)->source_channel : "(unknown)", \
    (req)->source_chat_id ? (req)->source_chat_id : "(unknown)"

    int needed;
    char *text = NULL;

    if (!request) {
        text = NULL;
        goto cleanup;
    }

    needed = snprintf(NULL, 0, CLAW_CORE_TURN_PROMPT_FMT, CLAW_CORE_TURN_PROMPT_ARGS(request));
    if (needed < 0) {
        ESP_LOGE(TAG, "failed to size current turn prompt");
        text = NULL;
        goto cleanup;
    }

    text = calloc(1, (size_t)needed + 1);
    if (!text) {
        goto cleanup;
    }

    if (snprintf(text, (size_t)needed + 1, CLAW_CORE_TURN_PROMPT_FMT, CLAW_CORE_TURN_PROMPT_ARGS(request)) < 0) {
        ESP_LOGE(TAG, "failed to build current turn prompt");
        free(text);
        text = NULL;
    }

cleanup:
#undef CLAW_CORE_TURN_PROMPT_ARGS
#undef CLAW_CORE_TURN_PROMPT_FMT
    return text;
}

static esp_err_t append_prompt_section(char **prompt,
                                       const char *section_name,
                                       const char *content)
{
    char *grown = NULL;
    size_t current_len;
    size_t extra_len;

    if (!prompt || !*prompt || !section_name || !content || !content[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    current_len = strlen(*prompt);
    extra_len = strlen("\n\n## \n") + strlen(section_name) + strlen(content);
    grown = realloc(*prompt, current_len + extra_len + 1);
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }

    *prompt = grown;
    snprintf((*prompt) + current_len,
             extra_len + 1,
             "\n\n## %s\n%s",
             section_name,
             content);
    return ESP_OK;
}

esp_err_t claw_core_append_assistant_tool_calls(cJSON *messages,
                                                const claw_core_llm_response_t *response)
{
    cJSON *assistant = NULL;

    if (!messages || !response) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!response->raw_message_json || !response->raw_message_json[0]) {
        return ESP_ERR_INVALID_STATE;
    }

    assistant = cJSON_ParseWithOpts(response->raw_message_json, NULL, 1);
    if (!assistant) {
        cJSON_Delete(assistant);
        return ESP_ERR_INVALID_STATE;
    }
    cJSON_AddItemToArray(messages, assistant);
    return ESP_OK;
}

void claw_core_finish_from_plain_text(uint32_t request_id,
                                      const claw_core_llm_response_t *llm_response,
                                      claw_core_response_t *response)
{
    const char *text = (llm_response && llm_response->text) ? llm_response->text : "";

    response->completion_type = CLAW_CORE_COMPLETION_DONE;
    free(response->text);
    response->text = claw_utils_string_dup(text);
    free(response->error_message);
    response->error_message = NULL;

    ESP_LOGI(TAG, "completion request=%" PRIu32 " status=done raw=%.*s%s",
             request_id,
             claw_core_log_snippet_len(text),
             claw_core_log_snippet(text),
             claw_core_log_snippet_suffix(text));
}

esp_err_t claw_core_append_tool_results_messages(claw_core_state_t *core,
                                                 cJSON *runtime_messages,
                                                 const claw_core_llm_response_t *response,
                                                 const claw_core_request_t *request,
                                                 char *tool_summary,
                                                 size_t tool_summary_size,
                                                 char **out_tool_results_json)
{
    cJSON *tool_results = NULL;
    char *tool_results_json = NULL;
    size_t i;
    esp_err_t ret = ESP_OK;

    if (!core || !runtime_messages || !response || !request || !out_tool_results_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!core->call_cap) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_tool_results_json = NULL;

    tool_results = cJSON_CreateArray();
    if (!tool_results) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < response->tool_call_count; i++) {
        char *tool_output = NULL;
        cJSON *tool_message = NULL;
        cJSON *runtime_copy = NULL;
        esp_err_t err;

        ESP_LOGI(TAG, "tool_call request=%" PRIu32 " name=%s args=%.*s%s",
                 request->request_id,
                 response->tool_calls[i].name ? response->tool_calls[i].name : "(null)",
                 claw_core_log_snippet_len(response->tool_calls[i].arguments_json),
                 claw_core_log_snippet(response->tool_calls[i].arguments_json),
                 claw_core_log_snippet_suffix(response->tool_calls[i].arguments_json));

        err = core->call_cap(response->tool_calls[i].name,
                             response->tool_calls[i].arguments_json,
                             request,
                             &tool_output,
                             core->cap_user_ctx);
        if (err != ESP_OK && !tool_output) {
            tool_output = claw_utils_string_dup(esp_err_to_name(err));
        }
        if (!tool_output) {
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        ESP_LOGI(TAG, "tool_result request=%" PRIu32 " name=%s err=%s output=%.*s%s",
                 request->request_id,
                 response->tool_calls[i].name ? response->tool_calls[i].name : "(null)",
                 esp_err_to_name(err),
                 claw_core_log_snippet_len(tool_output),
                 claw_core_log_snippet(tool_output),
                 claw_core_log_snippet_suffix(tool_output));

        if (tool_summary && tool_summary_size > 0 && response->tool_calls[i].name) {
            esp_err_t summary_err = claw_core_append_tool_summary_line(tool_summary,
                                                                       tool_summary_size,
                                                                       response->tool_calls[i].name,
                                                                       err == ESP_OK);
            if (summary_err != ESP_OK) {
                ESP_LOGW(TAG, "tool summary truncated for request=%" PRIu32, request->request_id);
            }
        }

        tool_message = cJSON_CreateObject();
        if (!tool_message) {
            free(tool_output);
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }

        cJSON_AddStringToObject(tool_message, "role", "tool");
        cJSON_AddStringToObject(tool_message, "tool_call_id", response->tool_calls[i].id);
        cJSON_AddStringToObject(tool_message, "content", tool_output);
        cJSON_AddBoolToObject(tool_message, "is_error", err != ESP_OK);
        free(tool_output);

        runtime_copy = cJSON_Duplicate(tool_message, true);
        if (!runtime_copy) {
            cJSON_Delete(tool_message);
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddItemToArray(runtime_messages, runtime_copy);
        cJSON_AddItemToArray(tool_results, tool_message);
    }

    tool_results_json = cJSON_PrintUnformatted(tool_results);
    if (!tool_results_json) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_tool_results_json = tool_results_json;
    tool_results_json = NULL;

cleanup:
    if (tool_results_json) {
        cJSON_free(tool_results_json);
    }
    cJSON_Delete(tool_results);
    return ret;
}

static esp_err_t apply_context_content(char **system_prompt,
                                       cJSON *messages,
                                       cJSON *tools,
                                       claw_core_context_kind_t kind,
                                       const char *section_name,
                                       const char *content)
{
    if (!system_prompt || !*system_prompt || !messages || !tools ||
            !section_name || !content || !content[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (kind) {
    case CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT:
        return append_prompt_section(system_prompt, section_name, content);
    case CLAW_CORE_CONTEXT_KIND_MESSAGES:
        return append_message_array_json(messages, content);
    case CLAW_CORE_CONTEXT_KIND_TOOLS:
        return append_tool_array_json(tools, content);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t claw_core_collect_request_start_only_contexts(
    claw_core_state_t *core,
    const claw_core_request_item_t *request,
    claw_core_cached_context_t **out_contexts,
    size_t *out_count)
{
    claw_core_cached_context_t *contexts = NULL;
    size_t i;
    esp_err_t err = ESP_OK;

    if (!core || !request || !out_contexts || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_contexts = NULL;
    *out_count = 0;

    if (core->context_provider_count == 0) {
        return ESP_OK;
    }

    contexts = calloc(core->context_provider_count, sizeof(*contexts));
    if (!contexts) {
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < core->context_provider_count; i++) {
        claw_core_context_t context = {0};
        const claw_core_context_provider_t *provider = &core->context_providers[i];
        size_t context_len;

        if (!(provider->flags & CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY)) {
            continue;
        }

        err = provider->collect(&request->view, &context, provider->user_ctx);
        if (err == ESP_ERR_NOT_FOUND) {
            err = ESP_OK;
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "context provider collect failed request=%" PRIu32
                     " provider=%s err=%s",
                     request->view.request_id,
                     provider->name,
                     esp_err_to_name(err));
            goto cleanup;
        }
        if (!context.content || !context.content[0]) {
            ESP_LOGW(TAG,
                     "context provider returned empty content request=%" PRIu32
                     " provider=%s",
                     request->view.request_id,
                     provider->name);
            free(context.content);
            err = ESP_FAIL;
            goto cleanup;
        }

        context_len = strlen(context.content);
        ESP_LOGI(TAG,
                 "context_cached request=%" PRIu32 " provider=%s context_kind=%s context_len=%u",
                 request->view.request_id,
                 provider->name,
                 claw_core_context_kind_to_string(context.kind),
                 (unsigned)context_len);

        contexts[i].valid = true;
        contexts[i].kind = context.kind;
        contexts[i].content = context.content;
        context.content = NULL;
    }

    *out_contexts = contexts;
    *out_count = core->context_provider_count;
    contexts = NULL;

cleanup:
    claw_core_free_cached_contexts(contexts, core->context_provider_count);
    return err;
}

bool claw_core_cached_contexts_have_messages(const claw_core_cached_context_t *contexts,
                                             size_t count)
{
    size_t i;

    if (!contexts) {
        return false;
    }

    for (i = 0; i < count; i++) {
        if (contexts[i].valid && contexts[i].kind == CLAW_CORE_CONTEXT_KIND_MESSAGES) {
            return true;
        }
    }
    return false;
}

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
                                            size_t obs_providers_csv_size)
{
    char *system_prompt = NULL;
    char *turn_prompt = NULL;
    cJSON *messages = NULL;
    cJSON *tools = NULL;
    size_t i;
    esp_err_t err = ESP_OK;

    if (!core || !request || !out_system_prompt || !out_messages || !out_tools_json) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_system_prompt = NULL;
    *out_messages = NULL;
    *out_tools_json = NULL;

    system_prompt = claw_utils_string_dup(core->system_prompt);
    messages = cJSON_CreateArray();
    tools = cJSON_CreateArray();
    if (!system_prompt || !messages || !tools) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    for (i = 0; i < core->context_provider_count; i++) {
        claw_core_context_t context = {0};
        const claw_core_context_provider_t *provider = &core->context_providers[i];
        size_t context_len;

        if (provider->flags & CLAW_CORE_CONTEXT_PROVIDER_FLAG_REQUEST_START_ONLY) {
            if (i < request_start_context_count && request_start_contexts &&
                    request_start_contexts[i].valid) {
                err = apply_context_content(&system_prompt,
                                            messages,
                                            tools,
                                            request_start_contexts[i].kind,
                                            provider->name,
                                            request_start_contexts[i].content);
                if (err != ESP_OK) {
                    goto cleanup;
                }
                claw_core_obs_csv_append(obs_providers_csv, obs_providers_csv_size, provider->name, true);
            }
            continue;
        }

        err = provider->collect(&request->view, &context, provider->user_ctx);
        if (err == ESP_ERR_NOT_FOUND) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG,
                     "context provider collect failed request=%" PRIu32
                     " provider=%s err=%s",
                     request->view.request_id,
                     provider->name,
                     esp_err_to_name(err));
            goto cleanup;
        }
        if (!context.content || !context.content[0]) {
            ESP_LOGW(TAG,
                     "context provider returned empty content request=%" PRIu32
                     " provider=%s",
                     request->view.request_id,
                     provider->name);
            free(context.content);
            err = ESP_FAIL;
            goto cleanup;
        }
        context_len = strlen(context.content);
        ESP_LOGI(TAG,
                 "context_loaded request=%" PRIu32 " provider=%s context_kind=%s context_len=%u",
                 request->view.request_id,
                 provider->name,
                 claw_core_context_kind_to_string(context.kind),
                 (unsigned)context_len);
        claw_core_obs_csv_append(obs_providers_csv, obs_providers_csv_size, provider->name, true);

        err = apply_context_content(&system_prompt,
                                    messages,
                                    tools,
                                    context.kind,
                                    provider->name,
                                    context.content);
        free(context.content);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    turn_prompt = build_current_turn_prompt(&request->view);
    if (!turn_prompt) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = append_prompt_section(&system_prompt, "Core Request", turn_prompt);
    free(turn_prompt);
    turn_prompt = NULL;
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (inject_active_user) {
        err = claw_core_append_user_message(messages, request->view.user_text);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    if (runtime_messages && cJSON_GetArraySize((cJSON *)runtime_messages) > 0) {
        err = append_message_array(messages, runtime_messages);
        if (err != ESP_OK) {
            goto cleanup;
        }
    }

    *out_tools_json = cJSON_GetArraySize(tools) > 0 ? cJSON_PrintUnformatted(tools) : NULL;
    if (cJSON_GetArraySize(tools) > 0 && !*out_tools_json) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    *out_system_prompt = system_prompt;
    *out_messages = messages;
    system_prompt = NULL;
    messages = NULL;
    err = ESP_OK;

cleanup:
    free(turn_prompt);
    free(system_prompt);
    cJSON_Delete(messages);
    cJSON_Delete(tools);
    if (err != ESP_OK) {
        free(*out_tools_json);
        *out_tools_json = NULL;
    }
    return err;
}
