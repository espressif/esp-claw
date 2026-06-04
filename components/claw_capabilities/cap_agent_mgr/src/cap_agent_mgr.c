/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_agent_mgr.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_agent_mgr.h"
#include "claw_cap.h"
#include "esp_log.h"

static const char *TAG = "cap_agent_mgr";

// max number of agents
#define CAP_AGENT_MGR_LIST_MAX 16

static const char *cap_agent_mgr_get_string(cJSON *root, const char *name)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, name));
}

static esp_err_t cap_agent_mgr_require_root(const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    if (!ctx || (ctx->caller != CLAW_CAP_CALLER_AGENT &&
                 ctx->caller != CLAW_CAP_CALLER_ROOT_AGENT)) {
        snprintf(output, output_size, "Error: subagent management tools are root-agent only.");
        return ESP_ERR_INVALID_STATE;
    }
    if (!ctx->session_id || !ctx->session_id[0]) {
        snprintf(output, output_size, "Error: missing parent session_id.");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t cap_agent_mgr_spawn_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    const char *prompt = NULL;
    const char *agent_type = NULL;
    cJSON *background_item = NULL;
    char agent_id[CLAW_SESSION_MGR_ID_SIZE] = {0};
    esp_err_t err;

    err = cap_agent_mgr_require_root(ctx, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input.");
        return ESP_ERR_INVALID_ARG;
    }
    prompt = cap_agent_mgr_get_string(root, "prompt");
    agent_type = cap_agent_mgr_get_string(root, "agent_type");
    background_item = cJSON_GetObjectItemCaseSensitive(root, "background");
    if (!prompt || !prompt[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing prompt.");
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_agent_mgr_spawn_subagent(ctx,
                                        prompt,
                                        agent_type,
                                        cJSON_IsBool(background_item) ? cJSON_IsTrue(background_item) : true,
                                        agent_id,
                                        sizeof(agent_id));
    cJSON_Delete(root);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spawn_agent failed: %s", esp_err_to_name(err));
        snprintf(output, output_size, "Error: spawn_agent failed: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(output, output_size, "{\"agent_id\":\"%s\",\"status\":\"running\"}", agent_id);
    return ESP_OK;
}

static esp_err_t cap_agent_mgr_send_input_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    cJSON *root = NULL;
    const char *agent_id = NULL;
    const char *input = NULL;
    cJSON *interrupt_item = NULL;
    char agent_id_copy[CLAW_SESSION_MGR_ID_SIZE] = {0};
    esp_err_t err;

    err = cap_agent_mgr_require_root(ctx, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input.");
        return ESP_ERR_INVALID_ARG;
    }
    agent_id = cap_agent_mgr_get_string(root, "agent_id");
    input = cap_agent_mgr_get_string(root, "input");
    interrupt_item = cJSON_GetObjectItemCaseSensitive(root, "interrupt");
    if (!agent_id || !agent_id[0] || !input || !input[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing agent_id or input.");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(agent_id_copy, agent_id, sizeof(agent_id_copy)) >= sizeof(agent_id_copy)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: agent_id is too long.");
        return ESP_ERR_INVALID_SIZE;
    }

    err = claw_agent_mgr_send_subagent_input(ctx,
                                             agent_id_copy,
                                             input,
                                             cJSON_IsBool(interrupt_item) ? cJSON_IsTrue(interrupt_item) : false);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: send_input failed: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "{\"agent_id\":\"%s\",\"status\":\"submitted\"}", agent_id_copy);
    return ESP_OK;
}

static esp_err_t cap_agent_mgr_inspect_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = NULL;
    const char *agent_id = NULL;
    claw_agent_mgr_agent_info_t info = {0};
    esp_err_t err;

    err = cap_agent_mgr_require_root(ctx, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input.");
        return ESP_ERR_INVALID_ARG;
    }
    agent_id = cap_agent_mgr_get_string(root, "agent_id");
    if (!agent_id || !agent_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing agent_id.");
        return ESP_ERR_INVALID_ARG;
    }
    err = claw_agent_mgr_inspect_agent(ctx, agent_id, &info);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: inspect_agent failed: %s", esp_err_to_name(err));
        return err;
    }

    snprintf(output,
             output_size,
             "{\"agent_id\":\"%s\",\"status\":\"%s\",\"phase\":%d,\"last_request_id\":%" PRIu32 ",\"agent_type\":\"%s\",\"last_error\":\"%s\"}",
             info.agent_id,
             claw_agent_mgr_status_to_string(info.status),
             (int)info.phase,
             info.last_request_id,
             info.agent_type,
             info.last_error);
    return ESP_OK;
}

