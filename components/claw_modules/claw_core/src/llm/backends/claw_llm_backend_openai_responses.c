/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/backends/claw_llm_backend_openai_responses.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llm/claw_llm_http_transport.h"
#include "llm/media/claw_media_pipeline.h"

#define OPENAI_RESPONSES_PROVIDER_STATE_KEY "_claw_provider_state"
#define OPENAI_RESPONSES_PROVIDER_OUTPUT_KEY "output"

typedef struct {
    char *api_key;
    char *model;
    char *base_url;
    char *auth_type;
    char *reasoning_effort;
    uint32_t timeout_ms;
    uint32_t max_tokens;
    size_t image_max_bytes;
} openai_responses_backend_ctx_t;

static char *dup_printf(const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int needed;
    char *buf;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return NULL;
    }
    buf = calloc(1, (size_t)needed + 1U);
    if (!buf) {
        va_end(args);
        return NULL;
    }
    vsnprintf(buf, (size_t)needed + 1U, fmt, args);
    va_end(args);
    return buf;
}

static char *join_url(const char *base_url, const char *path)
{
    bool base_has_slash;
    bool path_has_slash;

    if (!base_url || !path) {
        return NULL;
    }
    base_has_slash = base_url[0] && base_url[strlen(base_url) - 1U] == '/';
    path_has_slash = path[0] == '/';
    if (base_has_slash && path_has_slash) {
        return dup_printf("%s%s", base_url, path + 1);
    }
    if (!base_has_slash && !path_has_slash) {
        return dup_printf("%s/%s", base_url, path);
    }
    return dup_printf("%s%s", base_url, path);
}

