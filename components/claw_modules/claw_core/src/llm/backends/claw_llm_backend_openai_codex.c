/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "llm/backends/claw_llm_backend_openai_codex.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "claw_openai_codex_auth.h"
#include "llm/claw_llm_http_transport.h"

#define OPENAI_CODEX_RESPONSES_URL "https://chatgpt.com/backend-api/codex/responses"
#define OPENAI_CODEX_DEFAULT_TIMEOUT_MS 120000
#define OPENAI_CODEX_ORIGINATOR "esp_claw"
#define OPENAI_CODEX_CLIENT_VERSION "0.1.0"
#define OPENAI_CODEX_USER_AGENT "esp-claw/0.1.0"

typedef struct {
    char model[96];
    uint32_t timeout_ms;
} openai_codex_backend_ctx_t;

static openai_codex_backend_ctx_t s_ctx;

static char *dup_printf(const char *fmt, ...)
{
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) { va_end(args); return NULL; }
    char *buf = calloc(1, (size_t)needed + 1U);
    if (!buf) { va_end(args); return NULL; }
    vsnprintf(buf, (size_t)needed + 1U, fmt, args);
    va_end(args);
    return buf;
}

static esp_err_t append_text(char **buffer, size_t *length, const char *text)
{
    if (!buffer || !length || !text) return ESP_ERR_INVALID_ARG;
    size_t add = strlen(text);
    if (!add) return ESP_OK;
    char *grown = realloc(*buffer, *length + add + 1U);
    if (!grown) return ESP_ERR_NO_MEM;
    memcpy(grown + *length, text, add);
    *length += add;
    grown[*length] = '\0';
    *buffer = grown;
    return ESP_OK;
}

static char *message_text(cJSON *message)
{
    if (!message || !cJSON_IsObject(message)) return NULL;
    cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
    if (cJSON_IsString(content) && content->valuestring) return strdup(content->valuestring);
    if (!cJSON_IsArray(content)) return NULL;

    char *text = NULL;
    size_t length = 0;
    cJSON *part = NULL;
    cJSON_ArrayForEach(part, content) {
        if (!cJSON_IsObject(part)) continue;
        cJSON *t = cJSON_GetObjectItemCaseSensitive(part, "text");
        if (!cJSON_IsString(t) || !t->valuestring) continue;
        if (append_text(&text, &length, t->valuestring) != ESP_OK) { free(text); return NULL; }
    }
    return text;
}


static esp_err_t add_responses_message(cJSON *input, cJSON *source)
{
    if (!input || !source || !cJSON_IsArray(input) || !cJSON_IsObject(source)) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *codex_output =
        cJSON_GetObjectItemCaseSensitive(source, "_codex_output");

    if (cJSON_IsArray(codex_output)) {
        cJSON *saved = NULL;

        cJSON_ArrayForEach(saved, codex_output) {
            cJSON *dup = cJSON_Duplicate(saved, true);
            if (!dup) return ESP_ERR_NO_MEM;
            cJSON_AddItemToArray(input, dup);
        }

        return ESP_OK;
    }

    const char *role =
        cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(source, "role"));

    if (!role || !role[0]) return ESP_OK;

    if (strcmp(role, "tool") == 0 ||
        strcmp(role, "function") == 0) {

        const char *call_id =
            cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(
                    source, "tool_call_id"));

        if (!call_id || !call_id[0]) {
            return ESP_ERR_INVALID_ARG;
        }

        char *result_text = message_text(source);

        if (!result_text) {
            result_text = strdup("");
        }

        if (!result_text) {
            return ESP_ERR_NO_MEM;
        }

        cJSON *item = cJSON_CreateObject();

        if (!item) {
            free(result_text);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(item, "type", "function_call_output");
        cJSON_AddStringToObject(item, "call_id", call_id);
        cJSON_AddStringToObject(item, "output", result_text);
        cJSON_AddItemToArray(input, item);

        free(result_text);
        return ESP_OK;
    }

    char *text = message_text(source);

    if (!text || !text[0]) {
        free(text);
        return ESP_OK;
    }

    const char *wire_role =
        strcmp(role, "system") == 0 ? "developer" : role;

    const char *part_type =
        strcmp(role, "assistant") == 0
            ? "output_text"
            : "input_text";

    cJSON *item = cJSON_CreateObject();
    cJSON *content = cJSON_CreateArray();
    cJSON *part = cJSON_CreateObject();

    if (!item || !content || !part) {
        cJSON_Delete(item);
        cJSON_Delete(content);
        cJSON_Delete(part);
        free(text);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(item, "type", "message");
    cJSON_AddStringToObject(item, "role", wire_role);
    cJSON_AddStringToObject(part, "type", part_type);
    cJSON_AddStringToObject(part, "text", text);
    cJSON_AddItemToArray(content, part);
    cJSON_AddItemToObject(item, "content", content);
    cJSON_AddItemToArray(input, item);

    free(text);
    return ESP_OK;
}


