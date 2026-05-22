/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_skill.h"

#include <inttypes.h>
#include <stdbool.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"

static const char *TAG = "claw_skill";
static const char *SKILL_FRONTMATTER_DELIM = "---";
static const char *SKILL_DOCUMENT_NAME = "SKILL.md";
static const char *SKILL_MANAGE_MODE_READONLY = "readonly";
static const char *SKILL_MANAGE_MODE_WEB = "web";
static const char *SKILL_MANAGE_MODE_RUNTIME = "runtime";

#define CLAW_SKILL_DEFAULT_MAX_FILES 64
#define CLAW_SKILL_DEFAULT_MAX_BYTES 2048
#define CLAW_SKILL_MAX_PATH          192

#ifdef CONFIG_CLAW_SKILL_DEBUG_LOG
#define CLAW_SKILL_DIAGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define CLAW_SKILL_DIAGI(...) do { } while (0)
#endif

typedef struct {
    char *id;
    char *file;
    char *summary;
    char **cap_groups;
    size_t cap_group_count;
    claw_skill_manage_mode_t manage_mode;
} claw_skill_registry_entry_t;

typedef struct {
    int initialized;
    char skills_root_dir[CLAW_SKILL_MAX_PATH];
    char session_state_root_dir[CLAW_SKILL_MAX_PATH];
    size_t max_skill_files;
    size_t max_file_bytes;
    claw_skill_registry_entry_t *entries;
    size_t entry_count;
} claw_skill_state_t;

static claw_skill_state_t *s_skill = NULL;

static bool string_array_contains(const char *const *items, size_t count, const char *value);
static esp_err_t push_unique_string(char ***items, size_t *count, const char *value);
static esp_err_t load_registry_dir_recursive(const char *relative_dir,
                                             claw_skill_registry_entry_t **entries,
                                             size_t *entry_count);
static esp_err_t parse_skill_document_metadata(const char *filename, const char *text, claw_skill_registry_entry_t *entry);

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

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

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        va_end(args);
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    va_end(args);
    return buf;
}

static void free_string_array(char **items, size_t count)
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

static void free_registry_entry(claw_skill_registry_entry_t *entry)
{
    if (!entry) {
        return;
    }

    free(entry->id);
    free(entry->file);
    free(entry->summary);
    free_string_array(entry->cap_groups, entry->cap_group_count);
    memset(entry, 0, sizeof(*entry));
}

static void free_registry_entries(claw_skill_registry_entry_t *entries, size_t count)
{
    size_t i;

    if (!entries) {
        return;
    }

    for (i = 0; i < count; i++) {
        free_registry_entry(&entries[i]);
    }
    free(entries);
}

static void claw_skill_reset(void)
{
    size_t i;

    if (!s_skill) {
        return;
    }

    for (i = 0; i < s_skill->entry_count; i++) {
        free_registry_entry(&s_skill->entries[i]);
    }
    free(s_skill->entries);
    memset(s_skill, 0, sizeof(*s_skill));
    free(s_skill);
    s_skill = NULL;
}

static bool is_skill_document_file(const char *name)
{
    const char *base;

    if (!name) {
        return false;
    }

    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return strcasecmp(base, SKILL_DOCUMENT_NAME) == 0;
}

static bool skill_path_is_valid(const char *path)
{
    if (!path || !path[0]) {
        return false;
    }
    if (path[0] == '/' || strstr(path, "..") != NULL) {
        return false;
    }
    return strchr(path, '\\') == NULL;
}

