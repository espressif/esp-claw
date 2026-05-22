/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_skill.h"
#include "cap_skill_mgr.h"
#include "esp_log.h"

static const char *TAG = "cap_skill_mgr";
static const char *CAP_SKILL_LIST = "list_skill";
static const char *CAP_SKILL_REGISTER = "register_skill";
static const char *CAP_SKILL_UNREGISTER = "unregister_skill";

#define CAP_SKILL_MAX_CATALOG_LEN 16384
#define CAP_SKILL_MAX_PATH_LEN    128

static const char *cap_skill_root_dir(void)
{
    return claw_skill_get_skills_root_dir();
}

static void cap_skill_free_string_array(char **items, size_t count)
{
    size_t i;

    if (!items) {
        return;
    }

    for (i = 0; i < count; i++) {
        free(items[i]);
    }
    free(items);
}

static esp_err_t cap_skill_sync_session_visible_groups(const char *session_id)
{
    char **group_ids = NULL;
    size_t group_count = 0;
    esp_err_t err = ESP_OK;

    if (!session_id || !session_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_skill_load_active_cap_groups(session_id, &group_ids, &group_count);
    if (err == ESP_ERR_NOT_FOUND) {
        return claw_cap_set_session_llm_visible_groups(session_id, NULL, 0);
    }
    if (err != ESP_OK) {
        return err;
    }

    err = claw_cap_set_session_llm_visible_groups(session_id,
                                                  (const char *const *)group_ids,
                                                  group_count);
    cap_skill_free_string_array(group_ids, group_count);
    return err;
}

static void cap_skill_write_error(char *output,
                                  size_t output_size,
                                  const char *error,
                                  const char *skill_id)
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
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error ? error : "unknown error");
        return;
    }

    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error ? error : "unknown error");
    if (skill_id && skill_id[0]) {
        cJSON_AddStringToObject(root, "skill_id", skill_id);
    }

    rendered = cJSON_PrintUnformatted(root);
    if (rendered) {
        snprintf(output, output_size, "%s", rendered);
        free(rendered);
    } else {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error ? error : "unknown error");
    }
    cJSON_Delete(root);
}

