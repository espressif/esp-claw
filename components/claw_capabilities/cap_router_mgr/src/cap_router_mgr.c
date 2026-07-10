/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_router_mgr.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_router.h"
#include "esp_err.h"

static const char *CAP_ROUTER_MGR_ADD = "add_router_rule";
static const char *CAP_ROUTER_MGR_UPDATE = "update_router_rule";
static const char *CAP_ROUTER_MGR_DELETE = "delete_router_rule";
static const char *CAP_ROUTER_MGR_RELOAD = "reload_router_rules";

static void cap_router_mgr_write_error(char *output,
                                       size_t output_size,
                                       const char *error,
                                       const char *id,
                                       esp_err_t err)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    if (!output || output_size == 0) {
        return;
    }

    root = cJSON_CreateObject();
    if (!root) {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"%s\",\"code\":\"%s\"}",
                 error ? error : "unknown error",
                 esp_err_to_name(err));
        return;
    }

    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error ? error : "unknown error");
    cJSON_AddStringToObject(root, "code", esp_err_to_name(err));
    if (id && id[0]) {
        cJSON_AddStringToObject(root, "id", id);
    }

    rendered = cJSON_PrintUnformatted(root);
    if (rendered) {
        snprintf(output, output_size, "%s", rendered);
        free(rendered);
    } else {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"%s\",\"code\":\"%s\"}",
                 error ? error : "unknown error",
                 esp_err_to_name(err));
    }
    cJSON_Delete(root);
}

static const char *cap_router_mgr_apply_error_reason(const char *action, esp_err_t err)
{
    if (!action) {
        return "failed to apply router rule";
    }

    if (strcmp(action, CAP_ROUTER_MGR_ADD) == 0) {
        if (err == ESP_ERR_INVALID_STATE) {
            return "rule id already exists; use update_router_rule";
        }
        if (err == ESP_ERR_INVALID_SIZE) {
            return "router rule limit reached";
        }
        if (err == ESP_ERR_INVALID_ARG) {
            return "invalid rule_json structure";
        }
        return "failed to add router rule";
    }

    if (strcmp(action, CAP_ROUTER_MGR_UPDATE) == 0) {
        if (err == ESP_ERR_NOT_FOUND) {
            return "rule id not found; use add_router_rule";
        }
        if (err == ESP_ERR_INVALID_ARG) {
            return "invalid rule_json structure";
        }
        return "failed to update router rule";
    }

    return "failed to apply router rule";
}

