/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_registry.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define APP_REGISTRY_MAX_ENTRIES 64
#define APP_REGISTRY_MAX_ROOTS 4
#define APP_REGISTRY_MAX_LISTENERS 4
#define APP_REGISTRY_MANIFEST_MAX_BYTES 4096
#define APP_REGISTRY_SCHEMA_VERSION 1
#define APP_REGISTRY_LOCK_TIMEOUT_MS 5000
#define APP_REGISTRY_MANIFEST_NAME "launcher.json"

static const char *TAG = "app_registry";

typedef struct {
    char *app_id;
    char *app_dir;
    char *display_name;
    char *entry;
    char *icon;
    char *args_json;
    int order;
    bool visible;
    app_registry_manage_mode_t manage_mode;
} app_registry_owned_entry_t;

typedef struct {
    app_registry_changed_cb_t callback;
    void *user_ctx;
} app_registry_listener_t;

typedef struct {
    bool initialized;
    SemaphoreHandle_t lock;
    char *roots[APP_REGISTRY_MAX_ROOTS];
    size_t root_count;
    app_registry_owned_entry_t *entries;
    size_t entry_count;
    app_registry_listener_t listeners[APP_REGISTRY_MAX_LISTENERS];
} app_registry_state_t;

static app_registry_state_t s_registry;

static char *path_join_dup(const char *dir, const char *name)
{
    int length;
    char *path;

    if (!dir || !name) {
        return NULL;
    }
    length = snprintf(NULL, 0, "%s/%s", dir, name);
    if (length < 0) {
        return NULL;
    }
    path = malloc((size_t)length + 1);
    if (path) {
        snprintf(path, (size_t)length + 1, "%s/%s", dir, name);
    }
    return path;
}

static bool has_suffix(const char *value, const char *suffix)
{
    size_t value_len;
    size_t suffix_len;

    if (!value || !suffix) {
        return false;
    }
    value_len = strlen(value);
    suffix_len = strlen(suffix);
    return value_len >= suffix_len && strcmp(value + value_len - suffix_len, suffix) == 0;
}

static bool relative_path_is_valid(const char *path)
{
    return path && path[0] && path[0] != '/' && !strstr(path, "..") && !strchr(path, '\\');
}

