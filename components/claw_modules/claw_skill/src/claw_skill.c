/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "claw_skill.h"
#include "claw_utils_file.h"

static const char *TAG = "claw_skill";
static const char *SKILL_FRONTMATTER_DELIM = "---";
static const char *SKILL_DOCUMENT_NAME = "SKILL.md";

#define CLAW_SKILL_MAX_FILES         64  /* hard cap on registry entries across all directories */
#define CLAW_SKILL_MAX_PATH          192
#define CLAW_SKILL_MAX_REGISTRY_LISTENERS 4
#define CLAW_SKILL_FRONTMATTER_READ_CHUNK 256
#define CLAW_SKILL_SESSION_STATE_MAX_BYTES 4096

#ifdef CONFIG_CLAW_SKILL_DEBUG_LOG
#define CLAW_SKILL_DIAGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define CLAW_SKILL_DIAGI(...) do { } while (0)
#endif

typedef struct {
    char *id;
    char *file;
    char *summary;
    char *skill_dir;
    char **cap_groups;
    size_t cap_group_count;
    claw_skill_manage_mode_t manage_mode;
    const char *root_dir;  /* points into claw_skill_state_t.roots; the partition this skill lives in */
} claw_skill_registry_entry_t;

typedef struct {
    claw_skill_registry_changed_cb_t callback;
    void *user_ctx;
} claw_skill_registry_listener_t;

typedef struct {
    int initialized;
    char **roots;        /* dynamic list of skills directories; roots[0] is the primary writable root */
    size_t root_count;
    char session_state_root_dir[CLAW_SKILL_MAX_PATH];
    size_t max_file_bytes;
    claw_skill_registry_entry_t *entries;
    size_t entry_count;
    SemaphoreHandle_t registry_lock;
    SemaphoreHandle_t session_lock;
    claw_skill_registry_listener_t
        registry_listeners[CLAW_SKILL_MAX_REGISTRY_LISTENERS];
} claw_skill_state_t;

static claw_skill_state_t *s_skill = NULL;

static bool string_array_contains(const char *const *items, size_t count, const char *value);
static esp_err_t push_unique_string(char ***items, size_t *count, const char *value);
static char *dup_printf(const char *fmt, ...);
static esp_err_t load_registry_dir_recursive(const char *root_dir,
                                             const char *relative_dir,
                                             claw_skill_registry_entry_t **entries,
                                             size_t *entry_count);
static esp_err_t parse_skill_document_metadata(const char *filename, const char *text, claw_skill_registry_entry_t *entry);
static esp_err_t claw_skill_reload_registry_locked(void);

static esp_err_t remove_directory_recursive(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "open directory for removal failed: path=%s errno=%d", path, errno);
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    esp_err_t err = ESP_OK;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        struct stat st = {0};
        char *child = NULL;

        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        child = dup_printf("%s/%s", path, item->d_name);
        if (!child) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        if (stat(child, &st) != 0) {
            ESP_LOGE(TAG, "stat during removal failed: path=%s errno=%d", child, errno);
            err = ESP_FAIL;
        } else if (S_ISDIR(st.st_mode)) {
            err = remove_directory_recursive(child);
        } else if (remove(child) != 0) {
            ESP_LOGE(TAG, "remove file failed: path=%s errno=%d", child, errno);
            err = ESP_FAIL;
        }
        free(child);
        if (err != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    if (err == ESP_OK && rmdir(path) != 0) {
        ESP_LOGE(TAG, "remove directory failed: path=%s errno=%d", path, errno);
        err = ESP_FAIL;
    }
    return err;
}

static void claw_skill_notify_registry_changed(void)
{
    claw_skill_registry_listener_t
        listeners[CLAW_SKILL_MAX_REGISTRY_LISTENERS] = {0};

    if (!s_skill || !s_skill->registry_lock) {
        return;
    }
    xSemaphoreTake(s_skill->registry_lock, portMAX_DELAY);
    memcpy(listeners, s_skill->registry_listeners, sizeof(listeners));
    xSemaphoreGive(s_skill->registry_lock);

    for (size_t i = 0; i < CLAW_SKILL_MAX_REGISTRY_LISTENERS; i++) {
        if (listeners[i].callback) {
            listeners[i].callback(listeners[i].user_ctx);
        }
    }
}

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
    free(entry->skill_dir);
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
    for (i = 0; i < s_skill->root_count; i++) {
        free(s_skill->roots[i]);
    }
    free(s_skill->roots);
    if (s_skill->registry_lock) {
        vSemaphoreDelete(s_skill->registry_lock);
    }
    if (s_skill->session_lock) {
        vSemaphoreDelete(s_skill->session_lock);
    }
    memset(s_skill, 0, sizeof(*s_skill));
    free(s_skill);
    s_skill = NULL;
}