static esp_err_t cap_agent_mgr_list_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    claw_agent_mgr_agent_info_t *infos = NULL;
    size_t count = 0;
    cJSON *root = NULL;
    cJSON *agents = NULL;
    char *json = NULL;
    esp_err_t err;

    (void)input_json;

    err = cap_agent_mgr_require_root(ctx, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    infos = calloc(CAP_AGENT_MGR_LIST_MAX, sizeof(*infos));
    if (!infos) {
        snprintf(output, output_size, "Error: out of memory.");
        return ESP_ERR_NO_MEM;
    }

    err = claw_agent_mgr_list_agents(ctx, infos, CAP_AGENT_MGR_LIST_MAX, &count);
    if (err != ESP_OK) {
        free(infos);
        snprintf(output, output_size, "Error: list_agents failed: %s", esp_err_to_name(err));
        return err;
    }

    root = cJSON_CreateObject();
    agents = cJSON_AddArrayToObject(root, "agents");
    if (!root || !agents) {
        cJSON_Delete(root);
        free(infos);
        snprintf(output, output_size, "Error: out of memory.");
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();

        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "agent_id", infos[i].agent_id);
        cJSON_AddStringToObject(item, "status", claw_agent_mgr_status_to_string(infos[i].status));
        cJSON_AddStringToObject(item, "agent_type", infos[i].agent_type);
        cJSON_AddNumberToObject(item, "phase", (double)infos[i].phase);
        cJSON_AddNumberToObject(item, "last_request_id", (double)infos[i].last_request_id);
        if (infos[i].last_error[0]) {
            cJSON_AddStringToObject(item, "last_error", infos[i].last_error);
        }
        cJSON_AddItemToArray(agents, item);
    }
    cJSON_AddNumberToObject(root, "count", (double)count);

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(infos);
    if (!json) {
        snprintf(output, output_size, "Error: out of memory.");
        return ESP_ERR_NO_MEM;
    }
    if (strlcpy(output, json, output_size) >= output_size) {
        free(json);
        snprintf(output, output_size, "Error: agent list is too large.");
        return ESP_ERR_INVALID_SIZE;
    }
    free(json);
    return ESP_OK;
}

static esp_err_t cap_agent_mgr_close_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    cJSON *root = NULL;
    const char *agent_id = NULL;
    char agent_id_copy[CLAW_SESSION_MGR_ID_SIZE] = {0};
    esp_err_t err;

    err = cap_agent_mgr_require_root(ctx, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input.");
        return ESP_ERR_INVALID_ARG;
    }
    agent_id = cap_agent_mgr_get_string(root, "agent_id");
    if (!agent_id || !agent_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing agent_id.");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(agent_id_copy, agent_id, sizeof(agent_id_copy)) >= sizeof(agent_id_copy)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: agent_id is too long.");
        return ESP_ERR_INVALID_SIZE;
    }
    err = claw_agent_mgr_close_agent(ctx, agent_id_copy);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: close_agent failed: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "{\"agent_id\":\"%s\",\"status\":\"closed\"}", agent_id_copy);
    return ESP_OK;
}