bool app_registry_id_is_valid(const char *app_id)
{
    size_t len;

    if (!app_id || !app_id[0]) {
        return false;
    }
    len = strlen(app_id);
    if (len > APP_REGISTRY_ID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char ch = app_id[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

static esp_err_t read_file_dup(const char *path, char **out_text)
{
    FILE *file;
    char *text;
    long size;
    size_t read_bytes;

    if (!path || !out_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;
    file = fopen(path, "rb");
    if (!file) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return ESP_FAIL;
    }
    size = ftell(file);
    if (size < 0 || size > APP_REGISTRY_MANIFEST_MAX_BYTES || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return size > APP_REGISTRY_MANIFEST_MAX_BYTES ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    }
    text = calloc(1, (size_t)size + 1);
    if (!text) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    read_bytes = fread(text, 1, (size_t)size, file);
    bool failed = ferror(file) != 0;
    fclose(file);
    if (failed || read_bytes != (size_t)size) {
        free(text);
        return ESP_FAIL;
    }
    *out_text = text;
    return ESP_OK;
}

static void free_entry(app_registry_owned_entry_t *entry)
{
    if (!entry) {
        return;
    }
    free(entry->app_id);
    free(entry->app_dir);
    free(entry->display_name);
    free(entry->entry);
    free(entry->icon);
    free(entry->args_json);
    memset(entry, 0, sizeof(*entry));
}

static void free_entries(app_registry_owned_entry_t *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free_entry(&entries[i]);
    }
    free(entries);
}

static esp_err_t parse_manifest(const char *app_id, const char *app_dir, app_registry_manage_mode_t manage_mode, app_registry_owned_entry_t *out)
{
    char *manifest_path = path_join_dup(app_dir, APP_REGISTRY_MANIFEST_NAME);
    char *text = NULL;
    cJSON *root = NULL;
    esp_err_t err;

    if (!manifest_path || !out) {
        free(manifest_path);
        return !out ? ESP_ERR_INVALID_ARG : ESP_ERR_NO_MEM;
    }
    err = read_file_dup(manifest_path, &text);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "app manifest missing: id=%s path=%s", app_id, manifest_path);
        } else {
            ESP_LOGW(TAG, "read app manifest failed: id=%s err=%s", app_id, esp_err_to_name(err));
        }
        free(manifest_path);
        return err;
    }
    root = cJSON_ParseWithOpts(text, NULL, true);
    free(text);
    if (!cJSON_IsObject(root)) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, "entry");
    cJSON *icon = cJSON_GetObjectItemCaseSensitive(root, "icon");
    cJSON *display_name = cJSON_GetObjectItemCaseSensitive(root, "display_name");
    cJSON *args = cJSON_GetObjectItemCaseSensitive(root, "args");
    cJSON *order = cJSON_GetObjectItemCaseSensitive(root, "order");
    cJSON *visible = cJSON_GetObjectItemCaseSensitive(root, "visible");
    bool has_icon = icon != NULL;
    bool has_args = args != NULL;
    if (!cJSON_IsNumber(schema) || schema->valuedouble != (double)schema->valueint || schema->valueint != APP_REGISTRY_SCHEMA_VERSION ||
            !cJSON_IsString(id) || !id->valuestring || !app_registry_id_is_valid(id->valuestring) || strcmp(id->valuestring, app_id) != 0 ||
            !cJSON_IsString(entry) || !entry->valuestring || !relative_path_is_valid(entry->valuestring) || !has_suffix(entry->valuestring, ".lua") ||
            (has_icon && (!cJSON_IsString(icon) || !relative_path_is_valid(icon->valuestring) || (!has_suffix(icon->valuestring, ".jpg") && !has_suffix(icon->valuestring, ".jpeg")))) ||
            (display_name && (!cJSON_IsString(display_name) || !display_name->valuestring || !display_name->valuestring[0])) || (has_args && !cJSON_IsObject(args)) ||
            (order && (!cJSON_IsNumber(order) || order->valuedouble != (double)order->valueint)) || (visible && !cJSON_IsBool(visible))) {
        err = ESP_ERR_INVALID_ARG;
        goto cleanup;
    }

    out->app_id = strdup(app_id);
    out->app_dir = strdup(app_dir);
    out->display_name = strdup(display_name ? display_name->valuestring : app_id);
    out->entry = path_join_dup(app_dir, entry->valuestring);
    out->icon = has_icon ? path_join_dup(app_dir, icon->valuestring) : NULL;
    out->args_json = has_args ? cJSON_PrintUnformatted(args) : strdup("{}");
    out->order = order ? order->valueint : 0;
    out->visible = visible ? cJSON_IsTrue(visible) : true;
    out->manage_mode = manage_mode;
    if (!out->app_id || !out->app_dir || !out->display_name || !out->entry || !out->args_json || (has_icon && !out->icon)) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    struct stat st = {0};
    if (stat(out->entry, &st) != 0 || !S_ISREG(st.st_mode)) {
        ESP_LOGW(TAG, "app entry missing: id=%s path=%s", app_id, out->entry);
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }
    if (out->icon && (stat(out->icon, &st) != 0 || !S_ISREG(st.st_mode))) {
        ESP_LOGW(TAG, "app icon missing, using default: id=%s path=%s", app_id, out->icon);
        free(out->icon);
        out->icon = NULL;
    }
    err = ESP_OK;

cleanup:
    if (err != ESP_OK) {
        free_entry(out);
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "invalid app manifest: id=%s path=%s err=%s", app_id, manifest_path, esp_err_to_name(err));
        }
    }
    cJSON_Delete(root);
    free(manifest_path);
    return err;
}

static bool entry_exists(const app_registry_owned_entry_t *entries, size_t count, const char *app_id)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].app_id, app_id) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t append_entry(app_registry_owned_entry_t **entries, size_t *count, app_registry_owned_entry_t *entry)
{
    app_registry_owned_entry_t *grown;

    if (*count >= APP_REGISTRY_MAX_ENTRIES) {
        return ESP_ERR_INVALID_SIZE;
    }
    grown = realloc(*entries, sizeof(*grown) * (*count + 1));
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }
    *entries = grown;
    (*entries)[*count] = *entry;
    memset(entry, 0, sizeof(*entry));
    (*count)++;
    return ESP_OK;
}