static esp_err_t add_responses_tools(cJSON *body,
                                     const char *tools_json,
                                     bool *out_has_tools)
{
    if (!body || !out_has_tools) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_has_tools = false;

    if (!tools_json || !tools_json[0]) {
        return ESP_OK;
    }

    cJSON *source_tools = cJSON_Parse(tools_json);

    if (!source_tools || !cJSON_IsArray(source_tools)) {
        cJSON_Delete(source_tools);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *wire_tools = cJSON_CreateArray();

    if (!wire_tools) {
        cJSON_Delete(source_tools);
        return ESP_ERR_NO_MEM;
    }

    cJSON *source_tool = NULL;

    cJSON_ArrayForEach(source_tool, source_tools) {
        if (!cJSON_IsObject(source_tool)) {
            cJSON_Delete(source_tools);
            cJSON_Delete(wire_tools);
            return ESP_ERR_INVALID_ARG;
        }

        const char *type =
            cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(
                    source_tool, "type"));

        if (!type || strcmp(type, "function") != 0) {
            cJSON_Delete(source_tools);
            cJSON_Delete(wire_tools);
            return ESP_ERR_NOT_SUPPORTED;
        }

        cJSON *nested =
            cJSON_GetObjectItemCaseSensitive(source_tool, "function");

        cJSON *definition =
            cJSON_IsObject(nested) ? nested : source_tool;

        const char *name =
            cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(definition, "name"));

        const char *description =
            cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(definition, "description"));

        cJSON *parameters =
            cJSON_GetObjectItemCaseSensitive(definition, "parameters");

        if (!name || !name[0]) {
            cJSON_Delete(source_tools);
            cJSON_Delete(wire_tools);
            return ESP_ERR_INVALID_ARG;
        }

        cJSON *wire_tool = cJSON_CreateObject();

        if (!wire_tool) {
            cJSON_Delete(source_tools);
            cJSON_Delete(wire_tools);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(wire_tool, "type", "function");
        cJSON_AddStringToObject(wire_tool, "name", name);

        if (description && description[0]) {
            cJSON_AddStringToObject(wire_tool, "description", description);
        }

        cJSON *parameters_copy =
            parameters ? cJSON_Duplicate(parameters, true) : cJSON_CreateObject();

        if (!parameters_copy) {
            cJSON_Delete(wire_tool);
            cJSON_Delete(source_tools);
            cJSON_Delete(wire_tools);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddItemToObject(wire_tool, "parameters", parameters_copy);

        cJSON *strict =
            cJSON_GetObjectItemCaseSensitive(definition, "strict");

        if (strict) {
            cJSON *strict_copy = cJSON_Duplicate(strict, true);

            if (!strict_copy) {
                cJSON_Delete(wire_tool);
                cJSON_Delete(source_tools);
                cJSON_Delete(wire_tools);
                return ESP_ERR_NO_MEM;
            }

            cJSON_AddItemToObject(wire_tool, "strict", strict_copy);
        }

        cJSON_AddItemToArray(wire_tools, wire_tool);
    }

    cJSON_Delete(source_tools);

    if (cJSON_GetArraySize(wire_tools) == 0) {
        cJSON_Delete(wire_tools);
        return ESP_OK;
    }

    cJSON_AddItemToObject(body, "tools", wire_tools);
    *out_has_tools = true;
    return ESP_OK;
}



