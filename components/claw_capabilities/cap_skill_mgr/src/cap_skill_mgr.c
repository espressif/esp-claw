/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_skill.h"
#include "cap_skill_mgr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cap_skill_mgr";

#define CAP_SKILL_STRINGIFY_INNER(value) #value
#define CAP_SKILL_STRINGIFY(value) CAP_SKILL_STRINGIFY_INNER(value)
#define CAP_SKILL_ID_SCHEMA \
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{" \
    "\"skill_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":" CAP_SKILL_STRINGIFY(CLAW_SKILL_ID_MAX_LEN) "," \
    "\"pattern\":\"^[A-Za-z0-9_-]+$\"}},\"required\":[\"skill_id\"]}"

static SemaphoreHandle_t s_skill_mutation_lock;

static void cap_skill_free_string_array(char **items, size_t count)
{
    if (!items) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static esp_err_t cap_skill_sync_session_visible_groups(const char *session_id)
{
    char **group_ids = NULL;
    size_t group_count = 0;
    esp_err_t err;

    if (!session_id || !session_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    err = claw_skill_load_active_cap_groups(session_id, &group_ids, &group_count);
    if (err == ESP_OK) {
        err = claw_cap_set_session_llm_visible_groups(session_id, (const char *const *)group_ids, group_count);
    }
    cap_skill_free_string_array(group_ids, group_count);
    return err;
}

static void cap_skill_write_error(char *output, size_t output_size, const char *code, const char *message, const char *skill_id)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    if (!output || output_size == 0) {
        return;
    }
    root = cJSON_CreateObject();
    if (!root || !cJSON_AddBoolToObject(root, "ok", false) ||
            !cJSON_AddStringToObject(root, "code", code ? code : "unknown") ||
            !cJSON_AddStringToObject(root, "error", message ? message : "unknown error") ||
            (skill_id && skill_id[0] && !cJSON_AddStringToObject(root, "skill_id", skill_id))) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"ok\":false,\"code\":\"unknown\",\"error\":\"unknown error\"}");
        return;
    }
    rendered = cJSON_PrintUnformatted(root);
    if (rendered) {
        snprintf(output, output_size, "%s", rendered);
        free(rendered);
    } else {
        snprintf(output, output_size, "{\"ok\":false,\"code\":\"out_of_memory\",\"error\":\"out of memory\"}");
    }
    cJSON_Delete(root);
}