static esp_err_t cap_skill_read_file_dup(const char *path, char **out_text)
{
    FILE *file = NULL;
    long size;
    char *text = NULL;
    size_t read_bytes;

    if (!path || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;

    file = fopen(path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0 || size > CAP_SKILL_MAX_CATALOG_LEN) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    text = calloc(1, (size_t)size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    read_bytes = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[read_bytes] = '\0';
    *out_text = text;
    return ESP_OK;
}

static esp_err_t cap_skill_write_file_text(const char *path, const char *text)
{
    FILE *file = NULL;

    if (!path || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    if (fputs(text, file) < 0) {
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    return ESP_OK;
}

static bool cap_skill_path_is_valid(const char *skill_id, const char *path)
{
    char expected[CAP_SKILL_MAX_PATH_LEN];

    if (!skill_id || !skill_id[0] || !path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || strstr(path, "..") != NULL || strchr(path, '\\') != NULL || strchr(skill_id, '/') || strchr(skill_id, '\\')) {
        return false;
    }
    if (snprintf(expected, sizeof(expected), "%s/SKILL.md", skill_id) >= (int)sizeof(expected)) {
        return false;
    }
    return strcmp(path, expected) == 0;
}

static bool cap_skill_file_exists(const char *path)
{
    struct stat st = {0};

    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static esp_err_t cap_skill_load_catalog_json(char **out_text, cJSON **out_catalog)
{
    char *catalog_text = NULL;
    cJSON *catalog = NULL;
    esp_err_t err;

    if (!out_text || !out_catalog) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;
    *out_catalog = NULL;

    catalog_text = calloc(1, CAP_SKILL_MAX_CATALOG_LEN);
    if (!catalog_text) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_skill_render_catalog_json(catalog_text, CAP_SKILL_MAX_CATALOG_LEN);
    if (err != ESP_OK) {
        free(catalog_text);
        return err;
    }

    catalog = cJSON_Parse(catalog_text);
    if (!catalog || !cJSON_IsObject(catalog)) {
        cJSON_Delete(catalog);
        free(catalog_text);
        return ESP_ERR_INVALID_STATE;
    }

    *out_text = catalog_text;
    *out_catalog = catalog;
    return ESP_OK;
}

static const char *cap_skill_manage_mode_to_string(claw_skill_manage_mode_t mode)
{
    switch (mode) {
    case CLAW_SKILL_MANAGE_MODE_READONLY:
        return "readonly";
    case CLAW_SKILL_MANAGE_MODE_RUNTIME:
        return "runtime";
    default:
        return "unknown";
    }
}

static cJSON *cap_skill_catalog_entry_to_json(const claw_skill_catalog_entry_t *entry)
{
    cJSON *skill = NULL;
    cJSON *cap_groups = NULL;
    size_t i;

    if (!entry) {
        return NULL;
    }

    skill = cJSON_CreateObject();
    cap_groups = cJSON_CreateArray();
    if (!skill || !cap_groups) {
        cJSON_Delete(skill);
        cJSON_Delete(cap_groups);
        return NULL;
    }

    cJSON_AddStringToObject(skill, "id", entry->id ? entry->id : "");
    cJSON_AddStringToObject(skill, "file", entry->file ? entry->file : "");
    cJSON_AddStringToObject(skill, "summary", entry->summary ? entry->summary : "");
    cJSON_AddStringToObject(skill, "manage_mode", cap_skill_manage_mode_to_string(entry->manage_mode));
    for (i = 0; i < entry->cap_group_count; i++) {
        cJSON_AddItemToArray(cap_groups, cJSON_CreateString(entry->cap_groups[i]));
    }
    cJSON_AddItemToObject(skill, "cap_groups", cap_groups);
    return skill;
}

static esp_err_t cap_skill_build_catalog_result(const char *action,
                                                cJSON *skill,
                                                const char *skill_id,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = NULL;
    cJSON *catalog = NULL;
    cJSON *skills = NULL;
    char *catalog_text = NULL;
    char *rendered = NULL;
    esp_err_t err;

    if (!action || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_skill_load_catalog_json(&catalog_text, &catalog);
    if (err != ESP_OK) {
        return err;
    }
    free(catalog_text);

    skills = cJSON_DetachItemFromObjectCaseSensitive(catalog, "skills");
    cJSON_Delete(catalog);
    if (!cJSON_IsArray(skills)) {
        cJSON_Delete(skills);
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(skills);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", action);
    if (skill) {
        cJSON_AddItemToObject(root, "skill", skill);
    } else if (skill_id && skill_id[0]) {
        cJSON_AddStringToObject(root, "skill_id", skill_id);
    }
    cJSON_AddItemToObject(root, "skills", skills);

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_skill_activate_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    char *doc_text = NULL;
    char activated_skill_id[64] = {0};
    const char *prefix = "<skill_content name=\"";
    const char *middle = "\">\n";
    const char *suffix = "\n</skill_content>";
    size_t content_len;
    int written;
    esp_err_t err = ESP_OK;

    if (!ctx || !ctx->session_id || !ctx->session_id[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid input json\"}");
        return ESP_ERR_INVALID_ARG;
    }
    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");

    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0]) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"skill_id is required\"}");
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (strlen(skill_id_item->valuestring) >= sizeof(activated_skill_id)) {
        cap_skill_write_error(output, output_size, "skill_id is too long", NULL);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    snprintf(activated_skill_id, sizeof(activated_skill_id), "%s", skill_id_item->valuestring);

    doc_text = calloc(1, output_size);
    if (!doc_text) {
        cap_skill_write_error(output, output_size, "out of memory", NULL);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = claw_skill_read_document(activated_skill_id, doc_text, output_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read skill doc %s: %s",
                 activated_skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, "failed to read skill document", activated_skill_id);
        goto cleanup;
    }

    content_len = strlen(prefix) + strlen(activated_skill_id) + strlen(middle) +
                  strlen(doc_text) + strlen(suffix);
    if (content_len >= output_size) {
        ESP_LOGE(TAG, "skill content result too large: %u >= %u",
                 (unsigned)content_len, (unsigned)output_size);
        cap_skill_write_error(output, output_size, "skill content result too large", activated_skill_id);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

    err = claw_skill_activate_for_session(ctx->session_id, activated_skill_id);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "failed to activate skill", activated_skill_id);
        goto cleanup;
    }

    err = cap_skill_sync_session_visible_groups(ctx->session_id);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "failed to sync capability visibility", activated_skill_id);
        goto cleanup;
    }

    written = snprintf(output, output_size, "%s%s%s%s%s",
                       prefix, activated_skill_id, middle, doc_text, suffix);
    if (written < 0 || (size_t)written >= output_size) {
        cap_skill_write_error(output, output_size, "skill content result too large", activated_skill_id);
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }

cleanup:
    cJSON_Delete(root);
    free(doc_text);
    return err;
}

static esp_err_t cap_skill_list_execute(const char *input_json,
                                        const claw_cap_call_context_t *ctx,
                                        char *output,
                                        size_t output_size)
{
    (void)input_json;
    (void)ctx;

    return cap_skill_build_catalog_result(CAP_SKILL_LIST, NULL, NULL, output, output_size);
}

static esp_err_t cap_skill_register_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    cJSON *file_item = NULL;
    cJSON *skill = NULL;
    claw_skill_catalog_entry_t entry;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        cap_skill_write_error(output, output_size, "invalid input json", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");
    file_item = cJSON_GetObjectItemCaseSensitive(root, "file");
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0] ||
            !cJSON_IsString(file_item) || !file_item->valuestring || !file_item->valuestring[0]) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill_id and file are required", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    if (!cap_skill_path_is_valid(skill_id_item->valuestring, file_item->valuestring)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "file must be <skill_id>/SKILL.md", skill_id_item->valuestring);
        return ESP_ERR_INVALID_ARG;
    }

    {
        const char *root_dir = cap_skill_root_dir();
        if (!root_dir) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "skill storage is not initialized", skill_id_item->valuestring);
            return ESP_ERR_INVALID_STATE;
        }
        if (snprintf(skill_path, sizeof(skill_path), "%s/%s", root_dir, file_item->valuestring) >= (int)sizeof(skill_path)) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "file path is too long", skill_id_item->valuestring);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (!cap_skill_file_exists(skill_path)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill markdown file does not exist", skill_id_item->valuestring);
        return ESP_ERR_NOT_FOUND;
    }

    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to reload skill registry", skill_id_item->valuestring);
        return err;
    }

    err = claw_skill_get_catalog_entry(skill_id_item->valuestring, &entry);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill not found after registry reload", skill_id_item->valuestring);
        return err;
    }
    if (!entry.file || strcmp(entry.file, file_item->valuestring) != 0) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "registered skill file does not match requested file", skill_id_item->valuestring);
        return ESP_ERR_INVALID_STATE;
    }

    skill = cap_skill_catalog_entry_to_json(&entry);
    if (!skill) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "out of memory", skill_id_item->valuestring);
        return ESP_ERR_NO_MEM;
    }

    cJSON_Delete(root);
    return cap_skill_build_catalog_result(CAP_SKILL_REGISTER, skill, NULL, output, output_size);
}