static esp_err_t build_body(const openai_codex_backend_ctx_t *ctx,
                            const claw_llm_chat_request_t *request,
                            char **out_body,
                            char **out_error_message)
{
    *out_body = NULL;

    cJSON *body = cJSON_CreateObject();
    cJSON *input = cJSON_CreateArray();

    if (!body || !input) {
        cJSON_Delete(body);
        cJSON_Delete(input);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(body, "model", ctx->model);

    if (request->system_prompt && request->system_prompt[0]) {
        cJSON_AddStringToObject(body, "instructions", request->system_prompt);
    }

    if (request->messages && cJSON_IsArray(request->messages)) {
        cJSON *message = NULL;

        cJSON_ArrayForEach(message, request->messages) {
            esp_err_t err = add_responses_message(input, message);

            if (err != ESP_OK) {
                cJSON_Delete(body);
                cJSON_Delete(input);
                *out_error_message = strdup("Failed building Codex input");
                return err;
            }
        }
    }

    if (cJSON_GetArraySize(input) == 0) {
        cJSON_Delete(body);
        cJSON_Delete(input);
        *out_error_message = strdup("Codex request has no input messages");
        return ESP_ERR_INVALID_ARG;
    }

    bool has_tools = false;

    esp_err_t tools_err =
        add_responses_tools(body, request->tools_json, &has_tools);

    if (tools_err != ESP_OK) {
        cJSON_Delete(body);
        cJSON_Delete(input);

        *out_error_message =
            strdup(tools_err == ESP_ERR_NOT_SUPPORTED
                ? "Codex received unsupported tool type"
                : "Invalid ESP-Claw tools JSON for Codex");

        return tools_err;
    }

    cJSON_AddItemToObject(body, "input", input);
    cJSON_AddStringToObject(body, "tool_choice", has_tools ? "auto" : "none");
    cJSON_AddBoolToObject(body, "parallel_tool_calls", false);
    cJSON_AddBoolToObject(body, "store", false);
    cJSON_AddBoolToObject(body, "stream", true);

    cJSON *include = cJSON_CreateArray();

    if (!include) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }

    cJSON *encrypted_reasoning =
        cJSON_CreateString("reasoning.encrypted_content");

    if (!encrypted_reasoning) {
        cJSON_Delete(include);
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToArray(include, encrypted_reasoning);
    cJSON_AddItemToObject(body, "include", include);

    cJSON *reasoning = cJSON_CreateObject();

    if (!reasoning) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(reasoning, "effort", "low");
    cJSON_AddStringToObject(reasoning, "summary", "auto");
    cJSON_AddItemToObject(body, "reasoning", reasoning);

    *out_body = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    return *out_body ? ESP_OK : ESP_ERR_NO_MEM;
}


static esp_err_t output_item_text(cJSON *event, char **fallback, size_t *fallback_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(event, "item");
    if (!cJSON_IsObject(item)) return ESP_OK;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "type"));
    const char *role = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "role"));
    if (!type || strcmp(type,"message") != 0 || !role || strcmp(role,"assistant") != 0) return ESP_OK;
    cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
    if (!cJSON_IsArray(content)) return ESP_OK;
    cJSON *part = NULL;
    cJSON_ArrayForEach(part, content) {
        const char *pt = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(part,"type"));
        const char *tx = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(part,"text"));
        if (pt && strcmp(pt,"output_text") == 0 && tx) {
            esp_err_t err = append_text(fallback,fallback_len,tx);
            if (err != ESP_OK) return err;
        }
    }
    return ESP_OK;
}