static esp_err_t cap_router_mgr_write_action_result(const char *action,
                                                    const char *id,
                                                    char *output,
                                                    size_t output_size)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    if (!action || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddBoolToObject(root, "ok", true);
    if (id && id[0]) {
        cJSON_AddStringToObject(root, "id", id);
    }

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_router_mgr_parse_input(const char *input_json, cJSON **out_root)
{
    cJSON *root = NULL;

    if (!input_json || !out_root) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    *out_root = root;
    return ESP_OK;
}

static esp_err_t cap_router_mgr_extract_id(cJSON *root, const char **out_id)
{
    const char *id = NULL;

    if (!root || !out_id) {
        return ESP_ERR_INVALID_ARG;
    }

    id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
    if (!id || !id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_id = id;
    return ESP_OK;
}

static esp_err_t cap_router_mgr_extract_rule_json_id(const char *rule_json,
                                                     char *id_buf,
                                                     size_t id_buf_size)
{
    cJSON *root = NULL;
    const char *id = NULL;

    if (!rule_json || !id_buf || id_buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(rule_json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
    if (!id || !id[0]) {
        cJSON_Delete(root);
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(id_buf, id, id_buf_size);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_router_mgr_list_execute(const char *input_json,
                                             const claw_cap_call_context_t *ctx,
                                             char *output,
                                             size_t output_size)
{
    (void)input_json;
    (void)ctx;
    return claw_event_router_list_rules_json(output, output_size);
}

static esp_err_t cap_router_mgr_get_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    const char *id = NULL;
    char id_buf[64] = {0};
    esp_err_t err;

    (void)ctx;

    err = cap_router_mgr_parse_input(input_json, &root);
    if (err != ESP_OK) {
        cap_router_mgr_write_error(output, output_size, "invalid input json", NULL, err);
        return err;
    }

    err = cap_router_mgr_extract_id(root, &id);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_router_mgr_write_error(output, output_size, "id is required", NULL, err);
        return err;
    }
    strlcpy(id_buf, id, sizeof(id_buf));
    cJSON_Delete(root);

    err = claw_event_router_get_rule_json(id_buf, output, output_size);
    if (err != ESP_OK) {
        cap_router_mgr_write_error(output,
                                   output_size,
                                   err == ESP_ERR_NOT_FOUND ? "router rule id not found" :
                                   "failed to get router rule",
                                   id_buf,
                                   err);
    }
    return err;
}

static esp_err_t cap_router_mgr_apply_rule_json(const char *input_json,
                                                const char *action,
                                                esp_err_t (*fn)(const char *rule_json),
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    const char *rule_json = NULL;
    char *rule_json_dup = NULL;
    char id[64] = {0};
    esp_err_t err;

    err = cap_router_mgr_parse_input(input_json, &root);
    if (err != ESP_OK) {
        cap_router_mgr_write_error(output, output_size, "invalid input json", NULL, err);
        return err;
    }

    rule_json = cJSON_GetStringValue(cJSON_GetObjectItem(root, "rule_json"));
    if (!rule_json || !rule_json[0]) {
        cJSON_Delete(root);
        cap_router_mgr_write_error(output,
                                   output_size,
                                   "rule_json is required",
                                   NULL,
                                   ESP_ERR_INVALID_ARG);
        return ESP_ERR_INVALID_ARG;
    }
    rule_json_dup = strdup(rule_json);
    if (!rule_json_dup) {
        cJSON_Delete(root);
        cap_router_mgr_write_error(output, output_size, "out of memory", NULL, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    (void)cap_router_mgr_extract_rule_json_id(rule_json_dup, id, sizeof(id));
    cJSON_Delete(root);

    err = fn(rule_json_dup);
    free(rule_json_dup);
    if (err != ESP_OK) {
        cap_router_mgr_write_error(output,
                                   output_size,
                                   cap_router_mgr_apply_error_reason(action, err),
                                   id,
                                   err);
        return err;
    }

    return cap_router_mgr_write_action_result(action, id, output, output_size);
}

static esp_err_t cap_router_mgr_add_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    (void)ctx;
    return cap_router_mgr_apply_rule_json(input_json,
                                          CAP_ROUTER_MGR_ADD,
                                          claw_event_router_add_rule_json,
                                          output,
                                          output_size);
}

static esp_err_t cap_router_mgr_update_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    (void)ctx;
    return cap_router_mgr_apply_rule_json(input_json,
                                          CAP_ROUTER_MGR_UPDATE,
                                          claw_event_router_update_rule_json,
                                          output,
                                          output_size);
}

static esp_err_t cap_router_mgr_delete_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    cJSON *root = NULL;
    const char *id = NULL;
    char id_buf[64] = {0};
    esp_err_t err;

    (void)ctx;

    err = cap_router_mgr_parse_input(input_json, &root);
    if (err != ESP_OK) {
        cap_router_mgr_write_error(output, output_size, "invalid input json", NULL, err);
        return err;
    }

    err = cap_router_mgr_extract_id(root, &id);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_router_mgr_write_error(output, output_size, "id is required", NULL, err);
        return err;
    }
    strlcpy(id_buf, id, sizeof(id_buf));
    cJSON_Delete(root);

    err = claw_event_router_delete_rule(id_buf);
    if (err != ESP_OK) {
        cap_router_mgr_write_error(output,
                                   output_size,
                                   err == ESP_ERR_NOT_FOUND ? "router rule id not found" :
                                   "failed to delete router rule",
                                   id_buf,
                                   err);
        return err;
    }

    return cap_router_mgr_write_action_result(CAP_ROUTER_MGR_DELETE, id_buf, output, output_size);
}

static esp_err_t cap_router_mgr_reload_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    esp_err_t err;

    (void)input_json;
    (void)ctx;

    err = claw_event_router_reload();
    if (err != ESP_OK) {
        cap_router_mgr_write_error(output, output_size, "failed to reload router rules", NULL, err);
        return err;
    }

    return cap_router_mgr_write_action_result(CAP_ROUTER_MGR_RELOAD, NULL, output, output_size);
}

static const claw_cap_descriptor_t s_router_mgr_descriptors[] = {
    {
        .id = "list_router_rules",
        .name = "list_router_rules",
        .family = "router_manager",
        .description = "List all automation rules as JSON.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_router_mgr_list_execute,
    },
    {
        .id = "get_router_rule",
        .name = "get_router_rule",
        .family = "router_manager",
        .description = "Get one automation rule by id as JSON.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        .execute = cap_router_mgr_get_execute,
    },
    {
        .id = "add_router_rule",
        .name = "add_router_rule",
        .family = "router_manager",
        .description = "Add one automation rule from rule JSON.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"rule_json\":{\"type\":\"string\"}},\"required\":[\"rule_json\"]}",
        .execute = cap_router_mgr_add_execute,
    },
    {
        .id = "update_router_rule",
        .name = "update_router_rule",
        .family = "router_manager",
        .description = "Update one automation rule from rule JSON.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"rule_json\":{\"type\":\"string\"}},\"required\":[\"rule_json\"]}",
        .execute = cap_router_mgr_update_execute,
    },
    {
        .id = "delete_router_rule",
        .name = "delete_router_rule",
        .family = "router_manager",
        .description = "Delete one automation rule by id.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"string\"}},\"required\":[\"id\"]}",
        .execute = cap_router_mgr_delete_execute,
    },
    {
        .id = "reload_router_rules",
        .name = "reload_router_rules",
        .family = "router_manager",
        .description = "Reload automation rules from disk.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_router_mgr_reload_execute,
    },
};

static const claw_cap_group_t s_router_mgr_group = {
    .group_id = "cap_router_mgr",
    .descriptors = s_router_mgr_descriptors,
    .descriptor_count = sizeof(s_router_mgr_descriptors) / sizeof(s_router_mgr_descriptors[0]),
};

esp_err_t cap_router_mgr_register_group(void)
{
    if (claw_cap_group_exists(s_router_mgr_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_router_mgr_group);
}