static esp_err_t cap_skill_unregister_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    char *old_markdown = NULL;
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    const char *skill_id = NULL;
    claw_skill_catalog_entry_t entry;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    skill_id_item = root ? cJSON_GetObjectItemCaseSensitive(root, "skill_id") : NULL;
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0]) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill_id is required", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    skill_id = skill_id_item->valuestring;
    err = claw_skill_get_catalog_entry(skill_id, &entry);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill not found", skill_id);
        return err;
    }
    {
        const char *root_dir = cap_skill_root_dir();
        if (!root_dir) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "skill storage is not initialized", skill_id);
            return ESP_ERR_INVALID_STATE;
        }
        if (snprintf(skill_path, sizeof(skill_path), "%s/%s", root_dir, entry.file) >= (int)sizeof(skill_path)) {
            cJSON_Delete(root);
            cap_skill_write_error(output, output_size, "file path is too long", skill_id);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    err = cap_skill_read_file_dup(skill_path, &old_markdown);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to read skill markdown", skill_id);
        return err;
    }
    if (remove(skill_path) != 0) {
        free(old_markdown);
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to delete skill markdown", skill_id);
        return ESP_FAIL;
    }

    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        if (cap_skill_write_file_text(skill_path, old_markdown) == ESP_OK) {
            (void)claw_skill_reload_registry();
        }
        free(old_markdown);
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "failed to reload skill registry", skill_id);
        return err;
    }

    free(old_markdown);
    cJSON_Delete(root);
    return cap_skill_build_catalog_result(CAP_SKILL_UNREGISTER, NULL, skill_id, output, output_size);
}

static const claw_cap_descriptor_t s_skill_descriptors[] = {
    {
        .id = "list_skill",
        .name = "list_skill",
        .family = "skill",
        .description = "List all skills discovered from markdown files under the skills root directory.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        /* The skills catalog is already injected into prompt context, so keep this for non-LLM callers only. */
        .cap_flags = 0,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_skill_list_execute,
    },
    {
        .id = "register_skill",
        .name = "register_skill",
        .family = "skill",
        .description = "Register or refresh an existing source-file skill markdown file and reload the in-memory skill registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_id\":{\"type\":\"string\"},"
        "\"file\":{\"type\":\"string\",\"pattern\":\"^[^/]+/SKILL\\\\.md$\"}},"
        "\"required\":[\"skill_id\",\"file\"]}",
        .execute = cap_skill_register_execute,
    },
    {
        .id = "unregister_skill",
        .name = "unregister_skill",
        .family = "skill",
        .description = "Delete one source-file skill markdown file and reload the in-memory skill registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_id\":{\"type\":\"string\"}},\"required\":[\"skill_id\"]}",
        .execute = cap_skill_unregister_execute,
    },
    {
        .id = "activate_skill",
        .name = "activate_skill",
        .family = "skill",
        .description = "Activate a skill from skill_id and return its full Skill markdown document "
                       "inside a <skill_content name=\"skill_id\"> block. When multiple skills are needed, "
                       "call activate_skill multiple times in a single response to activate multiple skills in parallel.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"skill_id\":{\"type\":\"string\"}},\"required\":[\"skill_id\"]}",
        .execute = cap_skill_activate_execute,
    },
};

static const claw_cap_group_t s_skill_group = {
    .group_id = "cap_skill",
    .descriptors = s_skill_descriptors,
    .descriptor_count = sizeof(s_skill_descriptors) / sizeof(s_skill_descriptors[0]),
};

esp_err_t cap_skill_mgr_register_group(void)
{
    if (claw_cap_group_exists(s_skill_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_skill_group);
}