static void codex_clear_tool_calls(claw_llm_response_t *response)
{
    if (!response) return;

    for (size_t i = 0; i < response->tool_call_count; i++) {
        free(response->tool_calls[i].id);
        free(response->tool_calls[i].name);
        free(response->tool_calls[i].arguments_json);
    }

    free(response->tool_calls);
    response->tool_calls = NULL;
    response->tool_call_count = 0;
}


static esp_err_t codex_append_tool_call(claw_llm_response_t *response,
                                        cJSON *item)
{
    if (!response || !item || !cJSON_IsObject(item)) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *call_id =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "call_id"));
    const char *name =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "name"));
    const char *arguments =
        cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, "arguments"));

    if (!call_id || !call_id[0] ||
        !name || !name[0] ||
        !arguments) {
        return ESP_FAIL;
    }

    size_t old_count = response->tool_call_count;

    claw_llm_tool_call_t *grown =
        realloc(response->tool_calls,
                (old_count + 1U) * sizeof(claw_llm_tool_call_t));

    if (!grown) {
        return ESP_ERR_NO_MEM;
    }

    response->tool_calls = grown;

    claw_llm_tool_call_t *dst =
        &response->tool_calls[old_count];

    memset(dst, 0, sizeof(*dst));

    dst->id = strdup(call_id);
    dst->name = strdup(name);
    dst->arguments_json = strdup(arguments);

    if (!dst->id || !dst->name || !dst->arguments_json) {
        free(dst->id);
        free(dst->name);
        free(dst->arguments_json);
        memset(dst, 0, sizeof(*dst));
        return ESP_ERR_NO_MEM;
    }

    response->tool_call_count = old_count + 1U;
    return ESP_OK;
}