static esp_err_t load_root(const char *root_dir, app_registry_manage_mode_t manage_mode, app_registry_owned_entry_t **entries, size_t *count)
{
    DIR *dir;
    struct dirent *item;

    dir = opendir(root_dir);
    if (!dir) {
        if (errno == ENOENT) {
            ESP_LOGI(TAG, "apps root not present, skipping: %s", root_dir);
            return ESP_OK;
        }
        ESP_LOGE(TAG, "open apps root failed: path=%s errno=%d", root_dir, errno);
        return ESP_FAIL;
    }
    while ((item = readdir(dir)) != NULL) {
        char *app_dir;
        struct stat st = {0};
        app_registry_owned_entry_t entry = {0};

        if (item->d_name[0] == '.' || !app_registry_id_is_valid(item->d_name)) {
            continue;
        }
        app_dir = path_join_dup(root_dir, item->d_name);
        if (!app_dir) {
            closedir(dir);
            return ESP_ERR_NO_MEM;
        }
        if (stat(app_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            free(app_dir);
            continue;
        }
        if (entry_exists(*entries, *count, item->d_name)) {
            ESP_LOGW(TAG, "app id %s in %s is shadowed", item->d_name, root_dir);
            free(app_dir);
            continue;
        }
        esp_err_t err = parse_manifest(item->d_name, app_dir, manage_mode, &entry);
        free(app_dir);
        if (err == ESP_ERR_NO_MEM) {
            closedir(dir);
            return err;
        }
        if (err != ESP_OK) {
            continue;
        }
        err = append_entry(entries, count, &entry);
        if (err != ESP_OK) {
            free_entry(&entry);
            closedir(dir);
            return err;
        }
    }
    closedir(dir);
    return ESP_OK;
}

static esp_err_t reload_registry_locked(void)
{
    app_registry_owned_entry_t *entries = NULL;
    size_t count = 0;

    for (size_t i = 0; i < s_registry.root_count; i++) {
        app_registry_manage_mode_t mode = i == 0 ? APP_REGISTRY_MANAGE_MODE_RUNTIME : APP_REGISTRY_MANAGE_MODE_READONLY;
        esp_err_t err = load_root(s_registry.roots[i], mode, &entries, &count);
        if (err != ESP_OK) {
            free_entries(entries, count);
            return err;
        }
    }
    app_registry_owned_entry_t *old_entries = s_registry.entries;
    size_t old_count = s_registry.entry_count;
    s_registry.entries = entries;
    s_registry.entry_count = count;
    free_entries(old_entries, old_count);
    ESP_LOGI(TAG, "Reloaded registry with %u app(s)", (unsigned)count);
    return ESP_OK;
}

static void notify_registry_changed(void)
{
    app_registry_listener_t listeners[APP_REGISTRY_MAX_LISTENERS] = {0};

    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    memcpy(listeners, s_registry.listeners, sizeof(listeners));
    xSemaphoreGive(s_registry.lock);
    for (size_t i = 0; i < APP_REGISTRY_MAX_LISTENERS; i++) {
        if (listeners[i].callback) {
            listeners[i].callback(listeners[i].user_ctx);
        }
    }
}

static esp_err_t remove_directory_recursive(const char *path)
{
    DIR *dir = opendir(path);
    struct dirent *item;
    esp_err_t err = ESP_OK;

    if (!dir) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    while ((item = readdir(dir)) != NULL) {
        char *child;
        struct stat st = {0};

        if (strcmp(item->d_name, ".") == 0 || strcmp(item->d_name, "..") == 0) {
            continue;
        }
        child = path_join_dup(path, item->d_name);
        if (!child) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        if (stat(child, &st) != 0) {
            err = ESP_FAIL;
        } else if (S_ISDIR(st.st_mode)) {
            err = remove_directory_recursive(child);
        } else if (remove(child) != 0) {
            err = ESP_FAIL;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "remove app path failed: path=%s errno=%d", child, errno);
        }
        free(child);
        if (err != ESP_OK) {
            break;
        }
    }
    closedir(dir);
    if (err == ESP_OK && rmdir(path) != 0) {
        ESP_LOGE(TAG, "remove app directory failed: path=%s errno=%d", path, errno);
        err = ESP_FAIL;
    }
    return err;
}

esp_err_t app_registry_init(void)
{
    if (s_registry.initialized) {
        return ESP_OK;
    }
    s_registry.lock = xSemaphoreCreateMutex();
    if (!s_registry.lock) {
        return ESP_ERR_NO_MEM;
    }
    s_registry.initialized = true;
    return ESP_OK;
}

esp_err_t app_registry_add_directory(const char *dir)
{
    char *copy;

    if (!dir || !dir[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registry.initialized || !s_registry.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_registry.lock, pdMS_TO_TICKS(APP_REGISTRY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < s_registry.root_count; i++) {
        if (strcmp(s_registry.roots[i], dir) == 0) {
            xSemaphoreGive(s_registry.lock);
            return ESP_OK;
        }
    }
    if (s_registry.root_count >= APP_REGISTRY_MAX_ROOTS) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_INVALID_SIZE;
    }
    copy = strdup(dir);
    if (!copy) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_NO_MEM;
    }
    s_registry.roots[s_registry.root_count++] = copy;
    xSemaphoreGive(s_registry.lock);
    return ESP_OK;
}

