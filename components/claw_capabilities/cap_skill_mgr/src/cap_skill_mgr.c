/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_skill.h"
#include "cap_skill_mgr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cap_skill_mgr";
static const char *CAP_SKILL_LIST = "list_skill";
static const char *CAP_SKILL_PUBLISH = "publish_skill";
static const char *CAP_SKILL_REGISTER = "register_skill";
static const char *CAP_SKILL_UNREGISTER = "unregister_skill";
static const char *CAP_SKILL_SET_WORK = "set_skill_work";
static const char *CAP_SKILL_REMOVE_WORK = "remove_skill_work";

#define CAP_SKILL_MAX_CATALOG_LEN 16384
#define CAP_SKILL_MAX_PATH_LEN    128
#define CAP_SKILL_WORK_SCHEMA_VERSION 1
#define CAP_SKILL_LAUNCHER_FILENAME "launcher.json"
#define CAP_SKILL_WORK_TEMP_SUFFIX ".tmp"
#define CAP_SKILL_WORK_BACKUP_SUFFIX ".bak"

static char s_skill_root_dir[CAP_SKILL_MAX_PATH_LEN];
static SemaphoreHandle_t s_skill_mutation_lock;

static const char *cap_skill_root_dir(void)
{
    return s_skill_root_dir[0] ? s_skill_root_dir : NULL;
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
    bool failed = false;

    if (!path || !text) {
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }
    if (fputs(text, file) < 0 || fflush(file) != 0) {
        failed = true;
    }
    if (fclose(file) != 0) {
        failed = true;
    }
    return failed ? ESP_FAIL : ESP_OK;
}

static bool cap_skill_string_has_suffix(const char *value, const char *suffix)
{
    size_t value_len;
    size_t suffix_len;

    if (!value || !suffix) {
        return false;
    }
    value_len = strlen(value);
    suffix_len = strlen(suffix);
    return value_len >= suffix_len &&
           strcmp(value + value_len - suffix_len, suffix) == 0;
}

static bool cap_skill_payload_path_is_valid(const char *path,
                                            const char *required_suffix)
{
    if (!path || !path[0] || !required_suffix || !required_suffix[0]) {
        return false;
    }
    if (path[0] == '/' || strstr(path, "..") != NULL ||
            strchr(path, '\\') != NULL) {
        return false;
    }
    return cap_skill_string_has_suffix(path, required_suffix);
}

static bool cap_skill_icon_path_is_valid(const char *path)
{
    return cap_skill_payload_path_is_valid(path, ".jpg") ||
           cap_skill_payload_path_is_valid(path, ".jpeg");
}