static esp_err_t cap_skill_render_result(cJSON *root, char *output, size_t output_size)
{
    char *rendered = NULL;

    if (!root || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    output[0] = '\0';
    rendered = cJSON_PrintUnformatted(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    if (strlen(rendered) >= output_size) {
        free(rendered);
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_skill_parse_id(const char *input_json, char skill_id[CLAW_SKILL_ID_MAX_LEN + 1], char *output, size_t output_size)
{
    cJSON *root = cJSON_ParseWithOpts(input_json ? input_json : "{}", NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "invalid_input", "input must be a JSON object", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *field = NULL;
    cJSON_ArrayForEach(field, root) {
        if (!field->string || strcmp(field->string, "skill_id") != 0) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "invalid_input", "unknown input field", NULL);
            return ESP_ERR_INVALID_ARG;
        }
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");
    if (!cJSON_IsString(item) || !claw_skill_id_is_valid(item->valuestring)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "invalid_skill_id", "skill_id must match ^[A-Za-z0-9_-]{1,63}$", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(skill_id, CLAW_SKILL_ID_MAX_LEN + 1, "%s", item->valuestring);
    cJSON_Delete(root);
    return ESP_OK;
}

static const char *cap_skill_manage_mode_to_string(claw_skill_manage_mode_t mode)
{
    return mode == CLAW_SKILL_MANAGE_MODE_RUNTIME ? "runtime" : "readonly";
}

static cJSON *cap_skill_catalog_entry_to_json(const claw_skill_catalog_entry_t *entry)
{
    cJSON *skill = NULL;
    cJSON *cap_groups = NULL;

    if (!entry) {
        return NULL;
    }
    skill = cJSON_CreateObject();
    cap_groups = cJSON_CreateArray();
    if (!skill || !cap_groups || !cJSON_AddStringToObject(skill, "id", entry->id ? entry->id : "") ||
            !cJSON_AddStringToObject(skill, "summary", entry->summary ? entry->summary : "")) {
        goto fail;
    }
    if (!cJSON_AddStringToObject(skill, "file", entry->file ? entry->file : "") ||
            !cJSON_AddStringToObject(skill, "manage_mode", cap_skill_manage_mode_to_string(entry->manage_mode))) {
        goto fail;
    }
    for (size_t i = 0; i < entry->cap_group_count; i++) {
        cJSON *group = cJSON_CreateString(entry->cap_groups[i]);
        if (!group || !cJSON_AddItemToArray(cap_groups, group)) {
            cJSON_Delete(group);
            goto fail;
        }
    }
    if (!cJSON_AddItemToObject(skill, "cap_groups", cap_groups)) {
        goto fail;
    }
    return skill;

fail:
    cJSON_Delete(cap_groups);
    cJSON_Delete(skill);
    return NULL;
}

static esp_err_t cap_skill_append_catalog_entry(const claw_skill_catalog_entry_t *entry, void *user_ctx)
{
    cJSON *skill = cap_skill_catalog_entry_to_json(entry);
    if (!skill) {
        return ESP_ERR_NO_MEM;
    }
    if (!cJSON_AddItemToArray((cJSON *)user_ctx, skill)) {
        cJSON_Delete(skill);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t cap_skill_list_execute(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    cJSON *root = NULL;
    cJSON *skills = NULL;
    esp_err_t err;

    (void)input_json;
    (void)ctx;
    skills = cJSON_CreateArray();
    if (!skills) {
        return ESP_ERR_NO_MEM;
    }
    err = claw_skill_foreach_catalog_entry(cap_skill_append_catalog_entry, skills);
    if (err != ESP_OK) {
        cJSON_Delete(skills);
        return err;
    }
    root = cJSON_CreateObject();
    if (!root || !cJSON_AddBoolToObject(root, "ok", true) || !cJSON_AddItemToObject(root, "skills", skills)) {
        cJSON_Delete(root);
        cJSON_Delete(skills);
        return ESP_ERR_NO_MEM;
    }
    err = cap_skill_render_result(root, output, output_size);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_skill_activate_execute(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    char skill_id[CLAW_SKILL_ID_MAX_LEN + 1] = {0};
    char *document = NULL;
    const char *prefix = "<skill_content name=\"";
    const char *middle = "\">\n";
    const char *suffix = "\n</skill_content>";
    size_t wrapper_len;
    size_t document_size;
    esp_err_t err;

    if (!ctx || !ctx->session_id || !ctx->session_id[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    err = cap_skill_parse_id(input_json, skill_id, output, output_size);
    if (err != ESP_OK) {
        return err;
    }
    wrapper_len = strlen(prefix) + strlen(skill_id) + strlen(middle) + strlen(suffix);
    if (wrapper_len >= output_size) {
        cap_skill_write_error(output, output_size, "result_too_large", "skill content result too large", skill_id);
        return ESP_ERR_INVALID_SIZE;
    }
    document_size = output_size - wrapper_len;
    document = calloc(1, document_size);
    if (!document) {
        cap_skill_write_error(output, output_size, "out_of_memory", "out of memory", skill_id);
        return ESP_ERR_NO_MEM;
    }
    err = claw_skill_activate_for_session(ctx->session_id, skill_id, document, document_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "activate %s failed: %s", skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, err == ESP_ERR_NOT_FOUND ? "skill_not_found" : "activation_failed", "failed to activate skill", skill_id);
        free(document);
        return err;
    }
    err = cap_skill_sync_session_visible_groups(ctx->session_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sync visibility for %s failed: %s", skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, "visibility_sync_failed", "failed to sync capability visibility", skill_id);
        free(document);
        return err;
    }
    int written = snprintf(output, output_size, "%s%s%s%s%s", prefix, skill_id, middle, document, suffix);
    free(document);
    if (written < 0 || (size_t)written >= output_size) {
        cap_skill_write_error(output, output_size, "result_too_large", "skill content result too large", skill_id);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t cap_skill_write_success(const char *skill_id, char *output, size_t output_size)
{
    int written;

    if (!skill_id || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    written = snprintf(output, output_size, "{\"ok\":true,\"skill_id\":\"%s\"}", skill_id);
    return written >= 0 && (size_t)written < output_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t cap_skill_publish_execute_inner(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    char skill_id[CLAW_SKILL_ID_MAX_LEN + 1] = {0};
    esp_err_t err;

    (void)ctx;
    err = cap_skill_parse_id(input_json, skill_id, output, output_size);
    if (err != ESP_OK) {
        return err;
    }
    err = cap_skill_write_success(skill_id, output, output_size);
    if (err != ESP_OK) {
        return err;
    }
    err = claw_skill_publish(skill_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "publish %s failed: %s", skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, err == ESP_ERR_NOT_FOUND ? "skill_not_found" :
                              err == ESP_ERR_INVALID_STATE ? "readonly_skill" : "publish_failed",
                              "failed to publish runtime skill", skill_id);
        return err;
    }
    return ESP_OK;
}

static esp_err_t cap_skill_remove_execute_inner(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    char skill_id[CLAW_SKILL_ID_MAX_LEN + 1] = {0};
    esp_err_t err;

    (void)ctx;
    err = cap_skill_parse_id(input_json, skill_id, output, output_size);
    if (err != ESP_OK) {
        return err;
    }
    err = cap_skill_write_success(skill_id, output, output_size);
    if (err != ESP_OK) {
        return err;
    }
    err = claw_skill_remove(skill_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remove %s failed: %s", skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, err == ESP_ERR_NOT_FOUND ? "skill_not_found" :
                              err == ESP_ERR_INVALID_STATE ? "readonly_skill" : "remove_failed",
                              "failed to remove runtime skill", skill_id);
        return err;
    }
    return ESP_OK;
}

typedef esp_err_t (*cap_skill_mutation_execute_fn)(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size);

static esp_err_t cap_skill_execute_mutation_locked(cap_skill_mutation_execute_fn execute, const char *input_json,
                                                   const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    if (!execute || !s_skill_mutation_lock) {
        cap_skill_write_error(output, output_size, "not_initialized", "skill manager is not initialized", NULL);
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_skill_mutation_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        cap_skill_write_error(output, output_size, "busy", "another skill update is in progress", NULL);
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = execute(input_json, ctx, output, output_size);
    xSemaphoreGive(s_skill_mutation_lock);
    return err;
}

static esp_err_t cap_skill_publish_execute(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    return cap_skill_execute_mutation_locked(cap_skill_publish_execute_inner, input_json, ctx, output, output_size);
}

static esp_err_t cap_skill_remove_execute(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    return cap_skill_execute_mutation_locked(cap_skill_remove_execute_inner, input_json, ctx, output, output_size);
}

static const claw_cap_descriptor_t s_skill_core_descriptors[] = {
    {
        .id = "list_skill",
        .name = "list_skill",
        .family = "skill",
        .description = "List the current skill catalog.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        /* The catalog is already injected into prompt context. */
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{}}",
        .execute = cap_skill_list_execute,
    },
    {
        .id = "activate_skill",
        .name = "activate_skill",
        .family = "skill",
        .description = "Load a skill's instructions and enable its capability groups for the current session.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_SKILL_ID_SCHEMA,
        .execute = cap_skill_activate_execute,
    },
};

static const claw_cap_descriptor_t s_skill_manage_descriptors[] = {
    {
        .id = "publish_skill",
        .name = "publish_skill",
        .family = "skill",
        .description = "Validate and publish an existing runtime skill after its files have been created or updated.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_SKILL_ID_SCHEMA,
        .execute = cap_skill_publish_execute,
    },
    {
        .id = "remove_skill",
        .name = "remove_skill",
        .family = "skill",
        .description = "Recursively remove a runtime skill directory from writable storage and refresh the skill registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_SKILL_ID_SCHEMA,
        .execute = cap_skill_remove_execute,
    },
};

static const claw_cap_group_t s_skill_core_group = {
    .group_id = "cap_skill",
    .descriptors = s_skill_core_descriptors,
    .descriptor_count = sizeof(s_skill_core_descriptors) / sizeof(s_skill_core_descriptors[0]),
};

static const claw_cap_group_t s_skill_manage_group = {
    .group_id = "cap_skill_manage",
    .descriptors = s_skill_manage_descriptors,
    .descriptor_count = sizeof(s_skill_manage_descriptors) / sizeof(s_skill_manage_descriptors[0]),
};

esp_err_t cap_skill_mgr_register_group(void)
{
    esp_err_t err;

    if (!s_skill_mutation_lock) {
        s_skill_mutation_lock = xSemaphoreCreateMutex();
        if (!s_skill_mutation_lock) {
            ESP_LOGE(TAG, "failed to create mutation lock");
            return ESP_ERR_NO_MEM;
        }
    }
    if (!claw_cap_group_exists(s_skill_core_group.group_id)) {
        err = claw_cap_register_group(&s_skill_core_group);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", s_skill_core_group.group_id, esp_err_to_name(err));
            return err;
        }
    }
    if (!claw_cap_group_exists(s_skill_manage_group.group_id)) {
        /* Management tools stay registered but are exposed only by management skills. */
        err = claw_cap_register_group(&s_skill_manage_group);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", s_skill_manage_group.group_id, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