static bool reasoning_effort_is_valid(const char *effort)
{
    static const char *const values[] = {
        "none", "low", "medium", "high", "xhigh", "max",
    };
    size_t i;

    if (!effort || !effort[0]) {
        return false;
    }
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (strcmp(effort, values[i]) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t add_duplicate_to_array(cJSON *array, const cJSON *item)
{
    cJSON *copy;

    if (!array || !item || !cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }
    copy = cJSON_Duplicate(item, true);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(array, copy);
    return ESP_OK;
}

static esp_err_t replay_provider_output(const cJSON *message,
                                        cJSON *input,
                                        bool *out_replayed)
{
    cJSON *state;
    cJSON *backend;
    cJSON *output;
    cJSON *item;
    esp_err_t err;

    *out_replayed = false;
    state = cJSON_GetObjectItem(message, OPENAI_RESPONSES_PROVIDER_STATE_KEY);
    backend = state ? cJSON_GetObjectItem(state, "backend") : NULL;
    output = state ? cJSON_GetObjectItem(state, OPENAI_RESPONSES_PROVIDER_OUTPUT_KEY) : NULL;
    if (!cJSON_IsObject(state) || !cJSON_IsString(backend) || !backend->valuestring ||
            strcmp(backend->valuestring, CLAW_LLM_BACKEND_OPENAI_RESPONSES_ID) != 0 ||
            !cJSON_IsArray(output)) {
        return ESP_OK;
    }

    cJSON_ArrayForEach(item, output) {
        err = add_duplicate_to_array(input, item);
        if (err != ESP_OK) {
            return err;
        }
    }
    *out_replayed = true;
    return ESP_OK;
}

static esp_err_t add_function_call_output(const cJSON *message,
                                          cJSON *input,
                                          char **out_error_message)
{
    cJSON *call_id = cJSON_GetObjectItem(message, "tool_call_id");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    cJSON *output_item = NULL;
    char *serialized = NULL;
    const char *output_text = "";

    if (!cJSON_IsString(call_id) || !call_id->valuestring) {
        *out_error_message = dup_printf("Tool result is missing tool_call_id");
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsString(content) && content->valuestring) {
        output_text = content->valuestring;
    } else if (content) {
        serialized = cJSON_PrintUnformatted(content);
        if (!serialized) {
            *out_error_message = dup_printf("Out of memory serializing tool result");
            return ESP_ERR_NO_MEM;
        }
        output_text = serialized;
    }

    output_item = cJSON_CreateObject();
    if (!output_item) {
        free(serialized);
        *out_error_message = dup_printf("Out of memory building tool result");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(output_item, "type", "function_call_output");
    cJSON_AddStringToObject(output_item, "call_id", call_id->valuestring);
    cJSON_AddStringToObject(output_item, "output", output_text);
    cJSON_AddItemToArray(input, output_item);
    free(serialized);
    return ESP_OK;
}

static const char *chat_image_url(const cJSON *block)
{
    cJSON *image_url = cJSON_GetObjectItem(block, "image_url");
    cJSON *url;

    if (cJSON_IsString(image_url)) {
        return image_url->valuestring;
    }
    url = cJSON_IsObject(image_url) ? cJSON_GetObjectItem(image_url, "url") : NULL;
    return cJSON_IsString(url) ? url->valuestring : NULL;
}

static esp_err_t add_message_content_block(const cJSON *src,
                                           cJSON *content,
                                           char **out_error_message)
{
    cJSON *type = cJSON_GetObjectItem(src, "type");
    cJSON *text = cJSON_GetObjectItem(src, "text");
    cJSON *dst;
    const char *image_url;

    if (!cJSON_IsString(type) || !type->valuestring) {
        return ESP_OK;
    }
    if (strcmp(type->valuestring, "text") == 0 ||
            strcmp(type->valuestring, "input_text") == 0 ||
            strcmp(type->valuestring, "output_text") == 0) {
        if (!cJSON_IsString(text) || !text->valuestring) {
            return ESP_OK;
        }
        dst = cJSON_CreateObject();
        if (!dst) {
            *out_error_message = dup_printf("Out of memory converting message text");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(dst, "type", "input_text");
        cJSON_AddStringToObject(dst, "text", text->valuestring);
        cJSON_AddItemToArray(content, dst);
        return ESP_OK;
    }
    if (strcmp(type->valuestring, "image_url") != 0 &&
            strcmp(type->valuestring, "input_image") != 0) {
        return ESP_OK;
    }
    image_url = chat_image_url(src);
    if (!image_url && strcmp(type->valuestring, "input_image") == 0) {
        cJSON *native_url = cJSON_GetObjectItem(src, "image_url");

        image_url = cJSON_IsString(native_url) ? native_url->valuestring : NULL;
    }
    if (!image_url) {
        *out_error_message = dup_printf("Image message is missing image_url");
        return ESP_ERR_INVALID_ARG;
    }
    dst = cJSON_CreateObject();
    if (!dst) {
        *out_error_message = dup_printf("Out of memory converting image message");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(dst, "type", "input_image");
    cJSON_AddStringToObject(dst, "image_url", image_url);
    cJSON_AddItemToArray(content, dst);
    return ESP_OK;
}

static esp_err_t add_normal_message(const cJSON *message,
                                    cJSON *input,
                                    char **out_error_message)
{
    cJSON *role = cJSON_GetObjectItem(message, "role");
    cJSON *source_content = cJSON_GetObjectItem(message, "content");
    cJSON *dst = NULL;
    cJSON *content = NULL;
    cJSON *block;
    esp_err_t err = ESP_OK;

    if (!cJSON_IsString(role) || !role->valuestring) {
        *out_error_message = dup_printf("Message is missing role");
        return ESP_ERR_INVALID_ARG;
    }
    dst = cJSON_CreateObject();
    if (!dst) {
        *out_error_message = dup_printf("Out of memory converting message");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(dst, "role", role->valuestring);
    if (cJSON_IsString(source_content)) {
        cJSON_AddStringToObject(dst, "content", source_content->valuestring ? source_content->valuestring : "");
    } else if (cJSON_IsArray(source_content)) {
        content = cJSON_CreateArray();
        if (!content) {
            err = ESP_ERR_NO_MEM;
            *out_error_message = dup_printf("Out of memory converting message content");
            goto fail;
        }
        cJSON_ArrayForEach(block, source_content) {
            err = add_message_content_block(block, content, out_error_message);
            if (err != ESP_OK) {
                goto fail;
            }
        }
        cJSON_AddItemToObject(dst, "content", content);
        content = NULL;
    } else {
        cJSON_AddStringToObject(dst, "content", "");
    }
    cJSON_AddItemToArray(input, dst);
    return ESP_OK;

fail:
    cJSON_Delete(content);
    cJSON_Delete(dst);
    return err;
}

static esp_err_t add_fallback_chat_tool_calls(const cJSON *message,
                                              cJSON *input,
                                              char **out_error_message)
{
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    cJSON *tool_call;

    if (!cJSON_IsArray(tool_calls)) {
        return ESP_OK;
    }
    cJSON_ArrayForEach(tool_call, tool_calls) {
        cJSON *id = cJSON_GetObjectItem(tool_call, "id");
        cJSON *function = cJSON_GetObjectItem(tool_call, "function");
        cJSON *name = function ? cJSON_GetObjectItem(function, "name") : NULL;
        cJSON *arguments = function ? cJSON_GetObjectItem(function, "arguments") : NULL;
        cJSON *item;

        if (!cJSON_IsString(id) || !cJSON_IsString(name) || !cJSON_IsString(arguments)) {
            *out_error_message = dup_printf("Malformed assistant tool call in history");
            return ESP_ERR_INVALID_ARG;
        }
        item = cJSON_CreateObject();
        if (!item) {
            *out_error_message = dup_printf("Out of memory converting assistant tool call");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "type", "function_call");
        cJSON_AddStringToObject(item, "call_id", id->valuestring);
        cJSON_AddStringToObject(item, "name", name->valuestring);
        cJSON_AddStringToObject(item, "arguments", arguments->valuestring);
        cJSON_AddItemToArray(input, item);
    }
    return ESP_OK;
}

static esp_err_t convert_messages(const cJSON *messages,
                                  cJSON *input,
                                  char **out_error_message)
{
    cJSON *message;

    cJSON_ArrayForEach(message, messages) {
        cJSON *role;
        bool replayed = false;
        esp_err_t err;

        if (!cJSON_IsObject(message)) {
            *out_error_message = dup_printf("Message history contains a non-object item");
            return ESP_ERR_INVALID_ARG;
        }
        err = replay_provider_output(message, input, &replayed);
        if (err != ESP_OK) {
            *out_error_message = dup_printf("Out of memory replaying Responses output");
            return err;
        }
        if (replayed) {
            continue;
        }
        role = cJSON_GetObjectItem(message, "role");
        if (cJSON_IsString(role) && role->valuestring && strcmp(role->valuestring, "tool") == 0) {
            err = add_function_call_output(message, input, out_error_message);
            if (err != ESP_OK) {
                return err;
            }
            continue;
        }
        err = add_normal_message(message, input, out_error_message);
        if (err != ESP_OK) {
            return err;
        }
        err = add_fallback_chat_tool_calls(message, input, out_error_message);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t convert_tools(const char *tools_json,
                               cJSON **out_tools,
                               char **out_error_message)
{
    cJSON *source = NULL;
    cJSON *tools = NULL;
    cJSON *tool;

    *out_tools = NULL;
    source = cJSON_Parse(tools_json);
    tools = cJSON_CreateArray();
    if (!source || !cJSON_IsArray(source)) {
        cJSON_Delete(source);
        cJSON_Delete(tools);
        *out_error_message = dup_printf("Invalid tools JSON");
        return ESP_ERR_INVALID_ARG;
    }
    if (!tools) {
        cJSON_Delete(source);
        *out_error_message = dup_printf("Out of memory converting tools");
        return ESP_ERR_NO_MEM;
    }

    cJSON_ArrayForEach(tool, source) {
        cJSON *function = cJSON_GetObjectItem(tool, "function");
        cJSON *definition = cJSON_IsObject(function) ? function : tool;
        cJSON *name = cJSON_GetObjectItem(definition, "name");
        cJSON *description = cJSON_GetObjectItem(definition, "description");
        cJSON *parameters = cJSON_GetObjectItem(definition, "parameters");
        cJSON *strict = cJSON_GetObjectItem(definition, "strict");
        cJSON *dst;
        cJSON *parameters_copy;

        if (!cJSON_IsString(name) || !name->valuestring || !cJSON_IsObject(parameters)) {
            cJSON_Delete(source);
            cJSON_Delete(tools);
            *out_error_message = dup_printf("Malformed function tool definition");
            return ESP_ERR_INVALID_ARG;
        }
        dst = cJSON_CreateObject();
        parameters_copy = cJSON_Duplicate(parameters, true);
        if (!dst || !parameters_copy) {
            cJSON_Delete(dst);
            cJSON_Delete(parameters_copy);
            cJSON_Delete(source);
            cJSON_Delete(tools);
            *out_error_message = dup_printf("Out of memory converting tools");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(dst, "type", "function");
        cJSON_AddStringToObject(dst, "name", name->valuestring);
        if (cJSON_IsString(description) && description->valuestring) {
            cJSON_AddStringToObject(dst, "description", description->valuestring);
        }
        cJSON_AddItemToObject(dst, "parameters", parameters_copy);
        if (cJSON_IsBool(strict)) {
            cJSON_AddBoolToObject(dst, "strict", cJSON_IsTrue(strict));
        }
        cJSON_AddItemToArray(tools, dst);
    }
    cJSON_Delete(source);
    *out_tools = tools;
    return ESP_OK;
}

static esp_err_t build_responses_body(const openai_responses_backend_ctx_t *ctx,
                                      const claw_llm_model_profile_t *profile,
                                      const claw_llm_chat_request_t *request,
                                      char **out_post_data,
                                      char **out_error_message)
{
    cJSON *body = NULL;
    cJSON *input = NULL;
    cJSON *reasoning = NULL;
    cJSON *include = NULL;
    cJSON *tools = NULL;
    char *post_data = NULL;
    esp_err_t err;

    body = cJSON_CreateObject();
    input = cJSON_CreateArray();
    reasoning = cJSON_CreateObject();
    include = cJSON_CreateArray();
    if (!body || !input || !reasoning || !include) {
        *out_error_message = dup_printf("Out of memory building Responses request");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    cJSON_AddStringToObject(body, "model", ctx->model);
    cJSON_AddStringToObject(body, "instructions", request->system_prompt);
    cJSON_AddNumberToObject(body,
                            CLAW_LLM_BACKEND_OPENAI_RESPONSES_MAX_TOKENS_FIELD,
                            ctx->max_tokens);
    cJSON_AddBoolToObject(body, "store", false);
    cJSON_AddStringToObject(reasoning, "effort", ctx->reasoning_effort);
    cJSON_AddStringToObject(reasoning, "context", "current_turn");
    cJSON_AddItemToObject(body, "reasoning", reasoning);
    reasoning = NULL;
    cJSON_AddItemToArray(include, cJSON_CreateString("reasoning.encrypted_content"));
    cJSON_AddItemToObject(body, "include", include);
    include = NULL;

    err = convert_messages(request->messages, input, out_error_message);
    if (err != ESP_OK) {
        goto cleanup;
    }
    cJSON_AddItemToObject(body, "input", input);
    input = NULL;

    if (request->tools_json && request->tools_json[0]) {
        if (!profile->supports_tools) {
            *out_error_message = dup_printf("Selected backend does not support tool calls");
            err = ESP_ERR_NOT_SUPPORTED;
            goto cleanup;
        }
        err = convert_tools(request->tools_json, &tools, out_error_message);
        if (err != ESP_OK) {
            goto cleanup;
        }
        cJSON_AddItemToObject(body, "tools", tools);
        tools = NULL;
        cJSON_AddBoolToObject(body, "parallel_tool_calls", true);
    }

    post_data = cJSON_PrintUnformatted(body);
    if (!post_data) {
        *out_error_message = dup_printf("Out of memory serializing Responses request");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    *out_post_data = post_data;
    err = ESP_OK;

cleanup:
    cJSON_Delete(body);
    cJSON_Delete(input);
    cJSON_Delete(reasoning);
    cJSON_Delete(include);
    cJSON_Delete(tools);
    return err;
}

static esp_err_t append_text(char **buffer, size_t *length, const char *text)
{
    size_t add_len;
    char *next;

    if (!text || !text[0]) {
        return ESP_OK;
    }
    add_len = strlen(text);
    next = realloc(*buffer, *length + add_len + 1U);
    if (!next) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(next + *length, text, add_len + 1U);
    *buffer = next;
    *length += add_len;
    return ESP_OK;
}

static esp_err_t collect_output_text(const cJSON *output, char **out_text)
{
    cJSON *item;
    size_t length = 0;
    esp_err_t err;

    cJSON_ArrayForEach(item, output) {
        cJSON *type = cJSON_GetObjectItem(item, "type");
        cJSON *content;
        cJSON *block;

        if (!cJSON_IsString(type) || !type->valuestring || strcmp(type->valuestring, "message") != 0) {
            continue;
        }
        content = cJSON_GetObjectItem(item, "content");
        if (!cJSON_IsArray(content)) {
            continue;
        }
        cJSON_ArrayForEach(block, content) {
            cJSON *block_type = cJSON_GetObjectItem(block, "type");
            cJSON *value = NULL;

            if (!cJSON_IsString(block_type) || !block_type->valuestring) {
                continue;
            }
            if (strcmp(block_type->valuestring, "output_text") == 0) {
                value = cJSON_GetObjectItem(block, "text");
            } else if (strcmp(block_type->valuestring, "refusal") == 0) {
                value = cJSON_GetObjectItem(block, "refusal");
            }
            if (cJSON_IsString(value) && value->valuestring) {
                err = append_text(out_text, &length, value->valuestring);
                if (err != ESP_OK) {
                    return err;
                }
            }
        }
    }
    return ESP_OK;
}

static size_t count_function_calls(const cJSON *output)
{
    cJSON *item;
    size_t count = 0;

    cJSON_ArrayForEach(item, output) {
        cJSON *type = cJSON_GetObjectItem(item, "type");

        if (cJSON_IsString(type) && type->valuestring && strcmp(type->valuestring, "function_call") == 0) {
            count++;
        }
    }
    return count;
}

static esp_err_t copy_function_calls(const cJSON *output,
                                     claw_llm_response_t *response,
                                     cJSON *raw_tool_calls,
                                     char **out_error_message)
{
    cJSON *item;
    size_t index = 0;

    cJSON_ArrayForEach(item, output) {
        cJSON *type = cJSON_GetObjectItem(item, "type");
        cJSON *call_id;
        cJSON *name;
        cJSON *arguments;
        claw_llm_tool_call_t *dst;
        cJSON *raw_call;
        cJSON *raw_function;

        if (!cJSON_IsString(type) || !type->valuestring || strcmp(type->valuestring, "function_call") != 0) {
            continue;
        }
        call_id = cJSON_GetObjectItem(item, "call_id");
        name = cJSON_GetObjectItem(item, "name");
        arguments = cJSON_GetObjectItem(item, "arguments");
        if (!cJSON_IsString(call_id) || !call_id->valuestring ||
                !cJSON_IsString(name) || !name->valuestring ||
                !cJSON_IsString(arguments) || !arguments->valuestring) {
            *out_error_message = dup_printf("Malformed function_call in Responses output");
            return ESP_FAIL;
        }
        dst = &response->tool_calls[index++];
        dst->id = strdup(call_id->valuestring);
        dst->name = strdup(name->valuestring);
        dst->arguments_json = strdup(arguments->valuestring);
        raw_call = cJSON_CreateObject();
        raw_function = cJSON_CreateObject();
        if (!dst->id || !dst->name || !dst->arguments_json || !raw_call || !raw_function) {
            cJSON_Delete(raw_call);
            cJSON_Delete(raw_function);
            *out_error_message = dup_printf("Out of memory copying Responses function call");
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(raw_call, "id", call_id->valuestring);
        cJSON_AddStringToObject(raw_call, "type", "function");
        cJSON_AddStringToObject(raw_function, "name", name->valuestring);
        cJSON_AddStringToObject(raw_function, "arguments", arguments->valuestring);
        cJSON_AddItemToObject(raw_call, "function", raw_function);
        cJSON_AddItemToArray(raw_tool_calls, raw_call);
    }
    return ESP_OK;
}

static char *api_error_message(const cJSON *root)
{
    cJSON *error = cJSON_GetObjectItem(root, "error");
    cJSON *message = error ? cJSON_GetObjectItem(error, "message") : NULL;
    cJSON *status = cJSON_GetObjectItem(root, "status");
    cJSON *incomplete = cJSON_GetObjectItem(root, "incomplete_details");
    cJSON *reason = incomplete ? cJSON_GetObjectItem(incomplete, "reason") : NULL;

    if (cJSON_IsString(message) && message->valuestring) {
        return dup_printf("OpenAI Responses API error: %s", message->valuestring);
    }
    if (cJSON_IsString(status) && status->valuestring && strcmp(status->valuestring, "failed") == 0) {
        return dup_printf("OpenAI Responses API returned failed status");
    }
    if (cJSON_IsString(status) && status->valuestring && strcmp(status->valuestring, "incomplete") == 0 &&
            cJSON_IsString(reason) && reason->valuestring) {
        return dup_printf("OpenAI Responses API response incomplete: %s", reason->valuestring);
    }
    return NULL;
}

static esp_err_t parse_responses_response(const char *body,
                                          claw_llm_response_t *out_response,
                                          char **out_error_message)
{
    cJSON *root = NULL;
    cJSON *output;
    cJSON *raw_message = NULL;
    cJSON *raw_tool_calls = NULL;
    cJSON *provider_state = NULL;
    cJSON *output_copy = NULL;
    char *api_error = NULL;
    size_t tool_count;
    esp_err_t err;

    memset(out_response, 0, sizeof(*out_response));
    root = cJSON_Parse(body);
    if (!root) {
        *out_error_message = dup_printf("Failed to parse OpenAI Responses JSON");
        return ESP_FAIL;
    }
    api_error = api_error_message(root);
    if (api_error) {
        cJSON_Delete(root);
        *out_error_message = api_error;
        return ESP_FAIL;
    }
    output = cJSON_GetObjectItem(root, "output");
    if (!cJSON_IsArray(output)) {
        cJSON_Delete(root);
        *out_error_message = dup_printf("OpenAI Responses result is missing output");
        return ESP_FAIL;
    }

    err = collect_output_text(output, &out_response->text);
    if (err != ESP_OK) {
        *out_error_message = dup_printf("Out of memory copying Responses text");
        goto fail;
    }
    tool_count = count_function_calls(output);
    if (tool_count > 0) {
        out_response->tool_calls = calloc(tool_count, sizeof(out_response->tool_calls[0]));
        if (!out_response->tool_calls) {
            *out_error_message = dup_printf("Out of memory copying Responses tool calls");
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        out_response->tool_call_count = tool_count;
    }

    raw_message = cJSON_CreateObject();
    raw_tool_calls = cJSON_CreateArray();
    if (!raw_message || !raw_tool_calls) {
        *out_error_message = dup_printf("Out of memory building canonical Responses message");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    cJSON_AddStringToObject(raw_message, "role", "assistant");
    if (out_response->text) {
        cJSON_AddStringToObject(raw_message, "content", out_response->text);
    } else {
        cJSON_AddNullToObject(raw_message, "content");
    }
    err = copy_function_calls(output, out_response, raw_tool_calls, out_error_message);
    if (err != ESP_OK) {
        goto fail;
    }
    if (tool_count > 0) {
        cJSON_AddItemToObject(raw_message, "tool_calls", raw_tool_calls);
        raw_tool_calls = NULL;
        provider_state = cJSON_CreateObject();
        output_copy = cJSON_Duplicate(output, true);
        if (!provider_state || !output_copy) {
            *out_error_message = dup_printf("Out of memory preserving Responses provider state");
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        cJSON_AddStringToObject(provider_state, "backend", CLAW_LLM_BACKEND_OPENAI_RESPONSES_ID);
        cJSON_AddItemToObject(provider_state, OPENAI_RESPONSES_PROVIDER_OUTPUT_KEY, output_copy);
        output_copy = NULL;
        cJSON_AddItemToObject(raw_message, OPENAI_RESPONSES_PROVIDER_STATE_KEY, provider_state);
        provider_state = NULL;
    }
    out_response->raw_message_json = cJSON_PrintUnformatted(raw_message);
    if (!out_response->raw_message_json) {
        *out_error_message = dup_printf("Out of memory serializing canonical Responses message");
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (!out_response->text && tool_count == 0) {
        *out_error_message = dup_printf("OpenAI Responses API returned no text or function call");
        err = ESP_FAIL;
        goto fail;
    }
    err = ESP_OK;
    goto cleanup;

fail:
    claw_llm_response_free(out_response);
cleanup:
    cJSON_Delete(root);
    cJSON_Delete(raw_message);
    cJSON_Delete(raw_tool_calls);
    cJSON_Delete(provider_state);
    cJSON_Delete(output_copy);
    return err;
}

static esp_err_t post_responses(openai_responses_backend_ctx_t *ctx,
                                const claw_llm_model_profile_t *profile,
                                const char *post_data,
                                volatile bool *abort_flag,
                                claw_llm_http_response_t *out_http_response,
                                char **out_error_message)
{
    claw_llm_http_json_request_t request = {0};
    char *url;
    esp_err_t err;

    url = join_url(ctx->base_url, profile->chat_path);
    if (!url) {
        *out_error_message = dup_printf("Out of memory building Responses API URL");
        return ESP_ERR_NO_MEM;
    }
    request.url = url;
    request.body = post_data;
    request.api_key = ctx->api_key;
    request.auth_type = ctx->auth_type;
    request.timeout_ms = ctx->timeout_ms;
    request.abort_flag = abort_flag;
    err = claw_llm_http_post_json(&request, out_http_response, out_error_message);
    free(url);
    return err;
}

static esp_err_t openai_responses_init(const claw_llm_runtime_config_t *config,
                                       const claw_llm_model_profile_t *profile,
                                       void **out_backend_ctx,
                                       char **out_error_message)
{
    openai_responses_backend_ctx_t *ctx;
    const char *effort;

    if (!config || !profile || !out_backend_ctx || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->api_key || !config->api_key[0]) {
        *out_error_message = dup_printf("OpenAI API key is empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->model || !config->model[0] || !config->base_url || !config->base_url[0]) {
        *out_error_message = dup_printf("OpenAI Responses model or base_url is empty");
        return ESP_ERR_INVALID_ARG;
    }
    effort = (config->reasoning_effort && config->reasoning_effort[0]) ?
             config->reasoning_effort : CLAW_LLM_BACKEND_OPENAI_RESPONSES_DEFAULT_REASONING_EFFORT;
    if (!reasoning_effort_is_valid(effort)) {
        *out_error_message = dup_printf("Invalid reasoning_effort: %s", effort);
        return ESP_ERR_INVALID_ARG;
    }

    ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        *out_error_message = dup_printf("Out of memory allocating Responses backend");
        return ESP_ERR_NO_MEM;
    }
    ctx->api_key = strdup(config->api_key);
    ctx->model = strdup(config->model);
    ctx->base_url = strdup(config->base_url);
    ctx->auth_type = strdup((config->auth_type && config->auth_type[0]) ? config->auth_type : "bearer");
    ctx->reasoning_effort = strdup(effort);
    ctx->timeout_ms = config->timeout_ms;
    ctx->max_tokens = config->max_tokens;
    ctx->image_max_bytes = config->image_max_bytes;
    if (!ctx->api_key || !ctx->model || !ctx->base_url || !ctx->auth_type || !ctx->reasoning_effort) {
        *out_error_message = dup_printf("Out of memory copying Responses backend config");
        free(ctx->api_key);
        free(ctx->model);
        free(ctx->base_url);
        free(ctx->auth_type);
        free(ctx->reasoning_effort);
        free(ctx);
        return ESP_ERR_NO_MEM;
    }
    *out_backend_ctx = ctx;
    return ESP_OK;
}

static esp_err_t openai_responses_chat(void *backend_ctx,
                                       const claw_llm_model_profile_t *profile,
                                       const claw_llm_chat_request_t *request,
                                       claw_llm_response_t *out_response,
                                       char **out_error_message)
{
    openai_responses_backend_ctx_t *ctx = backend_ctx;
    claw_llm_http_response_t http_response = {0};
    char *post_data = NULL;
    esp_err_t err;

    if (!ctx || !profile || !request || !request->system_prompt || !cJSON_IsArray(request->messages) ||
            !out_response || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_error_message = NULL;
    err = build_responses_body(ctx, profile, request, &post_data, out_error_message);
    if (err != ESP_OK) {
        return err;
    }
    err = post_responses(ctx, profile, post_data, request->abort_flag, &http_response, out_error_message);
    free(post_data);
    if (err != ESP_OK) {
        return err;
    }
    err = parse_responses_response(http_response.body, out_response, out_error_message);
    claw_llm_http_response_free(&http_response);
    return err;
}

static esp_err_t add_media_message(cJSON *messages,
                                   const claw_llm_media_request_t *request,
                                   const claw_llm_model_profile_t *profile,
                                   openai_responses_backend_ctx_t *ctx,
                                   char **out_error_message)
{
    cJSON *message = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *text_block = cJSON_CreateObject();
    esp_err_t err = ESP_OK;
    size_t i;

    if (!message || !content || !text_block) {
        *out_error_message = dup_printf("Out of memory building Responses media input");
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    cJSON_AddStringToObject(message, "role", "user");
    cJSON_AddStringToObject(text_block, "type", "text");
    cJSON_AddStringToObject(text_block, "text", request->user_prompt);
    cJSON_AddItemToArray(content, text_block);
    text_block = NULL;

    for (i = 0; i < request->media_count; i++) {
        claw_media_prepared_t prepared = {0};
        cJSON *image_block;
        cJSON *image_value;

        err = claw_media_prepare_asset(&request->media[i], profile, ctx->image_max_bytes,
                                       &prepared, out_error_message);
        if (err != ESP_OK) {
            goto cleanup;
        }
        image_block = cJSON_CreateObject();
        image_value = cJSON_CreateObject();
        if (!image_block || !image_value) {
            cJSON_Delete(image_block);
            cJSON_Delete(image_value);
            claw_media_prepared_free(&prepared);
            *out_error_message = dup_printf("Out of memory building Responses image input");
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddStringToObject(image_block, "type", "image_url");
        cJSON_AddStringToObject(image_value, "url", prepared.payload);
        cJSON_AddItemToObject(image_block, "image_url", image_value);
        cJSON_AddItemToArray(content, image_block);
        claw_media_prepared_free(&prepared);
    }
    cJSON_AddItemToObject(message, "content", content);
    content = NULL;
    cJSON_AddItemToArray(messages, message);
    message = NULL;

cleanup:
    cJSON_Delete(message);
    cJSON_Delete(content);
    cJSON_Delete(text_block);
    return err;
}

static esp_err_t openai_responses_infer_media(void *backend_ctx,
                                              const claw_llm_model_profile_t *profile,
                                              const claw_llm_media_request_t *request,
                                              char **out_text,
                                              char **out_error_message)
{
    openai_responses_backend_ctx_t *ctx = backend_ctx;
    claw_llm_chat_request_t chat_request = {0};
    claw_llm_response_t response = {0};
    cJSON *messages = NULL;
    esp_err_t err;

    if (out_text) {
        *out_text = NULL;
    }
    if (out_error_message) {
        *out_error_message = NULL;
    }
    if (!ctx || !profile || !request || !out_text || !out_error_message) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!profile->supports_vision) {
        *out_error_message = dup_printf("Selected profile does not support media inference");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!request->user_prompt || !request->user_prompt[0] || !request->media || request->media_count == 0) {
        *out_error_message = dup_printf("Media request is incomplete");
        return ESP_ERR_INVALID_ARG;
    }
    messages = cJSON_CreateArray();
    if (!messages) {
        *out_error_message = dup_printf("Out of memory building media request");
        return ESP_ERR_NO_MEM;
    }
    err = add_media_message(messages, request, profile, ctx, out_error_message);
    if (err != ESP_OK) {
        cJSON_Delete(messages);
        return err;
    }
    chat_request.system_prompt = request->system_prompt ? request->system_prompt : "";
    chat_request.messages = messages;
    err = openai_responses_chat(ctx, profile, &chat_request, &response, out_error_message);
    cJSON_Delete(messages);
    if (err != ESP_OK) {
        return err;
    }
    if (!response.text || !response.text[0] || response.tool_call_count > 0) {
        claw_llm_response_free(&response);
        *out_error_message = dup_printf("OpenAI Responses returned no final media text");
        return ESP_FAIL;
    }
    *out_text = response.text;
    response.text = NULL;
    claw_llm_response_free(&response);
    return ESP_OK;
}

static void openai_responses_deinit(void *backend_ctx)
{
    openai_responses_backend_ctx_t *ctx = backend_ctx;

    if (!ctx) {
        return;
    }
    free(ctx->api_key);
    free(ctx->model);
    free(ctx->base_url);
    free(ctx->auth_type);
    free(ctx->reasoning_effort);
    free(ctx);
}

static const claw_llm_backend_vtable_t s_openai_responses_vtable = {
    .id = CLAW_LLM_BACKEND_OPENAI_RESPONSES_ID,
    .init = openai_responses_init,
    .chat = openai_responses_chat,
    .infer_media = openai_responses_infer_media,
    .deinit = openai_responses_deinit,
};

static const claw_llm_backend_registration_t s_openai_responses_registration = {
    .id = CLAW_LLM_BACKEND_OPENAI_RESPONSES_ID,
    .vtable = &s_openai_responses_vtable,
    .defaults = {
        .auth_type = CLAW_LLM_BACKEND_OPENAI_RESPONSES_AUTH_TYPE,
        .chat_path = CLAW_LLM_BACKEND_OPENAI_RESPONSES_CHAT_PATH,
        .max_tokens_field = CLAW_LLM_BACKEND_OPENAI_RESPONSES_MAX_TOKENS_FIELD,
    },
};

const claw_llm_backend_registration_t *claw_llm_backend_openai_responses_registration(void)
{
    return &s_openai_responses_registration;
}