/* Append a skills directory to the dynamic roots list. Entries borrow these
 * strings via claw_skill_registry_entry_t.root_dir, so they stay valid until
 * claw_skill_reset(). */
static esp_err_t append_root_dir(const char *dir)
{
    char **grown;
    char *copy;

    if (!s_skill || !dir || !dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    copy = strdup(dir);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    grown = realloc(s_skill->roots, sizeof(char *) * (s_skill->root_count + 1));
    if (!grown) {
        free(copy);
        return ESP_ERR_NO_MEM;
    }
    s_skill->roots = grown;
    s_skill->roots[s_skill->root_count] = copy;
    s_skill->root_count++;
    return ESP_OK;
}

static bool is_skill_document_file(const char *name)
{
    const char *base;

    if (!name) {
        return false;
    }

    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    return strcmp(base, SKILL_DOCUMENT_NAME) == 0;
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

static char *build_skill_path_dup(const char *root_dir, const char *filename)
{
    if (!root_dir || !filename) {
        return NULL;
    }

    return dup_printf("%s/%s", root_dir, filename);
}

static char *build_skill_dir_dup(const char *root_dir, const char *skill_id)
{
    if (!root_dir || !skill_id || !skill_id[0]) {
        return NULL;
    }

    return dup_printf("%s/%s", root_dir, skill_id);
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
    if (read_bytes != (size_t)size || ferror(file)) {
        ESP_LOGE(TAG, "read incomplete: %s", path);
        free(data);
        fclose(file);
        return ESP_FAIL;
    }
    fclose(file);
    data[read_bytes] = '\0';
    *out_data = data;
    return ESP_OK;
}

static bool skill_frontmatter_complete(const char *text, size_t len, bool eof, size_t *scan_offset)
{
    const char *start = text + *scan_offset;
    if (strstr(start, "\n---\n") || strstr(start, "\n---\r\n")) {
        return true;
    }
    if (eof && ((len >= 4 && memcmp(text + len - 4, "\n---", 4) == 0) || (len >= 5 && memcmp(text + len - 5, "\n---\r", 5) == 0))) {
        return true;
    }
    const size_t overlap = strlen("\n---\r\n") - 1;
    *scan_offset = len > overlap ? len - overlap : 0;
    return false;
}

static esp_err_t read_skill_frontmatter_dup(const char *path, size_t max_bytes, char **out_data)
{
    FILE *file = NULL;
    char *data = NULL;
    size_t used = 0;
    size_t scan_offset = 0;
    bool complete = false;

    if (!path || !out_data || max_bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "read skill metadata open: %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    data = calloc(1, max_bytes + 1);
    if (!data) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    while (used < max_bytes) {
        const size_t remaining = max_bytes - used;
        const size_t chunk = remaining < CLAW_SKILL_FRONTMATTER_READ_CHUNK ? remaining : CLAW_SKILL_FRONTMATTER_READ_CHUNK;
        const size_t read_bytes = fread(data + used, 1, chunk, file);
        used += read_bytes;
        data[used] = '\0';
        const bool eof = read_bytes < chunk || used == max_bytes;
        complete = skill_frontmatter_complete(data, used, eof, &scan_offset);
        if (complete || eof) {
            break;
        }
    }
    const bool read_failed = ferror(file) != 0;
    fclose(file);
    if (read_failed || !complete) {
        free(data);
        return read_failed ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
    }
    *out_data = data;
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

/* Caller must hold registry_lock. */
static const claw_skill_registry_entry_t *claw_skill_find_entry_locked(const char *skill_id)
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

    if (strlen(cursor) >= 3 && (unsigned char)cursor[0] == 0xEF && (unsigned char)cursor[1] == 0xBB && (unsigned char)cursor[2] == 0xBF) {
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
        ESP_LOGW(TAG, "invalid frontmatter: %s", filename);
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
        ESP_LOGW(TAG, "invalid metadata JSON: %s", filename);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    metadata = cJSON_GetObjectItemCaseSensitive(root, "metadata");
    if (metadata && !cJSON_IsObject(metadata)) {
        ESP_LOGW(TAG, "metadata must be an object: %s", filename);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    err = json_dup_required_string(root, "name", &entry->id);
    if (err == ESP_OK) {
        entry->file = strdup(filename);
        err = entry->file ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        entry->skill_dir = build_skill_dir_dup(entry->root_dir, entry->id);
        err = entry->skill_dir ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = json_dup_required_string(root, "description", &entry->summary);
    }
    if (err == ESP_OK && metadata) {
        err = json_dup_optional_unique_string_array(metadata, "cap_groups", &entry->cap_groups, &entry->cap_group_count);
    }
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "invalid metadata fields: %s", filename);
    }

    cJSON_Delete(root);
    return err;
}

static esp_err_t validate_registry_entry(claw_skill_registry_entry_t *entry)
{
    char expected_file[CLAW_SKILL_MAX_PATH] = {0};
    size_t i;

    if (!entry || !entry->id || !entry->file || !entry->summary) {
        ESP_LOGW(TAG, "skill metadata has missing fields");
        return ESP_ERR_INVALID_ARG;
    }
    if (!claw_skill_id_is_valid(entry->id)) {
        ESP_LOGW(TAG, "invalid skill id: %s", entry->id ? entry->id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!skill_path_is_valid(entry->file) || !is_skill_document_file(entry->file)) {
        ESP_LOGW(TAG, "invalid skill path: id=%s file=%s", entry->id ? entry->id : "(null)", entry->file ? entry->file : "(null)");
        return ESP_ERR_INVALID_ARG;
    }
    if (snprintf(expected_file, sizeof(expected_file), "%s/%s", entry->id, SKILL_DOCUMENT_NAME) >= (int)sizeof(expected_file)) {
        ESP_LOGW(TAG, "skill expected path too long: id=%s", entry->id);
        return ESP_ERR_INVALID_SIZE;
    }
    if (strcmp(entry->file, expected_file) != 0) {
        ESP_LOGW(TAG, "skill path must be %s, got %s", expected_file, entry->file);
        return ESP_ERR_INVALID_ARG;
    }
    for (i = 0; i < entry->cap_group_count; i++) {
        if (!entry->cap_groups[i] || !entry->cap_groups[i][0]) {
            ESP_LOGW(TAG, "invalid skill cap_group: id=%s idx=%u", entry->id ? entry->id : "(null)", (unsigned)i);
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (entry->manage_mode != CLAW_SKILL_MANAGE_MODE_READONLY && entry->manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME) {
        ESP_LOGW(TAG, "invalid skill mode: %s", entry->id ? entry->id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

static esp_err_t load_registry_dir_recursive(const char *root_dir,
                                             const char *relative_dir,
                                             claw_skill_registry_entry_t **entries,
                                             size_t *entry_count)
{
    DIR *dir = NULL;
    struct dirent *item = NULL;
    char *dir_path = NULL;
    bool has_skill_doc = false;
    esp_err_t err = ESP_OK;

    if (!s_skill || !root_dir || !root_dir[0] || !entries || !entry_count) {
        return ESP_ERR_INVALID_STATE;
    }

    dir_path = relative_dir && relative_dir[0] ? build_skill_path_dup(root_dir, relative_dir) : strdup(root_dir);
    if (!dir_path) {
        return ESP_ERR_NO_MEM;
    }
    dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "open skills dir %s failed", dir_path);
        free(dir_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Skills are leaves per the Skill package spec: when this directory already
     * holds SKILL.md, its content subdirs (scripts/, references/, assets/)
     * are skill payload, not nested skills. Detect that up front so the loop
     * below can skip descending into them — this both bounds recursion depth
     * (which protects the main-task stack) and avoids reading skill payload
     * files when looking for skill documents. */
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] && is_skill_document_file(item->d_name)) {
            has_skill_doc = true;
            break;
        }
    }
    rewinddir(dir);

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
                ESP_LOGW(TAG, "skill path too long under %s, skipping", dir_path);
                continue;
            }
        } else {
            if (snprintf(relative_path, sizeof(relative_path), "%s", item->d_name) >= (int)sizeof(relative_path)) {
                ESP_LOGW(TAG, "skill path too long under %s, skipping", dir_path);
                continue;
            }
        }
        path = build_skill_path_dup(root_dir, relative_path);
        if (!path) {
            err = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        if (stat(path, &st) != 0) {
            ESP_LOGW(TAG, "stat skill path failed, skipping: path=%s errno=%d", path, errno);
            free(path);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            free(path);
            if (has_skill_doc) {
                /* Don't descend into the current skill's content subtree. */
                continue;
            }
            err = load_registry_dir_recursive(root_dir, relative_path, entries, entry_count);
            if (err != ESP_OK) {
                goto cleanup;
            }
            continue;
        }
        if (!S_ISREG(st.st_mode) || !is_skill_document_file(relative_path)) {
            free(path);
            continue;
        }
        if (st.st_size < 0 || (size_t)st.st_size > s_skill->max_file_bytes) {
            ESP_LOGW(TAG, "skill file exceeds limit, skipping: %s", relative_path);
            free(path);
            continue;
        }
        if (*entry_count >= CLAW_SKILL_MAX_FILES) {
            ESP_LOGE(TAG, "too many skill files (cap %d) under %s", CLAW_SKILL_MAX_FILES, root_dir);
            err = ESP_ERR_INVALID_SIZE;
            free(path);
            goto cleanup;
        }

        esp_err_t skill_err = read_skill_frontmatter_dup(path, s_skill->max_file_bytes, &text);
        free(path);
        if (skill_err != ESP_OK) {
            if (skill_err == ESP_ERR_NO_MEM) {
                err = skill_err;
                goto cleanup;
            }
            ESP_LOGW(TAG, "read skill file failed, skipping: file=%s err=%s", relative_path, esp_err_to_name(skill_err));
            continue;
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
        entry->root_dir = root_dir;
        /* The primary root is writable; later roots are firmware-owned. */
        entry->manage_mode = root_dir == s_skill->roots[0] ? CLAW_SKILL_MANAGE_MODE_RUNTIME : CLAW_SKILL_MANAGE_MODE_READONLY;

        skill_err = parse_skill_document_metadata(relative_path, text, entry);
        free(text);
        if (skill_err != ESP_OK) {
            free_registry_entry(entry);
            if (skill_err == ESP_ERR_NO_MEM) {
                err = skill_err;
                goto cleanup;
            }
            ESP_LOGW(TAG, "invalid skill metadata, skipping: file=%s err=%s", relative_path, esp_err_to_name(skill_err));
            continue;
        }

        skill_err = validate_registry_entry(entry);
        if (skill_err != ESP_OK) {
            free_registry_entry(entry);
            ESP_LOGW(TAG, "invalid skill definition, skipping: file=%s err=%s", relative_path, esp_err_to_name(skill_err));
            continue;
        }

        /* A skill id already loaded from an earlier (higher-priority) root wins;
         * the copy in this root is shadowed. This lets the writable partition
         * override a firmware-baked skill of the same id. */
        bool shadowed = false;
        for (i = 0; i < *entry_count; i++) {
            if (strcmp((*entries)[i].id, entry->id) == 0) {
                ESP_LOGW(TAG, "skill id %s in %s shadowed by %s",
                         entry->id, root_dir, (*entries)[i].root_dir);
                shadowed = true;
                break;
            }
        }
        if (shadowed) {
            free_registry_entry(entry);
            continue;
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
    size_t r;

    /* Roots are scanned in priority order: roots[0] (writable) first, so a
     * skill there shadows a same-id skill in a later read-only root. */
    for (r = 0; r < s_skill->root_count; r++) {
        struct stat st = {0};

        /* A root may legitimately be absent (e.g. no system skills partition);
         * skip it rather than failing the whole load. */
        if (stat(s_skill->roots[r], &st) != 0 || !S_ISDIR(st.st_mode)) {
            ESP_LOGW(TAG, "skills root %s not present, skipping", s_skill->roots[r]);
            continue;
        }

        err = load_registry_dir_recursive(s_skill->roots[r], NULL, &entries, &entry_count);
        if (err != ESP_OK) {
            free_registry_entries(entries, entry_count);
            return err;
        }
    }

    if (entry_count == 0) {
        /* Empty is allowed when registered roots contain no skills. */
        ESP_LOGW(TAG, "no skill markdown found in any skills root (registry empty)");
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

static esp_err_t read_skill_document(const char *skill_id, char *buf, size_t size)
{
    const claw_skill_registry_entry_t *entry = NULL;
    char *path = NULL;
    char *text = NULL;
    char *skill_dir = NULL;
    const char *json_start = NULL;
    const char *json_end = NULL;
    const char *body = NULL;
    size_t max_file_bytes;
    size_t prefix_len;
    size_t body_required_len;
    size_t required_len;
    esp_err_t err;

    if (!s_skill || !s_skill->initialized) {
        ESP_LOGE(TAG, "read doc: not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (!skill_id || !skill_id[0] || !buf || size == 0) {
        ESP_LOGE(TAG, "read doc: bad arg");
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';

    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    entry = claw_skill_find_entry_locked(skill_id);
    if (!entry) {
        xSemaphoreGive(s_skill->registry_lock);
        ESP_LOGE(TAG, "read doc %s: not found", skill_id);
        return ESP_ERR_NOT_FOUND;
    }

    path = build_skill_path_dup(entry->root_dir, entry->file);
    skill_dir = entry->skill_dir ? strdup(entry->skill_dir) : NULL;
    max_file_bytes = s_skill->max_file_bytes;
    xSemaphoreGive(s_skill->registry_lock);
    if (!path || !skill_dir) {
        ESP_LOGE(TAG, "read doc %s: no path", skill_id);
        free(path);
        free(skill_dir);
        return ESP_ERR_NO_MEM;
    }
    CLAW_SKILL_DIAGI("read doc %s", skill_id);
    err = read_file_dup(path, max_file_bytes, &text);
    free(path);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read doc %s: %s", skill_id, esp_err_to_name(err));
        free(skill_dir);
        return err;
    }

    err = extract_skill_frontmatter_json(text, &json_start, &json_end, &body);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read doc %s: invalid frontmatter", skill_id);
        free(text);
        free(skill_dir);
        return err;
    }
    (void)json_start;
    (void)json_end;
    prefix_len = (size_t)(body - text);

    err = str_replace_required_len(body, "{CUR_SKILL_DIR}", skill_dir, &body_required_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read doc %s: size calculation failed", skill_id);
        free(text);
        free(skill_dir);
        return err;
    }
    if (SIZE_MAX - prefix_len < body_required_len) {
        free(text);
        free(skill_dir);
        return ESP_ERR_INVALID_SIZE;
    }
    required_len = prefix_len + body_required_len;
    if (required_len >= size) {
        ESP_LOGE(TAG, "read doc %s: document too large", skill_id);
        free(text);
        free(skill_dir);
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(buf, size, "%s", text);
    free(text);

    /* Frontmatter is metadata; expand paths only in the document body. */
    str_replace_inplace(buf + prefix_len, size - prefix_len, "{CUR_SKILL_DIR}", skill_dir);
    free(skill_dir);
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

    /* Session state file may not exist for new sessions. */
    struct stat st = {0};
    if (stat(path, &st) != 0) {
        int stat_errno = errno;
        free(path);
        return stat_errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    if (!S_ISREG(st.st_mode)) {
        free(path);
        return ESP_ERR_INVALID_STATE;
    }

    err = read_file_dup(path, CLAW_SKILL_SESSION_STATE_MAX_BYTES, &json_text);
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

    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        cJSON_Delete(root);
        return ESP_ERR_TIMEOUT;
    }
    for (i = 0; i < (size_t)cJSON_GetArraySize(root); i++) {
        cJSON *item = cJSON_GetArrayItem(root, (int)i);

        if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
            continue;
        }
        if (!claw_skill_find_entry_locked(item->valuestring)) {
            continue;
        }
        err = push_unique_string(&loaded, &loaded_count, item->valuestring);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "store skill %s: %s", item->valuestring, esp_err_to_name(err));
            free_string_array(loaded, loaded_count);
            xSemaphoreGive(s_skill->registry_lock);
            cJSON_Delete(root);
            return err;
        }
    }
    xSemaphoreGive(s_skill->registry_lock);

    cJSON_Delete(root);
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
        if (remove(path) != 0 && errno != ENOENT) {
            ESP_LOGE(TAG, "remove session %s failed: errno=%d", path, errno);
            free(path);
            return ESP_FAIL;
        }
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

    if (strlen(json_text) > CLAW_SKILL_SESSION_STATE_MAX_BYTES) {
        err = ESP_ERR_INVALID_SIZE;
        goto cleanup;
    }
    err = claw_utils_file_write_atomic(path, json_text, strlen(json_text));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write session %s: %s", session_id, esp_err_to_name(err));
    }

cleanup:
    free(path);
    cJSON_Delete(root);
    free(json_text);
    return err;
}

esp_err_t claw_skill_delete_session_state(const char *session_id,
                                          bool *out_deleted_any)
{
    char *path = NULL;

    if (!out_deleted_any) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_deleted_any = false;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0]) {
        ESP_LOGE(TAG, "delete session: bad state");
        return ESP_ERR_INVALID_STATE;
    }

    path = build_session_state_path_dup(session_id);
    if (!path) {
        ESP_LOGE(TAG, "session path %s", session_id ? session_id : "(null)");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_skill->session_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        free(path);
        return ESP_ERR_TIMEOUT;
    }
    if (remove(path) == 0) {
        *out_deleted_any = true;
        xSemaphoreGive(s_skill->session_lock);
        free(path);
        return ESP_OK;
    }
    if (errno == ENOENT) {
        xSemaphoreGive(s_skill->session_lock);
        free(path);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "delete session state %s failed: errno=%d", path, errno);
    xSemaphoreGive(s_skill->session_lock);
    free(path);
    return ESP_FAIL;
}

esp_err_t claw_skill_init(const claw_skill_config_t *config)
{
    esp_err_t err;
    size_t root_len;

    if (!config || !config->session_state_root_dir || !config->session_state_root_dir[0] || config->max_file_bytes == 0 || config->max_file_bytes == SIZE_MAX) {
        ESP_LOGE(TAG, "init: bad config");
        return ESP_ERR_INVALID_ARG;
    }
    root_len = strnlen(config->session_state_root_dir, CLAW_SKILL_MAX_PATH);
    if (root_len >= CLAW_SKILL_MAX_PATH) {
        ESP_LOGE(TAG, "init: session path too long");
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_skill && s_skill->initialized) {
        return strcmp(s_skill->session_state_root_dir, config->session_state_root_dir) == 0 && s_skill->max_file_bytes == config->max_file_bytes
               ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    s_skill = calloc(1, sizeof(*s_skill));
    ESP_RETURN_ON_FALSE(s_skill != NULL, ESP_ERR_NO_MEM, TAG, "alloc skill registry failed");
    s_skill->registry_lock = xSemaphoreCreateMutex();
    s_skill->session_lock = xSemaphoreCreateMutex();
    if (!s_skill->registry_lock || !s_skill->session_lock) {
        claw_skill_reset();
        return ESP_ERR_NO_MEM;
    }
    safe_copy(s_skill->session_state_root_dir, sizeof(s_skill->session_state_root_dir), config->session_state_root_dir);
    s_skill->max_file_bytes = config->max_file_bytes;

    err = ensure_dir(s_skill->session_state_root_dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init dir %s: %s", s_skill->session_state_root_dir, esp_err_to_name(err));
        claw_skill_reset();
        return err;
    }

    /* The registry starts empty until scan roots are registered. */
    s_skill->initialized = 1;
    ESP_LOGI(TAG, "Initialized skill registry (awaiting directories)");
    return ESP_OK;
}

esp_err_t claw_skill_add_directory(const char *dir)
{
    esp_err_t err;
    size_t i;

    if (!s_skill || !s_skill->initialized) {
        ESP_LOGE(TAG, "add dir before init");
        return ESP_ERR_INVALID_STATE;
    }
    if (!dir || !dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (i = 0; i < s_skill->root_count; i++) {
        if (strcmp(s_skill->roots[i], dir) == 0) {
            CLAW_SKILL_DIAGI("skills dir %s already registered, skipping", dir);
            xSemaphoreGive(s_skill->registry_lock);
            return ESP_OK;
        }
    }

    err = append_root_dir(dir);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add skills dir %s: %s", dir, esp_err_to_name(err));
        xSemaphoreGive(s_skill->registry_lock);
        return err;
    }
    xSemaphoreGive(s_skill->registry_lock);
    return ESP_OK;
}

static esp_err_t claw_skill_reload_registry_locked(void)
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

esp_err_t claw_skill_reload_registry(void)
{
    if (!s_skill || !s_skill->initialized || !s_skill->registry_lock) {
        ESP_LOGE(TAG, "reload before init");
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = claw_skill_reload_registry_locked();
    xSemaphoreGive(s_skill->registry_lock);
    if (err == ESP_OK) {
        claw_skill_notify_registry_changed();
    }
    return err;
}

bool claw_skill_id_is_valid(const char *skill_id)
{
    size_t len;

    if (!skill_id || !skill_id[0]) {
        return false;
    }
    len = strlen(skill_id);
    if (len > CLAW_SKILL_ID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = skill_id[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

esp_err_t claw_skill_publish(const char *skill_id)
{
    const claw_skill_registry_entry_t *entry;
    esp_err_t err;
    bool reload_succeeded;

    if (!claw_skill_id_is_valid(skill_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_skill || !s_skill->initialized || !s_skill->registry_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_skill->root_count == 0) {
        xSemaphoreGive(s_skill->registry_lock);
        return ESP_ERR_INVALID_STATE;
    }
    err = claw_skill_reload_registry_locked();
    reload_succeeded = err == ESP_OK;
    if (err == ESP_OK) {
        entry = claw_skill_find_entry_locked(skill_id);
        if (!entry) {
            err = ESP_ERR_NOT_FOUND;
        } else if (entry->manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME || entry->root_dir != s_skill->roots[0]) {
            err = ESP_ERR_INVALID_STATE;
        }
    }
    xSemaphoreGive(s_skill->registry_lock);

    /* A successful reload is visible even when the requested skill is invalid. */
    if (reload_succeeded) {
        claw_skill_notify_registry_changed();
    }
    return err;
}

esp_err_t claw_skill_remove(const char *skill_id)
{
    const claw_skill_registry_entry_t *entry;
    char *skill_dir = NULL;
    esp_err_t err;
    esp_err_t remove_err;
    bool reload_succeeded = false;

    if (!claw_skill_id_is_valid(skill_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_skill || !s_skill->initialized || !s_skill->registry_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_skill->root_count == 0) {
        xSemaphoreGive(s_skill->registry_lock);
        return ESP_ERR_INVALID_STATE;
    }

    entry = claw_skill_find_entry_locked(skill_id);
    if (!entry) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (entry->manage_mode != CLAW_SKILL_MANAGE_MODE_RUNTIME || entry->root_dir != s_skill->roots[0]) {
        err = ESP_ERR_INVALID_STATE;
        goto cleanup;
    }
    skill_dir = strdup(entry->skill_dir);
    if (!skill_dir) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }
    remove_err = remove_directory_recursive(skill_dir);
    if (remove_err != ESP_OK) {
        ESP_LOGE(TAG, "remove skill directory failed: id=%s err=%s", skill_id, esp_err_to_name(remove_err));
    }

    err = claw_skill_reload_registry_locked();
    reload_succeeded = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reload registry after removing skill failed: id=%s err=%s", skill_id, esp_err_to_name(err));
    }
    if (remove_err != ESP_OK) {
        err = remove_err;
    }

cleanup:
    xSemaphoreGive(s_skill->registry_lock);
    free(skill_dir);
    if (reload_succeeded) {
        claw_skill_notify_registry_changed();
    }
    return err;
}

esp_err_t claw_skill_register_registry_changed_cb(
    claw_skill_registry_changed_cb_t callback,
    void *user_ctx)
{
    size_t free_index = CLAW_SKILL_MAX_REGISTRY_LISTENERS;

    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_skill || !s_skill->initialized || !s_skill->registry_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < CLAW_SKILL_MAX_REGISTRY_LISTENERS; i++) {
        claw_skill_registry_listener_t *listener =
            &s_skill->registry_listeners[i];

        if (listener->callback == callback && listener->user_ctx == user_ctx) {
            xSemaphoreGive(s_skill->registry_lock);
            return ESP_OK;
        }
        if (!listener->callback &&
                free_index == CLAW_SKILL_MAX_REGISTRY_LISTENERS) {
            free_index = i;
        }
    }
    if (free_index == CLAW_SKILL_MAX_REGISTRY_LISTENERS) {
        xSemaphoreGive(s_skill->registry_lock);
        return ESP_ERR_NO_MEM;
    }
    s_skill->registry_listeners[free_index].callback = callback;
    s_skill->registry_listeners[free_index].user_ctx = user_ctx;
    xSemaphoreGive(s_skill->registry_lock);
    return ESP_OK;
}

static void fill_catalog_entry_view(const claw_skill_registry_entry_t *entry, claw_skill_catalog_entry_t *out_entry)
{
    memset(out_entry, 0, sizeof(*out_entry));
    out_entry->id = entry->id;
    out_entry->file = entry->file;
    out_entry->summary = entry->summary;
    out_entry->cap_groups = (const char *const *)entry->cap_groups;
    out_entry->cap_group_count = entry->cap_group_count;
    out_entry->manage_mode = entry->manage_mode;
    out_entry->skill_dir = entry->skill_dir;
}

esp_err_t claw_skill_foreach_catalog_entry(claw_skill_catalog_cb_t cb, void *user_ctx)
{
    size_t i;

    if (!s_skill || !s_skill->initialized) {
        ESP_LOGD(TAG, "foreach before init");
        return ESP_ERR_INVALID_STATE;
    }
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_skill->registry_lock ||
            xSemaphoreTake(s_skill->registry_lock,
                           pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (i = 0; i < s_skill->entry_count; i++) {
        claw_skill_catalog_entry_t view;
        esp_err_t err;

        fill_catalog_entry_view(&s_skill->entries[i], &view);
        err = cb(&view, user_ctx);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "catalog iteration stopped: id=%s err=%s", view.id ? view.id : "(null)", esp_err_to_name(err));
            xSemaphoreGive(s_skill->registry_lock);
            return err;
        }
    }
    xSemaphoreGive(s_skill->registry_lock);
    return ESP_OK;
}

esp_err_t claw_skill_load_active_skill_ids(const char *session_id,
                                           char ***out_skill_ids,
                                           size_t *out_skill_count)
{
    esp_err_t err;

    if (!s_skill || !s_skill->initialized || !s_skill->session_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_skill->session_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = load_active_skill_ids_from_disk(session_id, out_skill_ids, out_skill_count);
    xSemaphoreGive(s_skill->session_lock);
    return err;
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

    err = claw_skill_load_active_skill_ids(session_id, &active_skill_ids, &active_skill_count);
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        free_string_array(active_skill_ids, active_skill_count);
        return ESP_ERR_TIMEOUT;
    }
    for (i = 0; i < active_skill_count; i++) {
        const claw_skill_registry_entry_t *entry = claw_skill_find_entry_locked(active_skill_ids[i]);

        if (!entry) {
            continue;
        }

        for (j = 0; j < entry->cap_group_count; j++) {
            err = push_unique_string(&group_ids, &group_count, entry->cap_groups[j]);
            if (err != ESP_OK) {
                xSemaphoreGive(s_skill->registry_lock);
                free_string_array(active_skill_ids, active_skill_count);
                free_string_array(group_ids, group_count);
                return err;
            }
        }
    }
    xSemaphoreGive(s_skill->registry_lock);

    free_string_array(active_skill_ids, active_skill_count);
    *out_group_ids = group_ids;
    *out_group_count = group_count;
    return ESP_OK;
}

esp_err_t claw_skill_activate_for_session(const char *session_id, const char *skill_id, char *document, size_t document_size)
{
    char **active = NULL;
    size_t active_count = 0;
    esp_err_t err;

    if (!s_skill || !s_skill->initialized || !session_id || !session_id[0] || !skill_id || !skill_id[0] || !document || document_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    err = read_skill_document(skill_id, document, document_size);
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_skill->session_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = load_active_skill_ids_from_disk(session_id, &active, &active_count);
    if (err != ESP_OK) {
        xSemaphoreGive(s_skill->session_lock);
        return err;
    }
    if (xSemaphoreTake(s_skill->registry_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        free_string_array(active, active_count);
        xSemaphoreGive(s_skill->session_lock);
        return ESP_ERR_TIMEOUT;
    }
    if (!claw_skill_find_entry_locked(skill_id)) {
        free_string_array(active, active_count);
        xSemaphoreGive(s_skill->registry_lock);
        xSemaphoreGive(s_skill->session_lock);
        return ESP_ERR_NOT_FOUND;
    }

    err = push_unique_string(&active, &active_count, skill_id);
    if (err != ESP_OK) {
        free_string_array(active, active_count);
        xSemaphoreGive(s_skill->registry_lock);
        xSemaphoreGive(s_skill->session_lock);
        return err;
    }

    err = save_active_skill_ids_to_disk(session_id, (const char *const *)active, active_count);
    free_string_array(active, active_count);
    xSemaphoreGive(s_skill->registry_lock);
    xSemaphoreGive(s_skill->session_lock);
    return err;
}