static char *build_skill_path_dup(const char *filename)
{
    if (!s_skill) {
        return NULL;
    }

    return dup_printf("%s/%s", s_skill->skills_root_dir, filename);
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st = {0};

    if (!path || !path[0]) {
        ESP_LOGE(TAG, "mkdir: bad path");
        return ESP_ERR_INVALID_ARG;
    }
    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            ESP_LOGE(TAG, "mkdir: not dir %s", path);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    if (mkdir(path, 0755) != 0) {
        ESP_LOGE(TAG, "mkdir: %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void sanitize_session_id(const char *session_id, char *buf, size_t size)
{
    size_t off = 0;

    if (!buf || size == 0) {
        return;
    }
    buf[0] = '\0';
    if (!session_id) {
        return;
    }

    while (*session_id && off + 1 < size) {
        char ch = *session_id++;

        if ((ch >= 'a' && ch <= 'z') ||
                (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9')) {
            buf[off++] = ch;
        } else if (off == 0 || buf[off - 1] != '_') {
            buf[off++] = '_';
        }
    }
    if (off > 0 && buf[off - 1] == '_') {
        off--;
    }
    buf[off] = '\0';
}

static char *build_session_state_path_dup(const char *session_id)
{
    char safe_session_id[48];
    uint32_t hash = 2166136261u;
    const unsigned char *p = (const unsigned char *)session_id;
    size_t len;

    if (!s_skill || !session_id || !session_id[0] || !s_skill->session_state_root_dir[0]) {
        return NULL;
    }

    sanitize_session_id(session_id, safe_session_id, sizeof(safe_session_id));
    while (p && *p) {
        hash ^= *p++;
        hash *= 16777619u;
    }

    len = strnlen(safe_session_id, sizeof(safe_session_id) - 1);
    if (len > 24) {
        safe_session_id[24] = '\0';
    }

    return dup_printf("%s/s_%s_%08" PRIx32 ".skills.json",
                      s_skill->session_state_root_dir,
                      safe_session_id[0] ? safe_session_id : "default",
                      hash);
}

static esp_err_t read_file_dup(const char *path, size_t max_bytes, char **out_data)
{
    FILE *file = NULL;
    long size;
    char *data = NULL;
    size_t read_bytes;

    if (!path || !out_data || max_bytes == 0) {
        ESP_LOGE(TAG, "read: bad arg");
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    CLAW_SKILL_DIAGI("read %s", path);

    file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "read open: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "read seek: %s", path);
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0) {
        ESP_LOGE(TAG, "read size: %s", path);
        fclose(file);
        return ESP_FAIL;
    }
    if ((size_t)size > max_bytes) {
        ESP_LOGE(TAG, "read too large: %s (%ld > %u)",
                 path, size, (unsigned)max_bytes);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "read rewind: %s", path);
        fclose(file);
        return ESP_FAIL;
    }

    data = calloc(1, (size_t)size + 1);
    if (!data) {
        ESP_LOGE(TAG, "read oom: %s (%ld)", path, size);
        fclose(file);
        return ESP_ERR_NO_MEM;
    }

    read_bytes = fread(data, 1, (size_t)size, file);
    fclose(file);
    data[read_bytes] = '\0';
    *out_data = data;
    return ESP_OK;
}

static esp_err_t write_file_text(const char *path, const char *text)
{
    FILE *file = NULL;

    if (!path || !text) {
        ESP_LOGE(TAG, "write: bad arg");
        return ESP_ERR_INVALID_ARG;
    }
    CLAW_SKILL_DIAGI("write %s", path);

    file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "write open: %s", path);
        return ESP_FAIL;
    }
    if (fputs(text, file) < 0) {
        ESP_LOGE(TAG, "write fail: %s", path);
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    return ESP_OK;
}

static esp_err_t json_dup_required_string(cJSON *object, const char *key, char **out_value)
{
    cJSON *item;

    if (!object || !key || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_value = NULL;

    item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_value = strdup(item->valuestring);
    return *out_value ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t json_dup_optional_unique_string_array(cJSON *object,
                                                       const char *key,
                                                       char ***out_items,
                                                       size_t *out_count)
{
    cJSON *array = NULL;
    char **items = NULL;
    size_t count = 0;
    int index;

    if (!object || !key || !out_items || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_items = NULL;
    *out_count = 0;

    array = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!array) {
        return ESP_OK;
    }
    if (!cJSON_IsArray(array)) {
        return ESP_ERR_INVALID_ARG;
    }

    for (index = 0; index < cJSON_GetArraySize(array); index++) {
        cJSON *item = cJSON_GetArrayItem(array, index);
        esp_err_t err;

        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
            free_string_array(items, count);
            return ESP_ERR_INVALID_ARG;
        }
        if (string_array_contains((const char *const *)items, count, item->valuestring)) {
            free_string_array(items, count);
            return ESP_ERR_INVALID_ARG;
        }

        err = push_unique_string(&items, &count, item->valuestring);
        if (err != ESP_OK) {
            free_string_array(items, count);
            return err;
        }
    }

    *out_items = items;
    *out_count = count;
    return ESP_OK;
}

static esp_err_t json_parse_manage_mode(cJSON *object, const char *key, claw_skill_manage_mode_t *out_mode)
{
    cJSON *item;

    if (!object || !key || !out_mode) {
        return ESP_ERR_INVALID_ARG;
    }

    item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(item->valuestring, SKILL_MANAGE_MODE_READONLY) == 0) {
        *out_mode = CLAW_SKILL_MANAGE_MODE_READONLY;
        return ESP_OK;
    }
    if (strcmp(item->valuestring, SKILL_MANAGE_MODE_WEB) == 0) {
        *out_mode = CLAW_SKILL_MANAGE_MODE_READONLY;
        return ESP_OK;
    }
    if (strcmp(item->valuestring, SKILL_MANAGE_MODE_RUNTIME) == 0) {
        *out_mode = CLAW_SKILL_MANAGE_MODE_RUNTIME;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static const char *manage_mode_to_string(claw_skill_manage_mode_t mode)
{
    switch (mode) {
        case CLAW_SKILL_MANAGE_MODE_RUNTIME:
            return SKILL_MANAGE_MODE_RUNTIME;
        case CLAW_SKILL_MANAGE_MODE_READONLY:
        default:
            return SKILL_MANAGE_MODE_READONLY;
    }
}

static const claw_skill_registry_entry_t *claw_skill_find_entry(const char *skill_id)
{
    size_t i;

    if (!s_skill || !skill_id || !skill_id[0]) {
        return NULL;
    }

    for (i = 0; i < s_skill->entry_count; i++) {
        if (strcmp(s_skill->entries[i].id, skill_id) == 0) {
            return &s_skill->entries[i];
        }
    }

    return NULL;
}

static bool string_array_contains(const char *const *items, size_t count, const char *value)
{
    size_t i;

    if (!items || !value) {
        return false;
    }

    for (i = 0; i < count; i++) {
        if (items[i] && strcmp(items[i], value) == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t push_unique_string(char ***items, size_t *count, const char *value)
{
    char **grown;

    if (!items || !count || !value || !value[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (string_array_contains((const char *const *) * items, *count, value)) {
        return ESP_OK;
    }

    grown = realloc(*items, sizeof(char *) * (*count + 1));
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }
    *items = grown;
    (*items)[*count] = strdup(value);
    if (!(*items)[*count]) {
        return ESP_ERR_NO_MEM;
    }
    (*count)++;
    return ESP_OK;
}

static esp_err_t extract_skill_frontmatter_json(const char *text, const char **out_json_start, const char **out_json_end, const char **out_body)
{
    const char *cursor = text;
    const char *json_start;
    const char *json_end;

    if (!text || !out_json_start || !out_json_end || !out_body) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json_start = NULL;
    *out_json_end = NULL;
    *out_body = NULL;

    if ((unsigned char)cursor[0] == 0xEF && (unsigned char)cursor[1] == 0xBB && (unsigned char)cursor[2] == 0xBF) {
        cursor += 3;
    }
    if (strncmp(cursor, SKILL_FRONTMATTER_DELIM, strlen(SKILL_FRONTMATTER_DELIM)) != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    cursor += strlen(SKILL_FRONTMATTER_DELIM);
    if (*cursor == '\r') {
        cursor++;
    }
    if (*cursor != '\n') {
        return ESP_ERR_INVALID_ARG;
    }
    cursor++;

    json_start = cursor;
    json_end = strstr(json_start, "\n---");
    if (!json_end) {
        return ESP_ERR_INVALID_ARG;
    }

    cursor = json_end + strlen("\n---");
    if (*cursor == '\r') {
        cursor++;
    }
    if (*cursor != '\n' && *cursor != '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (*cursor == '\n') {
        cursor++;
    }

    *out_json_start = json_start;
    *out_json_end = json_end;
    *out_body = cursor;
    return ESP_OK;
}

static esp_err_t parse_skill_document_metadata(const char *filename, const char *text, claw_skill_registry_entry_t *entry)
{
    const char *json_start = NULL;
    const char *json_end = NULL;
    const char *body = NULL;
    char *json_text = NULL;
    cJSON *root = NULL;
    cJSON *metadata = NULL;
    esp_err_t err;

    if (!filename || !text || !entry) {
        return ESP_ERR_INVALID_ARG;
    }
    err = extract_skill_frontmatter_json(text, &json_start, &json_end, &body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frontmatter: %s", filename);
        return ESP_ERR_INVALID_ARG;
    }
    (void)body;

    json_text = calloc(1, (size_t)(json_end - json_start) + 1);
    if (!json_text) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(json_text, json_start, (size_t)(json_end - json_start));

    root = cJSON_Parse(json_text);
    free(json_text);
    if (!root || !cJSON_IsObject(root)) {
        ESP_LOGE(TAG, "meta json: %s", filename);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    metadata = cJSON_GetObjectItemCaseSensitive(root, "metadata");
    if (!cJSON_IsObject(metadata)) {
        ESP_LOGE(TAG, "meta metadata: %s", filename);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    err = json_dup_required_string(root, "name", &entry->id);
    if (err == ESP_OK) {
        entry->file = strdup(filename);
        err = entry->file ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = json_dup_required_string(root, "description", &entry->summary);
    }
    if (err == ESP_OK) {
        err = json_dup_optional_unique_string_array(metadata, "cap_groups", &entry->cap_groups, &entry->cap_group_count);
    }
    if (err == ESP_OK) {
        err = json_parse_manage_mode(metadata, "manage_mode", &entry->manage_mode);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "meta fields: %s", filename);
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t validate_registry_entry(claw_skill_registry_entry_t *entry)
{
    char *path = NULL;
    FILE *file = NULL;
    char expected_file[CLAW_SKILL_MAX_PATH] = {0};
    size_t i;

    if (!entry || !entry->id || !entry->file || !entry->summary) {
        ESP_LOGE(TAG, "skill meta: missing fields");
        return ESP_ERR_INVALID_ARG;
    }
    if (!skill_path_is_valid(entry->id) || strchr(entry->id, '/') || strchr(entry->id, '\\')) {
        ESP_LOGE(TAG, "skill id: %s", entry->id ? entry->id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!skill_path_is_valid(entry->file) || !is_skill_document_file(entry->file)) {
        ESP_LOGE(TAG, "skill path: id=%s file=%s", entry->id ? entry->id : "(null)", entry->file ? entry->file : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(expected_file, sizeof(expected_file), "%s/%s", entry->id, SKILL_DOCUMENT_NAME) >= (int)sizeof(expected_file)) {
        ESP_LOGE(TAG, "skill expected path too long: id=%s", entry->id);
        return ESP_ERR_INVALID_SIZE;
    }
    if (strcasecmp(entry->file, expected_file) != 0) {
        ESP_LOGE(TAG, "skill path must be %s, got %s", expected_file, entry->file);
        return ESP_ERR_INVALID_ARG;
    }
    for (i = 0; i < entry->cap_group_count; i++) {
        if (!entry->cap_groups[i] || !entry->cap_groups[i][0]) {
            ESP_LOGE(TAG, "skill cap_group: id=%s idx=%u", entry->id ? entry->id : "(null)", (unsigned)i);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (entry->manage_mode != CLAW_SKILL_MANAGE_MODE_READONLY && entry->manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME) {
        ESP_LOGE(TAG, "skill mode: %s", entry->id ? entry->id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    path = build_skill_path_dup(entry->file);
    if (!path) {
        ESP_LOGE(TAG, "skill path alloc: %s", entry->id ? entry->id : "(null)");
        return ESP_ERR_NO_MEM;
    }
    file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "skill missing: id=%s path=%s", entry->id ? entry->id : "(null)", path);
        free(path);
        return ESP_ERR_NOT_FOUND;
    }
    free(path);
    fclose(file);
    return ESP_OK;
}

static esp_err_t load_registry_dir_recursive(const char *relative_dir,
                                             claw_skill_registry_entry_t **entries,
                                             size_t *entry_count)
{
    DIR *dir = NULL;
    struct dirent *item = NULL;
    char *dir_path = NULL;
    esp_err_t err = ESP_OK;

    if (!s_skill || !s_skill->skills_root_dir[0] || !entries || !entry_count) {
        return ESP_ERR_INVALID_STATE;
    }

    dir_path = relative_dir && relative_dir[0] ? build_skill_path_dup(relative_dir) : strdup(s_skill->skills_root_dir);
    if (!dir_path) {
        return ESP_ERR_NO_MEM;
    }
    dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "open skills dir %s failed", dir_path);
        free(dir_path);
        return ESP_ERR_NOT_FOUND;
    }

    while ((item = readdir(dir)) != NULL) {
        char relative_path[CLAW_SKILL_MAX_PATH] = {0};
        char *path = NULL;
        char *text = NULL;
        claw_skill_registry_entry_t *grown = NULL;
        claw_skill_registry_entry_t *entry = NULL;
        size_t i;
        struct stat st = {0};

        if (!item->d_name[0] || strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (relative_dir && relative_dir[0]) {
            if (snprintf(relative_path, sizeof(relative_path), "%s/%s", relative_dir, item->d_name) >= (int)sizeof(relative_path)) {
                err = ESP_ERR_INVALID_SIZE;
                goto cleanup;
            }
        } else {
            if (snprintf(relative_path, sizeof(relative_path), "%s", item->d_name) >= (int)sizeof(relative_path)) {
                err = ESP_ERR_INVALID_SIZE;
                goto cleanup;
            }
        }
        path = build_skill_path_dup(relative_path);
        if (!path) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        if (stat(path, &st) != 0) {
            free(path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            free(path);
            err = load_registry_dir_recursive(relative_path, entries, entry_count);
            if (err != ESP_OK) {
                goto cleanup;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_skill_document_file(relative_path)) {
            free(path);
            continue;
        }
        if (*entry_count >= s_skill->max_skill_files) {
            ESP_LOGE(TAG, "too many skill files under %s", s_skill->skills_root_dir);
            err = ESP_ERR_INVALID_SIZE;
            free(path);
            goto cleanup;
        }

        err = read_file_dup(path, s_skill->max_file_bytes, &text);
        free(path);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "read skill file %s failed: %s", relative_path, esp_err_to_name(err));
            goto cleanup;
        }

        grown = realloc(*entries, sizeof(**entries) * (*entry_count + 1));
        if (!grown) {
            free(text);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        *entries = grown;
        memset(&(*entries)[*entry_count], 0, sizeof((*entries)[*entry_count]));
        entry = &(*entries)[*entry_count];

        err = parse_skill_document_metadata(relative_path, text, entry);
        free(text);
        if (err == ESP_ERR_INVALID_ARG) {
            ESP_LOGE(TAG, "skill file %s has invalid metadata", relative_path);
            free_registry_entry(entry);
            goto cleanup;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "skill file %s metadata parse failed: %s", relative_path, esp_err_to_name(err));
            free_registry_entry(entry);
            goto cleanup;
        }

        err = validate_registry_entry(entry);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "skill file %s validation failed: %s", relative_path, esp_err_to_name(err));
            free_registry_entry(entry);
            goto cleanup;
        }

        for (i = 0; i < *entry_count; i++) {
            if (strcmp((*entries)[i].id, entry->id) == 0) {
                ESP_LOGE(TAG, "duplicate skill id %s", entry->id);
                err = ESP_ERR_INVALID_ARG;
                free_registry_entry(entry);
                goto cleanup;
            }
            if (strcmp((*entries)[i].file, entry->file) == 0) {
                ESP_LOGE(TAG, "duplicate skill file %s", entry->file);
                err = ESP_ERR_INVALID_ARG;
                free_registry_entry(entry);
                goto cleanup;
            }
        }

        (*entry_count)++;
    }

cleanup:
    closedir(dir);
    free(dir_path);
    return err;
}

static esp_err_t load_registry_from_markdown(void)
{
    claw_skill_registry_entry_t *entries = NULL;
    size_t entry_count = 0;
    esp_err_t err;

    err = load_registry_dir_recursive(NULL, &entries, &entry_count);
    if (err != ESP_OK) {
        free_registry_entries(entries, entry_count);
        return err;
    }
    if (entry_count == 0) {
        free_registry_entries(entries, entry_count);
        ESP_LOGE(TAG, "no valid skill markdown found under %s", s_skill->skills_root_dir);
        return ESP_ERR_NOT_FOUND;
    }

    s_skill->entries = entries;
    s_skill->entry_count = entry_count;
    return ESP_OK;
}

/* Replace all occurrences of var with replacement in buf (in-place). */
static void str_replace_inplace(char *buf, size_t buf_size, const char *var, const char *replacement)
{
    size_t var_len = strlen(var);
    size_t rep_len = strlen(replacement);
    char *pos = buf;

    while ((pos = strstr(pos, var)) != NULL) {
        size_t tail_len = strlen(pos + var_len);
        if ((size_t)(pos - buf) + rep_len + tail_len + 1 > buf_size) {
            break;
        }
        memmove(pos + rep_len, pos + var_len, tail_len + 1);
        memcpy(pos, replacement, rep_len);
        pos += rep_len;
    }
}

static esp_err_t str_replace_required_len(const char *text,
                                          const char *var,
                                          const char *replacement,
                                          size_t *out_required_len)
{
    const char *pos = text;
    size_t required_len;
    size_t var_len;
    size_t rep_len;

    if (!text || !var || !replacement || !out_required_len || !var[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    required_len = strlen(text);
    var_len = strlen(var);
    rep_len = strlen(replacement);

    while ((pos = strstr(pos, var)) != NULL) {
        if (rep_len > var_len) {
            size_t delta = rep_len - var_len;

            if (SIZE_MAX - required_len < delta) {
                return ESP_ERR_INVALID_SIZE;
            }
            required_len += delta;
        } else {
            required_len -= var_len - rep_len;
        }
        pos += var_len;
    }

    *out_required_len = required_len;
    return ESP_OK;
}

esp_err_t claw_skill_read_document(const char *skill_id, char *buf, size_t size)
{
    const claw_skill_registry_entry_t *entry = NULL;
    char *path = NULL;
    char *text = NULL;
    char cur_skill_dir[CLAW_SKILL_MAX_PATH];
    size_t required_len;
    esp_err_t err;
    int written;

    if (!s_skill || !s_skill->initialized) {
        ESP_LOGE(TAG, "read doc: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!skill_id || !skill_id[0] || !buf || size == 0) {
        ESP_LOGE(TAG, "read doc: bad arg");
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    entry = claw_skill_find_entry(skill_id);
    if (!entry) {
        ESP_LOGE(TAG, "read doc %s: not found", skill_id);
        return ESP_ERR_NOT_FOUND;
    }

    path = build_skill_path_dup(entry->file);
    if (!path) {
        ESP_LOGE(TAG, "read doc %s: no path", entry->id ? entry->id : "(null)");
        return ESP_ERR_NO_MEM;
    }
    CLAW_SKILL_DIAGI("read doc %s", entry->id ? entry->id : "(null)");
    err = read_file_dup(path, s_skill->max_file_bytes, &text);
    free(path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read doc %s: %s", entry->id ? entry->id : "(null)", esp_err_to_name(err));
        return err;
    }

    written = snprintf(cur_skill_dir, sizeof(cur_skill_dir), "%s/%s", s_skill->skills_root_dir, skill_id);
    if (written < 0 || (size_t)written >= sizeof(cur_skill_dir)) {
        ESP_LOGE(TAG, "read doc %s: skill dir too long", entry->id ? entry->id : "(null)");
        free(text);
        return ESP_ERR_INVALID_SIZE;
    }

    err = str_replace_required_len(text, "{CUR_SKILL_DIR}", cur_skill_dir, &required_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read doc %s: size calculation failed", entry->id ? entry->id : "(null)");
        free(text);
        return err;
    }
    if (required_len >= size) {
        ESP_LOGE(TAG, "read doc %s: document too large", entry->id ? entry->id : "(null)");
        free(text);
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(buf, size, "%s", text);
    free(text);

    /* Expand {CUR_SKILL_DIR} placeholders so the LLM sees resolved absolute paths directly. */
    str_replace_inplace(buf, size, "{CUR_SKILL_DIR}", cur_skill_dir);
    return ESP_OK;
}

static esp_err_t load_active_skill_ids_from_disk(const char *session_id,
                                                 char ***out_skill_ids,
                                                 size_t *out_skill_count)
{
    char *path = NULL;
    char *json_text = NULL;
    cJSON *root = NULL;
    char **loaded = NULL;
    size_t loaded_count = 0;
    size_t i;
    esp_err_t err;

    if (!out_skill_ids || !out_skill_count) {
        ESP_LOGE(TAG, "load active: bad arg");
        return ESP_ERR_INVALID_ARG;
    }
    *out_skill_ids = NULL;
    *out_skill_count = 0;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0]) {
        ESP_LOGE(TAG, "load active: bad state");
        return ESP_ERR_INVALID_STATE;
    }

    path = build_session_state_path_dup(session_id);
    if (!path) {
        ESP_LOGE(TAG, "session path %s", session_id ? session_id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    CLAW_SKILL_DIAGI("load active %s", session_id);

    /* Session state file may not exist for new sessions — that is normal. */
    struct stat st = {0};
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        free(path);
        return ESP_ERR_NOT_FOUND;
    }

    err = read_file_dup(path, SIZE_MAX, &json_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read session %s: %s", session_id, esp_err_to_name(err));
        free(path);
        return err;
    }
    free(path);

    root = cJSON_Parse(json_text);
    free(json_text);
    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "bad session json %s", session_id);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < (size_t)cJSON_GetArraySize(root); i++) {
        cJSON *item = cJSON_GetArrayItem(root, (int)i);

        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
            continue;
        }
        if (!claw_skill_find_entry(item->valuestring)) {
            continue;
        }
        err = push_unique_string(&loaded, &loaded_count, item->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "store skill %s: %s", item->valuestring, esp_err_to_name(err));
            free_string_array(loaded, loaded_count);
            cJSON_Delete(root);
            return err;
        }
    }

    cJSON_Delete(root);
    if (loaded_count == 0) {
        free_string_array(loaded, loaded_count);
        return ESP_ERR_NOT_FOUND;
    }

    *out_skill_ids = loaded;
    *out_skill_count = loaded_count;
    return ESP_OK;
}

static esp_err_t save_active_skill_ids_to_disk(const char *session_id,
                                               const char *const *skill_ids,
                                               size_t skill_count)
{
    char *path = NULL;
    cJSON *root = NULL;
    char *json_text = NULL;
    esp_err_t err = ESP_OK;
    size_t i;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0]) {
        ESP_LOGE(TAG, "save active: bad state");
        return ESP_ERR_INVALID_STATE;
    }

    path = build_session_state_path_dup(session_id);
    if (!path) {
        ESP_LOGE(TAG, "session path %s", session_id ? session_id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    CLAW_SKILL_DIAGI("save active %s (%u)", session_id, (unsigned)skill_count);

    if (skill_count == 0) {
        remove(path);
        free(path);
        return ESP_OK;
    }

    root = cJSON_CreateArray();
    if (!root) {
        ESP_LOGE(TAG, "session array alloc %s", session_id);
        free(path);
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < skill_count; i++) {
        cJSON *item;

        if (!skill_ids[i] || !skill_ids[i][0]) {
            continue;
        }
        item = cJSON_CreateString(skill_ids[i]);
        if (!item) {
            ESP_LOGE(TAG, "skill item alloc %s", skill_ids[i]);
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        cJSON_AddItemToArray(root, item);
    }

    json_text = cJSON_PrintUnformatted(root);
    if (!json_text) {
        ESP_LOGE(TAG, "session encode %s", session_id);
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = write_file_text(path, json_text);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write session %s: %s", session_id, esp_err_to_name(err));
    }

cleanup:
    free(path);
    cJSON_Delete(root);
    free(json_text);
    return err;
}

static esp_err_t claw_skill_render_skills_list(char *buf, size_t size)
{
    size_t i;
    size_t off = 0;

    if (!s_skill || !s_skill->initialized || !buf || size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    buf[0] = '\0';
    off += snprintf(buf + off, size - off, "Available skills:\n");
    for (i = 0; i < s_skill->entry_count && off + 1 < size; i++) {
        const claw_skill_registry_entry_t *entry = &s_skill->entries[i];

        off += snprintf(buf + off,
                        size - off,
                        "- %s: %s\n",
                        entry->id,
                        entry->summary);
    }

    return ESP_OK;
}

esp_err_t claw_skill_init(const claw_skill_config_t *config)
{
    esp_err_t err;

    if (!config || !config->skills_root_dir || !config->session_state_root_dir) {
        ESP_LOGE(TAG, "init: bad config");
        return ESP_ERR_INVALID_ARG;
    }

    claw_skill_reset();
    s_skill = calloc(1, sizeof(*s_skill));
    if (!s_skill) {
        claw_skill_reset();
        return ESP_ERR_NO_MEM;
    }
    safe_copy(s_skill->skills_root_dir, sizeof(s_skill->skills_root_dir), config->skills_root_dir);
    safe_copy(s_skill->session_state_root_dir,
              sizeof(s_skill->session_state_root_dir),
              config->session_state_root_dir);
    s_skill->max_skill_files = config->max_skill_files ? config->max_skill_files : CLAW_SKILL_DEFAULT_MAX_FILES;
    s_skill->max_file_bytes = config->max_file_bytes ? config->max_file_bytes : CLAW_SKILL_DEFAULT_MAX_BYTES;

    err = ensure_dir(s_skill->session_state_root_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init dir %s: %s", s_skill->session_state_root_dir, esp_err_to_name(err));
        claw_skill_reset();
        return err;
    }

    err = load_registry_from_markdown();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init registry: %s", esp_err_to_name(err));
        claw_skill_reset();
        return err;
    }

    s_skill->initialized = 1;
    ESP_LOGI(TAG, "Initialized registry with %u skill(s)", (unsigned)s_skill->entry_count);
    return ESP_OK;
}

esp_err_t claw_skill_reload_registry(void)
{
    claw_skill_registry_entry_t *old_entries = NULL;
    size_t old_count = 0;
    esp_err_t err;

    if (!s_skill || !s_skill->initialized) {
        ESP_LOGE(TAG, "reload before init");
        return ESP_ERR_INVALID_STATE;
    }

    old_entries = s_skill->entries;
    old_count = s_skill->entry_count;
    s_skill->entries = NULL;
    s_skill->entry_count = 0;

    err = load_registry_from_markdown();
    if (err == ESP_OK) {
        free_registry_entries(old_entries, old_count);
        ESP_LOGI(TAG, "Reloaded registry with %u skill(s)", (unsigned)s_skill->entry_count);
        return ESP_OK;
    }

    s_skill->entries = old_entries;
    s_skill->entry_count = old_count;
    ESP_LOGE(TAG, "reload registry: %s", esp_err_to_name(err));
    return err;
}

const char *claw_skill_get_skills_root_dir(void)
{
    if (!s_skill || !s_skill->initialized || !s_skill->skills_root_dir[0]) {
        return NULL;
    }

    return s_skill->skills_root_dir;
}

esp_err_t claw_skill_read_skills_list(char *buf, size_t size)
{
    return claw_skill_render_skills_list(buf, size);
}

esp_err_t claw_skill_render_catalog_json(char *buf, size_t size)
{
    cJSON *root = NULL;
    cJSON *skills = NULL;
    char *rendered = NULL;
    size_t i;

    if (!s_skill || !s_skill->initialized || !buf || size == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    root = cJSON_CreateObject();
    skills = cJSON_CreateArray();
    if (!root || !skills) {
        cJSON_Delete(root);
        cJSON_Delete(skills);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddItemToObject(root, "skills", skills);
    for (i = 0; i < s_skill->entry_count; i++) {
        cJSON *skill = cJSON_CreateObject();
        cJSON *cap_groups = cJSON_CreateArray();
        size_t j;

        if (!skill || !cap_groups) {
            cJSON_Delete(skill);
            cJSON_Delete(cap_groups);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }

        cJSON_AddStringToObject(skill, "id", s_skill->entries[i].id);
        cJSON_AddStringToObject(skill, "file", s_skill->entries[i].file);
        cJSON_AddStringToObject(skill, "summary", s_skill->entries[i].summary);
        cJSON_AddStringToObject(skill, "manage_mode", manage_mode_to_string(s_skill->entries[i].manage_mode));
        for (j = 0; j < s_skill->entries[i].cap_group_count; j++) {
            cJSON_AddItemToArray(cap_groups, cJSON_CreateString(s_skill->entries[i].cap_groups[j]));
        }
        cJSON_AddItemToObject(skill, "cap_groups", cap_groups);
        cJSON_AddItemToArray(skills, skill);
    }

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    if (strlen(rendered) >= size) {
        free(rendered);
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(buf, size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

esp_err_t claw_skill_get_catalog_entry(const char *skill_id, claw_skill_catalog_entry_t *out_entry)
{
    const claw_skill_registry_entry_t *entry = claw_skill_find_entry(skill_id);

    if (!out_entry) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!entry) {
        return ESP_ERR_NOT_FOUND;
    }

    out_entry->id = entry->id;
    out_entry->file = entry->file;
    out_entry->summary = entry->summary;
    out_entry->cap_groups = (const char *const *)entry->cap_groups;
    out_entry->cap_group_count = entry->cap_group_count;
    out_entry->manage_mode = entry->manage_mode;
    return ESP_OK;
}

esp_err_t claw_skill_load_active_skill_ids(const char *session_id,
                                           char ***out_skill_ids,
                                           size_t *out_skill_count)
{
    return load_active_skill_ids_from_disk(session_id, out_skill_ids, out_skill_count);
}

esp_err_t claw_skill_load_active_cap_groups(const char *session_id,
                                            char ***out_group_ids,
                                            size_t *out_group_count)
{
    char **active_skill_ids = NULL;
    size_t active_skill_count = 0;
    char **group_ids = NULL;
    size_t group_count = 0;
    esp_err_t err;
    size_t i;
    size_t j;

    if (!out_group_ids || !out_group_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_group_ids = NULL;
    *out_group_count = 0;

    err = load_active_skill_ids_from_disk(session_id, &active_skill_ids, &active_skill_count);
    if (err != ESP_OK) {
        return err;
    }

    for (i = 0; i < active_skill_count; i++) {
        const claw_skill_registry_entry_t *entry = claw_skill_find_entry(active_skill_ids[i]);

        if (!entry) {
            continue;
        }

        for (j = 0; j < entry->cap_group_count; j++) {
            err = push_unique_string(&group_ids, &group_count, entry->cap_groups[j]);
            if (err != ESP_OK) {
                free_string_array(active_skill_ids, active_skill_count);
                free_string_array(group_ids, group_count);
                return err;
            }
        }
    }

    free_string_array(active_skill_ids, active_skill_count);
    if (group_count == 0) {
        free_string_array(group_ids, group_count);
        return ESP_ERR_NOT_FOUND;
    }

    *out_group_ids = group_ids;
    *out_group_count = group_count;
    return ESP_OK;
}

esp_err_t claw_skill_activate_for_session(const char *session_id, const char *skill_id)
{
    char **active = NULL;
    size_t active_count = 0;
    esp_err_t err;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0] || !skill_id || !skill_id[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_skill_find_entry(skill_id)) {
        return ESP_ERR_NOT_FOUND;
    }

    err = load_active_skill_ids_from_disk(session_id, &active, &active_count);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    err = push_unique_string(&active, &active_count, skill_id);
    if (err != ESP_OK) {
        free_string_array(active, active_count);
        return err;
    }

    err = save_active_skill_ids_to_disk(session_id, (const char *const *)active, active_count);
    free_string_array(active, active_count);
    return err;
}

static esp_err_t claw_skill_skills_list_collect(const claw_core_request_t *request,
                                                claw_core_context_t *out_context,
                                                void *user_ctx)
{
    char *content = NULL;
    size_t content_size;
    esp_err_t err;

    (void)request;
    (void)user_ctx;

    if (!out_context || !s_skill || !s_skill->initialized) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_context, 0, sizeof(*out_context));

    content_size = 64;
    for (size_t i = 0; i < s_skill->entry_count; i++) {
        content_size += strlen(s_skill->entries[i].id ? s_skill->entries[i].id : "");
        content_size += strlen(s_skill->entries[i].summary ? s_skill->entries[i].summary : "");
        content_size += 16;
    }

    content = calloc(1, content_size + 1);
    if (!content) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_skill_render_skills_list(content, content_size + 1);
    if (err != ESP_OK) {
        free(content);
        return err;
    }
    if (!content[0]) {
        free(content);
        return ESP_ERR_NOT_FOUND;
    }

    out_context->kind = CLAW_CORE_CONTEXT_KIND_SYSTEM_PROMPT;
    out_context->content = content;
    return ESP_OK;
}

const claw_core_context_provider_t claw_skill_skills_list_provider = {
    .name = "Skills List",
    .collect = claw_skill_skills_list_collect,
    .user_ctx = NULL,
};