static esp_err_t cap_skill_remove_file_if_exists(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (remove(path) == 0 || errno == ENOENT) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t cap_skill_build_transaction_paths(
    const char *path,
    char *temp_path,
    size_t temp_path_size,
    char *backup_path,
    size_t backup_path_size)
{
    if (!path || !temp_path || !backup_path ||
            snprintf(temp_path, temp_path_size, "%s%s", path,
                     CAP_SKILL_WORK_TEMP_SUFFIX) >= (int)temp_path_size ||
            snprintf(backup_path, backup_path_size, "%s%s", path,
                     CAP_SKILL_WORK_BACKUP_SUFFIX) >= (int)backup_path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* Begin a recoverable update. A NULL replacement removes the target. The
 * previous file, when present, remains at <path>.bak until commit. */
static esp_err_t cap_skill_begin_file_update(const char *path,
                                             const char *replacement,
                                             bool *out_had_previous)
{
    char temp_path[CAP_SKILL_MAX_PATH_LEN];
    char backup_path[CAP_SKILL_MAX_PATH_LEN];
    struct stat st = {0};
    bool target_exists = false;
    esp_err_t err;

    if (!path || !out_had_previous) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_had_previous = false;

    err = cap_skill_build_transaction_paths(path, temp_path, sizeof(temp_path),
                                            backup_path, sizeof(backup_path));
    if (err != ESP_OK) {
        return err;
    }
    err = cap_skill_remove_file_if_exists(temp_path);
    if (err != ESP_OK) {
        return err;
    }

    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            return ESP_ERR_INVALID_STATE;
        }
        target_exists = true;
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }

    /* Recover a prior interrupted transaction before starting another one. */
    if (stat(backup_path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (target_exists) {
            err = cap_skill_remove_file_if_exists(backup_path);
            if (err != ESP_OK) {
                return err;
            }
        } else {
            if (rename(backup_path, path) != 0) {
                return ESP_FAIL;
            }
            target_exists = true;
        }
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }

    if (replacement) {
        err = cap_skill_write_file_text(temp_path, replacement);
        if (err != ESP_OK) {
            (void)cap_skill_remove_file_if_exists(temp_path);
            return err;
        }
    }

    if (target_exists && rename(path, backup_path) != 0) {
        (void)cap_skill_remove_file_if_exists(temp_path);
        return ESP_FAIL;
    }

    if (replacement && rename(temp_path, path) != 0) {
        if (target_exists) {
            (void)rename(backup_path, path);
        }
        (void)cap_skill_remove_file_if_exists(temp_path);
        return ESP_FAIL;
    }

    *out_had_previous = target_exists;
    return ESP_OK;
}

static esp_err_t cap_skill_finish_file_update(const char *path,
                                              bool had_previous,
                                              bool commit)
{
    char temp_path[CAP_SKILL_MAX_PATH_LEN];
    char backup_path[CAP_SKILL_MAX_PATH_LEN];
    esp_err_t err;

    err = cap_skill_build_transaction_paths(path, temp_path, sizeof(temp_path),
                                            backup_path, sizeof(backup_path));
    if (err != ESP_OK) {
        return err;
    }
    (void)cap_skill_remove_file_if_exists(temp_path);

    if (commit) {
        return had_previous ? cap_skill_remove_file_if_exists(backup_path) : ESP_OK;
    }

    err = cap_skill_remove_file_if_exists(path);
    if (err != ESP_OK) {
        return err;
    }
    if (had_previous && rename(backup_path, path) != 0) {
        return ESP_FAIL;
    }
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

static bool cap_skill_id_is_valid(const char *skill_id)
{
    return skill_id && skill_id[0] && skill_id[0] != '/' &&
           strstr(skill_id, "..") == NULL &&
           strchr(skill_id, '/') == NULL && strchr(skill_id, '\\') == NULL;
}

static bool cap_skill_work_input_key_is_allowed(const char *key)
{
    static const char *const keys[] = {
        "skill_id", "entry", "icon", "args", "exclusive",
        "order", "visible", "replace",
    };

    if (!key) {
        return false;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcmp(key, keys[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool cap_skill_publish_input_key_is_allowed(const char *key)
{
    return key && (strcmp(key, "skill_id") == 0 ||
                   strcmp(key, "file") == 0 ||
                   strcmp(key, "launcher") == 0);
}

static bool cap_skill_launcher_input_key_is_allowed(const char *key)
{
    return key && strcmp(key, "skill_id") != 0 &&
           cap_skill_work_input_key_is_allowed(key);
}

static esp_err_t cap_skill_resolve_runtime_paths(const char *skill_id,
                                                 bool require_registered,
                                                 char *skill_dir,
                                                 size_t skill_dir_size,
                                                 char *work_path,
                                                 size_t work_path_size,
                                                 char *output,
                                                 size_t output_size)
{
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    claw_skill_catalog_entry_t entry;
    const char *root_dir = cap_skill_root_dir();
    esp_err_t err;

    if (!cap_skill_id_is_valid(skill_id)) {
        cap_skill_write_error(output, output_size, "invalid skill_id", skill_id);
        return ESP_ERR_INVALID_ARG;
    }
    if (!root_dir) {
        cap_skill_write_error(output, output_size, "skill storage is not initialized", skill_id);
        return ESP_ERR_INVALID_STATE;
    }

    if (snprintf(skill_dir, skill_dir_size, "%s/%s", root_dir, skill_id) >=
            (int)skill_dir_size ||
            snprintf(skill_path, sizeof(skill_path), "%s/SKILL.md", skill_dir) >=
            (int)sizeof(skill_path) ||
            snprintf(work_path, work_path_size, "%s/%s", skill_dir,
                     CAP_SKILL_LAUNCHER_FILENAME) >= (int)work_path_size) {
        cap_skill_write_error(output, output_size, "skill path is too long", skill_id);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!cap_skill_file_exists(skill_path)) {
        cap_skill_write_error(output, output_size,
                              "skill markdown file does not exist in writable skill storage",
                              skill_id);
        return ESP_ERR_NOT_FOUND;
    }
    if (!require_registered) {
        return ESP_OK;
    }

    err = claw_skill_get_catalog_entry(skill_id, &entry);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size,
                              "skill must be published before low-level Work management",
                              skill_id);
        return err;
    }
    if (entry.manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME) {
        cap_skill_write_error(output, output_size, "skill is readonly", skill_id);
        return ESP_ERR_INVALID_STATE;
    }
    if (!entry.skill_dir || strcmp(entry.skill_dir, skill_dir) != 0) {
        cap_skill_write_error(output, output_size,
                              "skill is not stored in writable skill storage", skill_id);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t cap_skill_rollback_file_update(const char *work_path,
                                                bool had_previous,
                                                bool reload_registry)
{
    esp_err_t restore_err = cap_skill_finish_file_update(work_path,
                                                         had_previous, false);

    if (restore_err != ESP_OK || !reload_registry) {
        return restore_err;
    }
    return claw_skill_reload_registry();
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
    cJSON *execution = NULL;
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
    cJSON_AddBoolToObject(skill, "is_work",
                         entry->execution && entry->execution->visible);
    for (i = 0; i < entry->cap_group_count; i++) {
        cJSON_AddItemToArray(cap_groups, cJSON_CreateString(entry->cap_groups[i]));
    }
    cJSON_AddItemToObject(skill, "cap_groups", cap_groups);
    if (entry->execution) {
        execution = cJSON_CreateObject();
        if (!execution) {
            cJSON_Delete(skill);
            return NULL;
        }
        cJSON_AddStringToObject(execution, "entry",
                               entry->execution->entry ? entry->execution->entry : "");
        if (entry->execution->icon) {
            cJSON_AddStringToObject(execution, "icon", entry->execution->icon);
        }
        if (entry->execution->args_json) {
            cJSON *args = cJSON_Parse(entry->execution->args_json);
            if (!args) {
                cJSON_Delete(execution);
                cJSON_Delete(skill);
                return NULL;
            }
            cJSON_AddItemToObject(execution, "args", args);
        }
        if (entry->execution->exclusive) {
            cJSON_AddStringToObject(execution, "exclusive",
                                   entry->execution->exclusive);
        }
        cJSON_AddBoolToObject(execution, "replace", entry->execution->replace);
        cJSON_AddNumberToObject(execution, "order", entry->execution->order);
        cJSON_AddBoolToObject(execution, "visible", entry->execution->visible);
        cJSON_AddItemToObject(skill, "execution", execution);
    }
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
        /* `skill` is owned by this function until adopted into `root` below;
         * release it on every early-error path so it cannot leak. */
        cJSON_Delete(skill);
        return err;
    }
    free(catalog_text);

    skills = cJSON_DetachItemFromObjectCaseSensitive(catalog, "skills");
    cJSON_Delete(catalog);
    if (!cJSON_IsArray(skills)) {
        cJSON_Delete(skills);
        cJSON_Delete(skill);
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(skills);
        cJSON_Delete(skill);
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

    root = cJSON_ParseWithOpts(input_json ? input_json : "{}", NULL, true);
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

static esp_err_t cap_skill_register_execute_inner(const char *input_json,
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

    root = cJSON_ParseWithOpts(input_json ? input_json : "{}", NULL, true);
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

static esp_err_t cap_skill_set_work_execute_common(const char *input_json,
                                                   const claw_cap_call_context_t *ctx,
                                                   char *output,
                                                   size_t output_size,
                                                   bool require_registered)
{
    char skill_dir[CAP_SKILL_MAX_PATH_LEN];
    char work_path[CAP_SKILL_MAX_PATH_LEN];
    char payload_path[CAP_SKILL_MAX_PATH_LEN];
    cJSON *root = NULL;
    cJSON *field = NULL;
    cJSON *skill_id_item = NULL;
    cJSON *entry_item = NULL;
    cJSON *icon_item = NULL;
    cJSON *args_item = NULL;
    cJSON *exclusive_item = NULL;
    cJSON *order_item = NULL;
    cJSON *visible_item = NULL;
    cJSON *replace_item = NULL;
    cJSON *work = NULL;
    cJSON *skill = NULL;
    char *work_text = NULL;
    bool had_previous = false;
    claw_skill_catalog_entry_t catalog_entry;
    esp_err_t err;
    esp_err_t rollback_err;

    (void)ctx;

    root = cJSON_ParseWithOpts(input_json ? input_json : "{}", NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "invalid input json", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_ArrayForEach(field, root) {
        if (!cap_skill_work_input_key_is_allowed(field->string)) {
            cap_skill_write_error(output, output_size, "unknown Work field", NULL);
            err = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }

    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");
    entry_item = cJSON_GetObjectItemCaseSensitive(root, "entry");
    icon_item = cJSON_GetObjectItemCaseSensitive(root, "icon");
    args_item = cJSON_GetObjectItemCaseSensitive(root, "args");
    exclusive_item = cJSON_GetObjectItemCaseSensitive(root, "exclusive");
    order_item = cJSON_GetObjectItemCaseSensitive(root, "order");
    visible_item = cJSON_GetObjectItemCaseSensitive(root, "visible");
    replace_item = cJSON_GetObjectItemCaseSensitive(root, "replace");

    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring ||
            !skill_id_item->valuestring[0] ||
            !cJSON_IsString(entry_item) || !entry_item->valuestring ||
            !cap_skill_payload_path_is_valid(entry_item->valuestring, ".lua")) {
        cap_skill_write_error(output, output_size,
                              "skill_id and a valid relative .lua entry are required",
                              cJSON_IsString(skill_id_item) ? skill_id_item->valuestring : NULL);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (icon_item && (!cJSON_IsString(icon_item) || !icon_item->valuestring ||
            !cap_skill_icon_path_is_valid(icon_item->valuestring))) {
        cap_skill_write_error(output, output_size, "icon must be a valid relative .jpg or .jpeg path",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (args_item && !cJSON_IsObject(args_item)) {
        cap_skill_write_error(output, output_size, "args must be an object",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (exclusive_item && (!cJSON_IsString(exclusive_item) ||
            !exclusive_item->valuestring || !exclusive_item->valuestring[0] ||
            strlen(exclusive_item->valuestring) > CLAW_SKILL_EXECUTION_EXCLUSIVE_MAX)) {
        cap_skill_write_error(output, output_size, "exclusive must contain 1-31 characters",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (order_item && (!cJSON_IsNumber(order_item) ||
            order_item->valuedouble != (double)order_item->valueint)) {
        cap_skill_write_error(output, output_size, "order must be an integer",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (visible_item && !cJSON_IsBool(visible_item)) {
        cap_skill_write_error(output, output_size, "visible must be a boolean",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (replace_item && !cJSON_IsBool(replace_item)) {
        cap_skill_write_error(output, output_size, "replace must be a boolean",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    err = cap_skill_resolve_runtime_paths(skill_id_item->valuestring,
                                          require_registered,
                                          skill_dir, sizeof(skill_dir),
                                          work_path, sizeof(work_path),
                                          output, output_size);
    if (err != ESP_OK) {
        goto cleanup;
    }
    if (snprintf(payload_path, sizeof(payload_path), "%s/%s", skill_dir,
                 entry_item->valuestring) >= (int)sizeof(payload_path) ||
            !cap_skill_file_exists(payload_path)) {
        cap_skill_write_error(output, output_size, "Work entry file does not exist",
                              skill_id_item->valuestring);
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (icon_item) {
        if (snprintf(payload_path, sizeof(payload_path), "%s/%s", skill_dir,
                     icon_item->valuestring) >= (int)sizeof(payload_path) ||
                !cap_skill_file_exists(payload_path)) {
            cap_skill_write_error(output, output_size, "Work icon file does not exist",
                                  skill_id_item->valuestring);
            err = ESP_ERR_NOT_FOUND;
            goto cleanup;
        }
    }

    work = cJSON_CreateObject();
    if (!work ||
            !cJSON_AddNumberToObject(work, "schema_version",
                                     CAP_SKILL_WORK_SCHEMA_VERSION) ||
            !cJSON_AddStringToObject(work, "entry", entry_item->valuestring)) {
        cap_skill_write_error(output, output_size, "out of memory",
                              skill_id_item->valuestring);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    if (icon_item && !cJSON_AddStringToObject(work, "icon", icon_item->valuestring)) {
        err = ESP_ERR_NO_MEM;
        goto work_alloc_failed;
    }
    if (args_item) {
        cJSON *args_copy = cJSON_Duplicate(args_item, true);
        if (!args_copy || !cJSON_AddItemToObject(work, "args", args_copy)) {
            cJSON_Delete(args_copy);
            err = ESP_ERR_NO_MEM;
            goto work_alloc_failed;
        }
    }
    if (exclusive_item && !cJSON_AddStringToObject(work, "exclusive",
                                                    exclusive_item->valuestring)) {
        err = ESP_ERR_NO_MEM;
        goto work_alloc_failed;
    }
    if (order_item && !cJSON_AddNumberToObject(work, "order", order_item->valueint)) {
        err = ESP_ERR_NO_MEM;
        goto work_alloc_failed;
    }
    if (visible_item && !cJSON_AddBoolToObject(work, "visible",
                                               cJSON_IsTrue(visible_item))) {
        err = ESP_ERR_NO_MEM;
        goto work_alloc_failed;
    }
    if (replace_item && !cJSON_AddBoolToObject(work, "replace",
                                               cJSON_IsTrue(replace_item))) {
        err = ESP_ERR_NO_MEM;
        goto work_alloc_failed;
    }

    work_text = cJSON_Print(work);
    if (!work_text) {
        err = ESP_ERR_NO_MEM;
        goto work_alloc_failed;
    }

    err = cap_skill_begin_file_update(work_path, work_text, &had_previous);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "failed to write Work definition",
                              skill_id_item->valuestring);
        goto cleanup;
    }
    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        rollback_err = cap_skill_rollback_file_update(work_path, had_previous,
                                                      false);
        cap_skill_write_error(output, output_size,
                              rollback_err == ESP_OK ?
                              "failed to reload registry; previous Work restored" :
                              "failed to reload registry and restore previous Work",
                              skill_id_item->valuestring);
        goto cleanup;
    }

    err = claw_skill_get_catalog_entry(skill_id_item->valuestring, &catalog_entry);
    if (err != ESP_OK || !catalog_entry.execution ||
            catalog_entry.manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME ||
            !catalog_entry.skill_dir ||
            strcmp(catalog_entry.skill_dir, skill_dir) != 0) {
        rollback_err = cap_skill_rollback_file_update(work_path, had_previous,
                                                      true);
        cap_skill_write_error(output, output_size,
                              rollback_err == ESP_OK ?
                              "Work was not available after reload; previous Work restored" :
                              "Work was not available after reload and rollback failed",
                              skill_id_item->valuestring);
        err = err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
        goto cleanup;
    }

    err = cap_skill_finish_file_update(work_path, had_previous, true);
    if (err != ESP_OK) {
        rollback_err = cap_skill_rollback_file_update(work_path, had_previous,
                                                      true);
        cap_skill_write_error(output, output_size,
                              rollback_err == ESP_OK ?
                              "failed to commit Work update; previous Work restored" :
                              "failed to commit Work update and rollback failed",
                              skill_id_item->valuestring);
        goto cleanup;
    }

    err = claw_skill_get_catalog_entry(skill_id_item->valuestring, &catalog_entry);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "skill not found after Work update",
                              skill_id_item->valuestring);
        goto cleanup;
    }
    skill = cap_skill_catalog_entry_to_json(&catalog_entry);
    if (!skill) {
        cap_skill_write_error(output, output_size, "out of memory",
                              skill_id_item->valuestring);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = cap_skill_build_catalog_result(CAP_SKILL_SET_WORK, skill, NULL,
                                         output, output_size);
    skill = NULL;
    goto cleanup;

work_alloc_failed:
    cap_skill_write_error(output, output_size, "out of memory",
                          skill_id_item->valuestring);

cleanup:
    cJSON_Delete(skill);
    cJSON_Delete(work);
    cJSON_Delete(root);
    free(work_text);
    return err;
}

static esp_err_t cap_skill_set_work_execute_inner(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    return cap_skill_set_work_execute_common(input_json, ctx, output,
                                             output_size, true);
}

static esp_err_t cap_skill_remove_work_execute_common(const char *input_json,
                                                      const claw_cap_call_context_t *ctx,
                                                      char *output,
                                                      size_t output_size,
                                                      bool require_registered,
                                                      bool force_reload)
{
    char skill_dir[CAP_SKILL_MAX_PATH_LEN];
    char work_path[CAP_SKILL_MAX_PATH_LEN];
    cJSON *root = NULL;
    cJSON *field = NULL;
    cJSON *skill_id_item = NULL;
    cJSON *skill = NULL;
    bool had_previous = false;
    claw_skill_catalog_entry_t catalog_entry;
    esp_err_t err;
    esp_err_t rollback_err;

    (void)ctx;

    root = cJSON_ParseWithOpts(input_json ? input_json : "{}", NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "invalid input json", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_ArrayForEach(field, root) {
        if (!field->string || strcmp(field->string, "skill_id") != 0) {
            cap_skill_write_error(output, output_size, "unknown remove_skill_work field", NULL);
            err = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring ||
            !skill_id_item->valuestring[0]) {
        cap_skill_write_error(output, output_size, "skill_id is required", NULL);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    err = cap_skill_resolve_runtime_paths(skill_id_item->valuestring,
                                          require_registered,
                                          skill_dir, sizeof(skill_dir),
                                          work_path, sizeof(work_path),
                                          output, output_size);
    if (err != ESP_OK) {
        goto cleanup;
    }
    err = cap_skill_begin_file_update(work_path, NULL, &had_previous);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "failed to remove Work definition",
                              skill_id_item->valuestring);
        goto cleanup;
    }

    if (had_previous || force_reload) {
        err = claw_skill_reload_registry();
        if (err != ESP_OK) {
            rollback_err = cap_skill_rollback_file_update(work_path,
                                                          had_previous, false);
            cap_skill_write_error(output, output_size,
                                  rollback_err == ESP_OK ?
                                  "failed to reload registry; Work restored" :
                                  "failed to reload registry and restore Work",
                                  skill_id_item->valuestring);
            goto cleanup;
        }
    }

    err = claw_skill_get_catalog_entry(skill_id_item->valuestring, &catalog_entry);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size, "skill not found after Work removal",
                              skill_id_item->valuestring);
        goto cleanup;
    }
    if (catalog_entry.manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME ||
            !catalog_entry.skill_dir ||
            strcmp(catalog_entry.skill_dir, skill_dir) != 0) {
        rollback_err = cap_skill_rollback_file_update(work_path, had_previous,
                                                      true);
        cap_skill_write_error(output, output_size,
                              rollback_err == ESP_OK ?
                              "published Skill is not runtime-manageable; Work restored" :
                              "published Skill is not runtime-manageable and Work rollback failed",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    if (catalog_entry.execution) {
        rollback_err = cap_skill_rollback_file_update(work_path, had_previous,
                                                      true);
        cap_skill_write_error(output, output_size,
                              rollback_err == ESP_OK ?
                              "Work remains available after removal; Work restored" :
                              "Work remains available after removal and rollback failed",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    err = cap_skill_finish_file_update(work_path, had_previous, true);
    if (err != ESP_OK) {
        rollback_err = cap_skill_rollback_file_update(work_path, had_previous,
                                                      true);
        cap_skill_write_error(output, output_size,
                              rollback_err == ESP_OK ?
                              "failed to commit Work removal; Work restored" :
                              "failed to commit Work removal and rollback failed",
                              skill_id_item->valuestring);
        goto cleanup;
    }
    skill = cap_skill_catalog_entry_to_json(&catalog_entry);
    if (!skill) {
        cap_skill_write_error(output, output_size, "out of memory",
                              skill_id_item->valuestring);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = cap_skill_build_catalog_result(CAP_SKILL_REMOVE_WORK, skill, NULL,
                                         output, output_size);
    skill = NULL;

cleanup:
    cJSON_Delete(skill);
    cJSON_Delete(root);
    return err;
}

static esp_err_t cap_skill_remove_work_execute_inner(const char *input_json,
                                                     const claw_cap_call_context_t *ctx,
                                                     char *output,
                                                     size_t output_size)
{
    return cap_skill_remove_work_execute_common(input_json, ctx, output,
                                                output_size, true, false);
}

static esp_err_t cap_skill_publish_execute_inner(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    cJSON *root = NULL;
    cJSON *field = NULL;
    cJSON *skill_id_item = NULL;
    cJSON *file_item = NULL;
    cJSON *launcher_item = NULL;
    cJSON *call = NULL;
    cJSON *skill = NULL;
    char *call_text = NULL;
    claw_skill_catalog_entry_t entry;
    esp_err_t err = ESP_OK;

    root = cJSON_ParseWithOpts(input_json ? input_json : "{}", NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "invalid input json", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON_ArrayForEach(field, root) {
        if (!cap_skill_publish_input_key_is_allowed(field->string)) {
            cap_skill_write_error(output, output_size,
                                  "unknown publish_skill field", NULL);
            err = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }

    skill_id_item = cJSON_GetObjectItemCaseSensitive(root, "skill_id");
    file_item = cJSON_GetObjectItemCaseSensitive(root, "file");
    launcher_item = cJSON_GetObjectItemCaseSensitive(root, "launcher");
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring ||
            !cJSON_IsString(file_item) || !file_item->valuestring ||
            !cap_skill_path_is_valid(skill_id_item->valuestring,
                                     file_item->valuestring)) {
        cap_skill_write_error(output, output_size,
                              "skill_id and file=<skill_id>/SKILL.md are required",
                              cJSON_IsString(skill_id_item) ?
                              skill_id_item->valuestring : NULL);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (launcher_item && !cJSON_IsNull(launcher_item) &&
            !cJSON_IsObject(launcher_item)) {
        cap_skill_write_error(output, output_size,
                              "launcher must be an object or null",
                              skill_id_item->valuestring);
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }
    if (cJSON_IsObject(launcher_item)) {
        cJSON_ArrayForEach(field, launcher_item) {
            if (!cap_skill_launcher_input_key_is_allowed(field->string)) {
                cap_skill_write_error(output, output_size,
                                      "unknown launcher field",
                                      skill_id_item->valuestring);
                err = ESP_ERR_INVALID_ARG;
                goto cleanup;
            }
        }
    }

    if (cJSON_IsObject(launcher_item)) {
        call = cJSON_Duplicate(launcher_item, true);
        if (!call || !cJSON_AddStringToObject(call, "skill_id",
                                               skill_id_item->valuestring)) {
            cap_skill_write_error(output, output_size, "out of memory",
                                  skill_id_item->valuestring);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        call_text = cJSON_PrintUnformatted(call);
        if (!call_text) {
            cap_skill_write_error(output, output_size, "out of memory",
                                  skill_id_item->valuestring);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        err = cap_skill_set_work_execute_common(call_text, ctx, output,
                                                output_size, false);
    } else if (cJSON_IsNull(launcher_item)) {
        call = cJSON_CreateObject();
        if (!call || !cJSON_AddStringToObject(call, "skill_id",
                                               skill_id_item->valuestring)) {
            cap_skill_write_error(output, output_size, "out of memory",
                                  skill_id_item->valuestring);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        call_text = cJSON_PrintUnformatted(call);
        if (!call_text) {
            cap_skill_write_error(output, output_size, "out of memory",
                                  skill_id_item->valuestring);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        err = cap_skill_remove_work_execute_common(call_text, ctx, output,
                                                   output_size, false, true);
    } else {
        call = cJSON_CreateObject();
        if (!call ||
                !cJSON_AddStringToObject(call, "skill_id",
                                         skill_id_item->valuestring) ||
                !cJSON_AddStringToObject(call, "file",
                                         file_item->valuestring)) {
            cap_skill_write_error(output, output_size, "out of memory",
                                  skill_id_item->valuestring);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        call_text = cJSON_PrintUnformatted(call);
        if (!call_text) {
            cap_skill_write_error(output, output_size, "out of memory",
                                  skill_id_item->valuestring);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        err = cap_skill_register_execute_inner(call_text, ctx, output,
                                               output_size);
    }
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = claw_skill_get_catalog_entry(skill_id_item->valuestring, &entry);
    if (err != ESP_OK) {
        cap_skill_write_error(output, output_size,
                              "skill not found after publication",
                              skill_id_item->valuestring);
        goto cleanup;
    }
    skill = cap_skill_catalog_entry_to_json(&entry);
    if (!skill) {
        cap_skill_write_error(output, output_size, "out of memory",
                              skill_id_item->valuestring);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    err = cap_skill_build_catalog_result(CAP_SKILL_PUBLISH, skill, NULL,
                                         output, output_size);
    skill = NULL;

cleanup:
    cJSON_Delete(skill);
    cJSON_Delete(call);
    cJSON_Delete(root);
    free(call_text);
    return err;
}

static esp_err_t cap_skill_unregister_execute_inner(const char *input_json,
                                                    const claw_cap_call_context_t *ctx,
                                                    char *output,
                                                    size_t output_size)
{
    char skill_path[CAP_SKILL_MAX_PATH_LEN];
    char skill_id[CAP_SKILL_MAX_PATH_LEN];
    char *old_markdown = NULL;
    cJSON *root = NULL;
    cJSON *skill_id_item = NULL;
    claw_skill_catalog_entry_t entry;
    esp_err_t err;

    (void)ctx;

    root = cJSON_ParseWithOpts(input_json ? input_json : "{}", NULL, true);
    skill_id_item = root ? cJSON_GetObjectItemCaseSensitive(root, "skill_id") : NULL;
    if (!cJSON_IsString(skill_id_item) || !skill_id_item->valuestring || !skill_id_item->valuestring[0]) {
        cJSON_Delete(root);
        cap_skill_write_error(output, output_size, "skill_id is required", NULL);
        return ESP_ERR_INVALID_ARG;
    }

    /* Copy skill_id out of the parsed JSON, then release `root` immediately:
     * the id is used on every path below (including the success result), so
     * holding a pointer into the freed cJSON tree would be a use-after-free. */
    strlcpy(skill_id, skill_id_item->valuestring, sizeof(skill_id));
    cJSON_Delete(root);

    err = claw_skill_get_catalog_entry(skill_id, &entry);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "skill %s not found before unregister: %s", skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, "skill not found", skill_id);
        return err;
    }
    if (entry.manage_mode == CLAW_SKILL_MANAGE_MODE_READONLY) {
        ESP_LOGW(TAG, "reject unregister readonly skill %s", skill_id);
        cap_skill_write_error(output, output_size, "skill is readonly", skill_id);
        return ESP_ERR_INVALID_STATE;
    }
    {
        const char *root_dir = cap_skill_root_dir();
        if (!root_dir) {
            ESP_LOGE(TAG, "skill storage is not initialized for unregister %s", skill_id);
            cap_skill_write_error(output, output_size, "skill storage is not initialized", skill_id);
            return ESP_ERR_INVALID_STATE;
        }
        if (snprintf(skill_path, sizeof(skill_path), "%s/%s", root_dir, entry.file) >= (int)sizeof(skill_path)) {
            cap_skill_write_error(output, output_size, "file path is too long", skill_id);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    err = cap_skill_read_file_dup(skill_path, &old_markdown);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read skill markdown before unregister %s: %s", skill_id, esp_err_to_name(err));
        cap_skill_write_error(output, output_size, "failed to read skill markdown", skill_id);
        return err;
    }
    if (remove(skill_path) != 0) {
        ESP_LOGE(TAG, "failed to delete skill markdown %s", skill_path);
        free(old_markdown);
        cap_skill_write_error(output, output_size, "failed to delete skill markdown", skill_id);
        return ESP_FAIL;
    }

    err = claw_skill_reload_registry();
    if (err != ESP_OK) {
        if (cap_skill_write_file_text(skill_path, old_markdown) == ESP_OK) {
            (void)claw_skill_reload_registry();
        }
        ESP_LOGE(TAG, "failed to reload registry after unregister %s: %s", skill_id, esp_err_to_name(err));
        free(old_markdown);
        cap_skill_write_error(output, output_size, "failed to reload skill registry", skill_id);
        return err;
    }

    free(old_markdown);
    return cap_skill_build_catalog_result(CAP_SKILL_UNREGISTER, NULL, skill_id, output, output_size);
}

typedef esp_err_t (*cap_skill_mutation_execute_fn)(
    const char *input_json,
    const claw_cap_call_context_t *ctx,
    char *output,
    size_t output_size);

static esp_err_t cap_skill_execute_mutation_locked(
    cap_skill_mutation_execute_fn execute,
    const char *input_json,
    const claw_cap_call_context_t *ctx,
    char *output,
    size_t output_size)
{
    esp_err_t err;

    if (!execute || !s_skill_mutation_lock) {
        cap_skill_write_error(output, output_size,
                              "skill manager is not initialized", NULL);
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_skill_mutation_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        cap_skill_write_error(output, output_size,
                              "another skill update is in progress", NULL);
        return ESP_ERR_TIMEOUT;
    }
    err = execute(input_json, ctx, output, output_size);
    xSemaphoreGive(s_skill_mutation_lock);
    return err;
}

static esp_err_t cap_skill_register_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    return cap_skill_execute_mutation_locked(cap_skill_register_execute_inner,
                                             input_json, ctx, output, output_size);
}

static esp_err_t cap_skill_publish_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    return cap_skill_execute_mutation_locked(cap_skill_publish_execute_inner,
                                             input_json, ctx, output,
                                             output_size);
}

static esp_err_t cap_skill_unregister_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    return cap_skill_execute_mutation_locked(cap_skill_unregister_execute_inner,
                                             input_json, ctx, output, output_size);
}

static esp_err_t cap_skill_set_work_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    return cap_skill_execute_mutation_locked(cap_skill_set_work_execute_inner,
                                             input_json, ctx, output, output_size);
}

static esp_err_t cap_skill_remove_work_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    return cap_skill_execute_mutation_locked(cap_skill_remove_work_execute_inner,
                                             input_json, ctx, output, output_size);
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
        .id = "publish_skill",
        .name = "publish_skill",
        .family = "skill",
        .description = "Publish or refresh one complete runtime Skill. Optionally include launcher behavior; "
                       "the firmware owns persistence and immediately notifies launcher consumers.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
        "\"skill_id\":{\"type\":\"string\"},"
        "\"file\":{\"type\":\"string\",\"pattern\":\"^[^/]+/SKILL\\\\.md$\"},"
        "\"launcher\":{\"type\":[\"object\",\"null\"],\"additionalProperties\":false,\"properties\":{"
        "\"entry\":{\"type\":\"string\"},"
        "\"icon\":{\"type\":\"string\"},"
        "\"args\":{\"type\":\"object\"},"
        "\"exclusive\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},"
        "\"order\":{\"type\":\"integer\"},"
        "\"visible\":{\"type\":\"boolean\"},"
        "\"replace\":{\"type\":\"boolean\"}},\"required\":[\"entry\"]}},"
        "\"required\":[\"skill_id\",\"file\"]}",
        .execute = cap_skill_publish_execute,
    },
    {
        .id = "register_skill",
        .name = "register_skill",
        .family = "skill",
        .description = "Register or refresh an existing source-file skill markdown file and reload the in-memory skill registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
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
        .id = "set_skill_work",
        .name = "set_skill_work",
        .family = "skill",
        .description = "Create or update a registered runtime Skill's Work launcher definition. "
                       "The firmware validates the files and generates launcher.json; do not write launcher.json directly.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json =
        "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
        "\"skill_id\":{\"type\":\"string\"},"
        "\"entry\":{\"type\":\"string\"},"
        "\"icon\":{\"type\":\"string\"},"
        "\"args\":{\"type\":\"object\"},"
        "\"exclusive\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":31},"
        "\"order\":{\"type\":\"integer\"},"
        "\"visible\":{\"type\":\"boolean\"},"
        "\"replace\":{\"type\":\"boolean\"}},"
        "\"required\":[\"skill_id\",\"entry\"]}",
        .execute = cap_skill_set_work_execute,
    },
    {
        .id = "remove_skill_work",
        .name = "remove_skill_work",
        .family = "skill",
        .description = "Remove a registered runtime Skill's Work launcher definition and refresh the registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = 0,
        .input_schema_json =
        "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
        "\"skill_id\":{\"type\":\"string\"}},\"required\":[\"skill_id\"]}",
        .execute = cap_skill_remove_work_execute,
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

esp_err_t cap_skill_mgr_register_group(const char *skills_root_dir)
{
    if (!skills_root_dir || !skills_root_dir[0]) {
        ESP_LOGE(TAG, "register group: missing skills root dir");
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(s_skill_root_dir, sizeof(s_skill_root_dir), "%s", skills_root_dir) >= (int)sizeof(s_skill_root_dir)) {
        s_skill_root_dir[0] = '\0';
        ESP_LOGE(TAG, "register group: skills root dir too long");
        return ESP_ERR_INVALID_SIZE;
    }

    if (!s_skill_mutation_lock) {
        s_skill_mutation_lock = xSemaphoreCreateMutex();
        if (!s_skill_mutation_lock) {
            ESP_LOGE(TAG, "register group: failed to create mutation lock");
            s_skill_root_dir[0] = '\0';
            return ESP_ERR_NO_MEM;
        }
    }

    if (claw_cap_group_exists(s_skill_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_skill_group);
}