static esp_err_t parse_sse(const char *body,
                           claw_llm_response_t *out_response,
                           char **out_error_message)
{
    memset(out_response, 0, sizeof(*out_response));

    char *copy = strdup(body ? body : "");
    if (!copy) return ESP_ERR_NO_MEM;

    cJSON *output_items = cJSON_CreateArray();

    if (!output_items) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }

    char *text = NULL;
    char *fallback = NULL;
    size_t text_len = 0;
    size_t fallback_len = 0;
    bool completed = false;
    esp_err_t err = ESP_OK;
    char *saveptr = NULL;

    for (char *line = strtok_r(copy, "\n", &saveptr);
         line;
         line = strtok_r(NULL, "\n", &saveptr)) {

        size_t n = strlen(line);

        if (n && line[n - 1] == '\r') {
            line[n - 1] = '\0';
        }

        if (strncmp(line, "data:", 5) != 0) continue;

        const char *payload = line + 5;

        while (*payload == ' ' || *payload == '\t') {
            payload++;
        }

        if (!payload[0] || strcmp(payload, "[DONE]") == 0) continue;

        cJSON *event = cJSON_Parse(payload);
        if (!event) continue;

        const char *type =
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "type"));

        if (type && strcmp(type, "response.output_text.delta") == 0) {
            const char *delta =
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(event, "delta"));

            if (delta) {
                err = append_text(&text, &text_len, delta);
            }

        } else if (type && strcmp(type, "response.output_item.done") == 0) {
            cJSON *item =
                cJSON_GetObjectItemCaseSensitive(event, "item");

            if (cJSON_IsObject(item)) {
                cJSON *saved = cJSON_Duplicate(item, true);

                if (!saved) {
                    err = ESP_ERR_NO_MEM;
                } else {
                    cJSON_AddItemToArray(output_items, saved);
                }

                if (err == ESP_OK) {
                    const char *item_type =
                        cJSON_GetStringValue(
                            cJSON_GetObjectItemCaseSensitive(item, "type"));

                    if (item_type && strcmp(item_type, "function_call") == 0) {
                        err = codex_append_tool_call(out_response, item);
                    } else {
                        err = output_item_text(event, &fallback, &fallback_len);
                    }
                }
            }

        } else if (type && strcmp(type, "response.completed") == 0) {
            completed = true;

        } else if (type && strcmp(type, "response.failed") == 0) {
            cJSON *response =
                cJSON_GetObjectItemCaseSensitive(event, "response");

            cJSON *error_obj =
                cJSON_IsObject(response)
                    ? cJSON_GetObjectItemCaseSensitive(response, "error")
                    : NULL;

            const char *message =
                cJSON_IsObject(error_obj)
                    ? cJSON_GetStringValue(
                          cJSON_GetObjectItemCaseSensitive(error_obj, "message"))
                    : NULL;

            *out_error_message =
                dup_printf("Codex response.failed%s%s",
                           message ? ": " : "",
                           message ? message : "");

            err = ESP_FAIL;
        }

        cJSON_Delete(event);

        if (err != ESP_OK) break;
    }

    free(copy);

    if (err != ESP_OK) {
        free(text);
        free(fallback);
        cJSON_Delete(output_items);
        codex_clear_tool_calls(out_response);

        if (!*out_error_message) {
            *out_error_message =
                strdup(err == ESP_ERR_NO_MEM
                    ? "Out of memory parsing Codex response"
                    : "Malformed Codex function_call response");
        }

        return err;
    }

    if (!completed) {
        free(text);
        free(fallback);
        cJSON_Delete(output_items);
        codex_clear_tool_calls(out_response);

        *out_error_message =
            strdup("Codex SSE stream ended before response.completed");

        return ESP_FAIL;
    }

    if (!text || !text[0]) {
        free(text);
        text = fallback;
        fallback = NULL;
    }

    free(fallback);

    if ((!text || !text[0]) && out_response->tool_call_count == 0) {
        free(text);
        cJSON_Delete(output_items);

        *out_error_message =
            strdup("Codex returned no assistant text or tool call");

        return ESP_FAIL;
    }

    out_response->text = text;

    cJSON *raw = cJSON_CreateObject();

    if (!raw) {
        free(out_response->text);
        out_response->text = NULL;
        cJSON_Delete(output_items);
        codex_clear_tool_calls(out_response);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(raw, "role", "assistant");

    if (out_response->tool_call_count > 0) {
        if (out_response->text && out_response->text[0]) {
            cJSON_AddStringToObject(raw, "content", out_response->text);
        }

        cJSON_AddItemToObject(raw, "_codex_output", output_items);
        output_items = NULL;

    } else {
        cJSON_Delete(output_items);
        output_items = NULL;

        cJSON_AddStringToObject(
            raw,
            "content",
            out_response->text ? out_response->text : "");
    }

    out_response->raw_message_json =
        cJSON_PrintUnformatted(raw);

    cJSON_Delete(raw);

    if (!out_response->raw_message_json) {
        free(out_response->text);
        out_response->text = NULL;
        codex_clear_tool_calls(out_response);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}


static esp_err_t openai_codex_init(const claw_llm_runtime_config_t *config,
                                   const claw_llm_model_profile_t *profile,
                                   void **out_backend_ctx,
                                   char **out_error_message)
{
    (void)profile;
    if (!config || !out_backend_ctx || !out_error_message) return ESP_ERR_INVALID_ARG;
    *out_error_message = NULL;
    if (!config->model || !config->model[0]) {
        *out_error_message = strdup("OpenAI Codex model is not configured");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(s_ctx.model,config->model,sizeof(s_ctx.model)) >= sizeof(s_ctx.model)) {
        *out_error_message = strdup("OpenAI Codex model name is too long");
        return ESP_ERR_INVALID_SIZE;
    }
    s_ctx.timeout_ms = config->timeout_ms ? config->timeout_ms : OPENAI_CODEX_DEFAULT_TIMEOUT_MS;
    *out_backend_ctx = &s_ctx;
    return ESP_OK;
}

static esp_err_t openai_codex_chat(void *backend_ctx,
                                   const claw_llm_model_profile_t *profile,
                                   const claw_llm_chat_request_t *request,
                                   claw_llm_response_t *out_response,
                                   char **out_error_message)
{
    (void)profile;
    if (!backend_ctx || !request || !out_response || !out_error_message) return ESP_ERR_INVALID_ARG;
    *out_error_message = NULL;
    memset(out_response,0,sizeof(*out_response));

    openai_codex_backend_ctx_t *ctx = (openai_codex_backend_ctx_t *)backend_ctx;
    char *access_token = NULL, *account_id = NULL, *post_data = NULL, *transport_error = NULL;
    claw_llm_http_response_t http_response = {0};

    esp_err_t err = claw_openai_codex_auth_get_session(&access_token,&account_id);
    if (err != ESP_OK) {
        *out_error_message = strdup("ChatGPT OAuth session is not connected");
        goto cleanup;
    }
    err = build_body(ctx,request,&post_data,out_error_message);
    if (err != ESP_OK) goto cleanup;

    const claw_llm_http_header_t headers[] = {
        {"ChatGPT-Account-ID",account_id},
        {"Accept","text/event-stream"},
        {"originator",OPENAI_CODEX_ORIGINATOR},
        {"User-Agent",OPENAI_CODEX_USER_AGENT},
    };

    claw_llm_http_json_request_t http_request = {0};
    http_request.url = OPENAI_CODEX_RESPONSES_URL;
    http_request.body = post_data;
    http_request.api_key = access_token;
    http_request.auth_type = "bearer";
    http_request.timeout_ms = ctx->timeout_ms;
    http_request.abort_flag = request->abort_flag;
    http_request.headers = headers;
    http_request.header_count = sizeof(headers)/sizeof(headers[0]);
    http_request.content_type = "application/json";
    http_request.accept_non_200 = true;

    err = claw_llm_http_post_json(&http_request,&http_response,&transport_error);
    if (err != ESP_OK) {
        *out_error_message = transport_error ? transport_error : strdup("Codex HTTP transport failed");
        transport_error = NULL;
        goto cleanup;
    }
    if (http_response.status_code < 200 || http_response.status_code >= 300) {
        *out_error_message = dup_printf("Codex HTTP %d: %.320s",http_response.status_code,
                                        http_response.body ? http_response.body : "(empty response)");
        err = ESP_FAIL;
        goto cleanup;
    }
    err = parse_sse(http_response.body ? http_response.body : "",out_response,out_error_message);

cleanup:
    free(access_token); free(account_id); free(post_data); free(transport_error);
    claw_llm_http_response_free(&http_response);
    return err;
}

static esp_err_t openai_codex_infer_media(void *backend_ctx,
                                          const claw_llm_model_profile_t *profile,
                                          const claw_llm_media_request_t *request,
                                          char **out_text,
                                          char **out_error_message)
{
    (void)backend_ctx;
    if (!profile || !request || !out_text || !out_error_message) return ESP_ERR_INVALID_ARG;
    *out_text = NULL;
    *out_error_message = strdup("OpenAI Codex media inference is not implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

static const claw_llm_backend_vtable_t s_openai_codex_vtable = {
    .id = CLAW_LLM_BACKEND_OPENAI_CODEX_ID,
    .init = openai_codex_init,
    .chat = openai_codex_chat,
    .infer_media = openai_codex_infer_media,
};

static const claw_llm_backend_registration_t s_openai_codex_registration = {
    .id = CLAW_LLM_BACKEND_OPENAI_CODEX_ID,
    .vtable = &s_openai_codex_vtable,
    .defaults = {
        .auth_type = CLAW_LLM_BACKEND_OPENAI_CODEX_AUTH_TYPE,
        .chat_path = CLAW_LLM_BACKEND_OPENAI_CODEX_CHAT_PATH,
        .max_tokens_field = CLAW_LLM_BACKEND_OPENAI_CODEX_DEFAULT_MAX_TOKENS_FIELD,
    },
};

const claw_llm_backend_vtable_t *claw_llm_backend_openai_codex_vtable(void)
{
    return &s_openai_codex_vtable;
}

const claw_llm_backend_registration_t *claw_llm_backend_openai_codex_registration(void)
{
    return &s_openai_codex_registration;
}