static esp_err_t cap_agent_mgr_delete_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    cJSON *root = NULL;
    const char *agent_id = NULL;
    char agent_id_copy[CLAW_SESSION_MGR_ID_SIZE] = {0};
    esp_err_t err;

    err = cap_agent_mgr_require_root(ctx, output, output_size);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: invalid JSON input.");
        return ESP_ERR_INVALID_ARG;
    }
    agent_id = cap_agent_mgr_get_string(root, "agent_id");
    if (!agent_id || !agent_id[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing agent_id.");
        return ESP_ERR_INVALID_ARG;
    }
    if (strlcpy(agent_id_copy, agent_id, sizeof(agent_id_copy)) >= sizeof(agent_id_copy)) {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: agent_id is too long.");
        return ESP_ERR_INVALID_SIZE;
    }
    err = claw_agent_mgr_delete_agent(ctx, agent_id_copy);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: delete_agent failed: %s", esp_err_to_name(err));
        return err;
    }
    snprintf(output, output_size, "{\"agent_id\":\"%s\",\"status\":\"deleted\"}", agent_id_copy);
    return ESP_OK;
}

static const claw_cap_descriptor_t s_agent_mgr_caps[] = {
    {
        .id = "spawn_agent",
        .name = "spawn_agent",
        .family = "agent_mgr",
        .description = "Spawn an asynchronous subagent from an explicit prompt.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"},\"agent_type\":{\"type\":\"string\"},\"background\":{\"type\":\"boolean\"}},\"required\":[\"prompt\"]}",
        .execute = cap_agent_mgr_spawn_execute,
    },
    {
        .id = "send_agent_followup",
        .name = "send_agent_followup",
        .family = "agent_mgr",
        .description = "Continue delegating work to a subagent by sending a follow-up message. Use this to add new tasks, refine existing tasks, guide an active subagent, or leave instructions for a closed subagent.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"agent_id\":{\"type\":\"string\"},\"input\":{\"type\":\"string\"},\"interrupt\":{\"type\":\"boolean\"}},\"required\":[\"agent_id\",\"input\"]}",
        .execute = cap_agent_mgr_send_input_execute,
    },
    {
        .id = "inspect_agent",
        .name = "inspect_agent",
        .family = "agent_mgr",
        .description = "Inspect a subagent lifecycle status and lightweight progress.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"agent_id\":{\"type\":\"string\"}},\"required\":[\"agent_id\"]}",
        .execute = cap_agent_mgr_inspect_execute,
    },
    {
        .id = "list_agents",
        .name = "list_agents",
        .family = "agent_mgr",
        .description = "List the live and closed subagents spawned in the current session, with their lifecycle status.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_agent_mgr_list_execute,
    },
    {
        .id = "close_agent",
        .name = "close_agent",
        .family = "agent_mgr",
        .description = "Close a live subagent runtime while preserving persisted session history.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"agent_id\":{\"type\":\"string\"}},\"required\":[\"agent_id\"]}",
        .execute = cap_agent_mgr_close_execute,
    },
    {
        .id = "delete_agent",
        .name = "delete_agent",
        .family = "agent_mgr",
        .description = "Permanently delete a closed or live subagent runtime history and parent map entry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM | CLAW_CAP_FLAG_ROOT_AGENT_ONLY,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"agent_id\":{\"type\":\"string\"}},\"required\":[\"agent_id\"]}",
        .execute = cap_agent_mgr_delete_execute,
    },
};

static const claw_cap_group_t s_agent_mgr_group = {
    .group_id = "cap_agent_mgr",
    .plugin_name = "cap_agent_mgr",
    .version = "1.0.0",
    .descriptors = s_agent_mgr_caps,
    .descriptor_count = sizeof(s_agent_mgr_caps) / sizeof(s_agent_mgr_caps[0]),
};

esp_err_t cap_agent_mgr_register_group(void)
{
    return claw_cap_register_group(&s_agent_mgr_group);
}