esp_err_t app_registry_reload(void)
{
    esp_err_t err;

    if (!s_registry.initialized || !s_registry.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_registry.lock, pdMS_TO_TICKS(APP_REGISTRY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = reload_registry_locked();
    xSemaphoreGive(s_registry.lock);
    if (err == ESP_OK) {
        notify_registry_changed();
    }
    return err;
}

esp_err_t app_registry_register_changed_cb(app_registry_changed_cb_t callback, void *user_ctx)
{
    size_t free_index = APP_REGISTRY_MAX_LISTENERS;

    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registry.initialized || !s_registry.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_registry.lock, pdMS_TO_TICKS(APP_REGISTRY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < APP_REGISTRY_MAX_LISTENERS; i++) {
        if (s_registry.listeners[i].callback == callback && s_registry.listeners[i].user_ctx == user_ctx) {
            xSemaphoreGive(s_registry.lock);
            return ESP_OK;
        }
        if (!s_registry.listeners[i].callback && free_index == APP_REGISTRY_MAX_LISTENERS) {
            free_index = i;
        }
    }
    if (free_index == APP_REGISTRY_MAX_LISTENERS) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_NO_MEM;
    }
    s_registry.listeners[free_index] = (app_registry_listener_t) {.callback = callback, .user_ctx = user_ctx};
    xSemaphoreGive(s_registry.lock);
    return ESP_OK;
}

esp_err_t app_registry_foreach(app_registry_entry_cb_t callback, void *user_ctx)
{
    if (!callback) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registry.initialized || !s_registry.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_registry.lock, pdMS_TO_TICKS(APP_REGISTRY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    for (size_t i = 0; i < s_registry.entry_count; i++) {
        const app_registry_owned_entry_t *entry = &s_registry.entries[i];
        app_registry_entry_t view = {
            .app_id = entry->app_id,
            .app_dir = entry->app_dir,
            .display_name = entry->display_name,
            .entry = entry->entry,
            .icon = entry->icon,
            .args_json = entry->args_json,
            .order = entry->order,
            .visible = entry->visible,
            .manage_mode = entry->manage_mode,
        };
        esp_err_t err = callback(&view, user_ctx);
        if (err != ESP_OK) {
            xSemaphoreGive(s_registry.lock);
            return err;
        }
    }
    xSemaphoreGive(s_registry.lock);
    return ESP_OK;
}

esp_err_t app_registry_publish(const char *app_id)
{
    app_registry_owned_entry_t entry = {0};
    char *app_dir = NULL;
    esp_err_t err;

    if (!app_registry_id_is_valid(app_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registry.initialized || !s_registry.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_registry.lock, pdMS_TO_TICKS(APP_REGISTRY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_registry.root_count == 0) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_INVALID_STATE;
    }
    app_dir = path_join_dup(s_registry.roots[0], app_id);
    err = app_dir ? parse_manifest(app_id, app_dir, APP_REGISTRY_MANAGE_MODE_RUNTIME, &entry) : ESP_ERR_NO_MEM;
    free_entry(&entry);
    if (err == ESP_OK) {
        err = reload_registry_locked();
    }
    xSemaphoreGive(s_registry.lock);
    free(app_dir);
    if (err == ESP_OK) {
        notify_registry_changed();
    }
    return err;
}

esp_err_t app_registry_remove(const char *app_id)
{
    char *app_dir = NULL;
    esp_err_t err;

    if (!app_registry_id_is_valid(app_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registry.initialized || !s_registry.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_registry.lock, pdMS_TO_TICKS(APP_REGISTRY_LOCK_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_registry.root_count == 0) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_INVALID_STATE;
    }
    app_dir = path_join_dup(s_registry.roots[0], app_id);
    err = app_dir ? remove_directory_recursive(app_dir) : ESP_ERR_NO_MEM;
    if (err == ESP_ERR_NOT_FOUND) {
        for (size_t i = 0; i < s_registry.entry_count; i++) {
            if (strcmp(s_registry.entries[i].app_id, app_id) == 0 && s_registry.entries[i].manage_mode == APP_REGISTRY_MANAGE_MODE_READONLY) {
                err = ESP_ERR_INVALID_STATE;
                break;
            }
        }
    }
    if (err == ESP_OK) {
        err = reload_registry_locked();
    }
    xSemaphoreGive(s_registry.lock);
    free(app_dir);
    if (err == ESP_OK) {
        notify_registry_changed();
    }
    return err;
}
