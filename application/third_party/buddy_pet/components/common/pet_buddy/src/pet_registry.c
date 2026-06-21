/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "pet_registry.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define PET_REGISTRY_DEFAULT_FATFS_BASE "/fatfs"
#define PET_REGISTRY_JSON_NAME          "pet.json"
#define PET_REGISTRY_SIDECAR_JSON_SUFFIX ".pet.json"
#define PET_REGISTRY_MODULE_DIR         "pet_module"
#define PET_REGISTRY_NVS_NAMESPACE      "pet"
#define PET_REGISTRY_NVS_KEY_SELECTED   "selected"
#define PET_REGISTRY_MMAP_HEADER_LEN    32U
#define PET_REGISTRY_MMAP_MAGIC         "MMAP"
#define PET_REGISTRY_ASSET_MAGIC_LEN    2U

/* Persistent registry state. All fields are protected by lock after initialization. */
typedef struct {
    SemaphoreHandle_t lock;
    char fatfs_base_path[PET_REGISTRY_BASE_PATH_LEN];
    pet_registry_entry_t entries[PET_REGISTRY_MAX];
    size_t entry_count;
    char selected_id[PET_REGISTRY_ID_LEN];
    bool loaded_selected;
} pet_registry_state_t;

/* Refresh scratch storage kept off the task stack. */
typedef struct {
    char path[PET_REGISTRY_ASSET_PATH_LEN];
    pet_registry_entry_t entry;
    size_t skipped;
    bool truncated;
} pet_registry_scan_ctx_t;

static const char *const TAG = "pet_registry";
static pet_registry_state_t s_registry = {
    .fatfs_base_path = PET_REGISTRY_DEFAULT_FATFS_BASE,
};

esp_err_t pet_registry_clear_selected(void);

static esp_err_t pet_registry_ensure_lock(void)
{
    if (s_registry.lock == NULL) {
        s_registry.lock = xSemaphoreCreateMutex();
    }
    return s_registry.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void pet_registry_copy_str(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static esp_err_t pet_registry_load_selected_locked(void)
{
    nvs_handle_t nvs;
    size_t len = sizeof(s_registry.selected_id);
    esp_err_t ret;

    if (s_registry.loaded_selected) {
        return ESP_OK;
    }
    s_registry.loaded_selected = true;
    s_registry.selected_id[0] = '\0';

    ret = nvs_open(PET_REGISTRY_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "open selected pet nvs failed: %s", esp_err_to_name(ret));
        }
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
    }
    ret = nvs_get_str(nvs, PET_REGISTRY_NVS_KEY_SELECTED, s_registry.selected_id, &len);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        s_registry.selected_id[0] = '\0';
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "load selected pet failed: %s", esp_err_to_name(ret));
        s_registry.selected_id[0] = '\0';
    } else if (s_registry.selected_id[0] != '\0') {
        ESP_LOGI(TAG, "selected pet restored: %s", s_registry.selected_id);
    }
    return ret;
}

static esp_err_t pet_registry_save_selected_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PET_REGISTRY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_registry.selected_id[0] == '\0') {
        ret = nvs_erase_key(nvs, PET_REGISTRY_NVS_KEY_SELECTED);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    } else {
        ret = nvs_set_str(nvs, PET_REGISTRY_NVS_KEY_SELECTED, s_registry.selected_id);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static bool pet_registry_find_locked(const char *pet_id, size_t *out_index)
{
    if (pet_id == NULL || pet_id[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < s_registry.entry_count; i++) {
        if (strcmp(s_registry.entries[i].id, pet_id) == 0) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return true;
        }
    }
    return false;
}

static esp_err_t pet_registry_ensure_entries_locked(void)
{
    if (s_registry.entry_count != 0) {
        return ESP_OK;
    }

    xSemaphoreGive(s_registry.lock);
    esp_err_t ret = pet_registry_refresh();
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    return ret;
}

static esp_err_t pet_registry_find_selected_entry_locked(size_t *out_index)
{
    esp_err_t ret;

    ret = pet_registry_load_selected_locked();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = pet_registry_ensure_entries_locked();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND) {
        return ret;
    }
    return pet_registry_find_locked(s_registry.selected_id, out_index) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool pet_registry_file_exists(const char *path)
{
    FILE *fp;

    if (path == NULL || path[0] == '\0') {
        return false;
    }

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }
    fclose(fp);
    return true;
}

static const char *pet_registry_json_string(cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItem(root, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const char *pet_registry_json_profile(cJSON *root)
{
    cJSON *profile;
    const char *value;

    if (root == NULL) {
        return NULL;
    }

    value = pet_registry_json_string(root, "profile");
    if (value != NULL) {
        return value;
    }

    profile = cJSON_GetObjectItem(root, "profile");
    if (cJSON_IsObject(profile)) {
        value = pet_registry_json_string(profile, "team");
        if (value != NULL) {
            return value;
        }
        value = pet_registry_json_string(profile, "subject");
        if (value != NULL) {
            return value;
        }
        value = pet_registry_json_string(profile, "country");
        if (value != NULL) {
            return value;
        }
    }

    value = pet_registry_json_string(root, "team");
    if (value != NULL) {
        return value;
    }
    return pet_registry_json_string(root, "country");
}

static bool pet_registry_name_has_suffix(const char *name, const char *suffix)
{
    size_t name_len;
    size_t suffix_len;

    if (name == NULL || suffix == NULL) {
        return false;
    }
    name_len = strlen(name);
    suffix_len = strlen(suffix);
    return name_len >= suffix_len && strcmp(name + name_len - suffix_len, suffix) == 0;
}

static const char *pet_registry_basename(const char *path)
{
    const char *slash;

    if (path == NULL) {
        return "";
    }
    slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static void pet_registry_copy_dirname(char *dst, size_t dst_size, const char *path)
{
    const char *slash;
    size_t len;

    if (dst == NULL || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (path == NULL) {
        return;
    }

    slash = strrchr(path, '/');
    if (slash == NULL) {
        return;
    }
    len = (size_t)(slash - path);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, path, len);
    dst[len] = '\0';
}

static void pet_registry_copy_sidecar_path(char *dst, size_t dst_size, const char *asset_path)
{
    const char *dot;
    size_t len;
    size_t suffix_len = strlen(PET_REGISTRY_SIDECAR_JSON_SUFFIX);

    if (dst == NULL || dst_size == 0) {
        return;
    }
    dst[0] = '\0';
    if (asset_path == NULL || dst_size <= suffix_len) {
        return;
    }

    dot = strrchr(asset_path, '.');
    len = dot != NULL ? (size_t)(dot - asset_path) : strlen(asset_path);
    if (len + suffix_len >= dst_size) {
        len = dst_size - suffix_len - 1U;
    }
    memcpy(dst, asset_path, len);
    strcpy(dst + len, PET_REGISTRY_SIDECAR_JSON_SUFFIX);
}

static esp_err_t pet_registry_read_text_file(const char *path, char **out_text, size_t *out_len)
{
    FILE *fp;
    char *text;
    long file_size;

    if (path == NULL || out_text == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_text = NULL;
    *out_len = 0;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    file_size = ftell(fp);
    rewind(fp);
    if (file_size < 0 || file_size > 4096) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    text = calloc(1, (size_t)file_size + 1U);
    if (text == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    if (file_size > 0 && fread(text, 1, (size_t)file_size, fp) != (size_t)file_size) {
        free(text);
        fclose(fp);
        return ESP_FAIL;
    }

    fclose(fp);
    *out_text = text;
    *out_len = (size_t)file_size;
    return ESP_OK;
}

static uint32_t pet_registry_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t pet_registry_read_embedded_json(const char *bin_path, char **out_json, size_t *out_len)
{
    FILE *fp;
    uint8_t header[PET_REGISTRY_MMAP_HEADER_LEN];
    uint8_t *table = NULL;
    char *json = NULL;
    uint32_t name_len;
    uint32_t files;
    uint32_t payload_len;
    uint32_t stride;
    uint32_t table_len;
    long file_size;
    esp_err_t ret = ESP_ERR_NOT_FOUND;

    if (bin_path == NULL || out_json == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    *out_len = 0;

    fp = fopen(bin_path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    file_size = ftell(fp);
    rewind(fp);
    if (file_size <= (long)PET_REGISTRY_MMAP_HEADER_LEN ||
            fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(header, PET_REGISTRY_MMAP_MAGIC, 4) != 0) {
        fclose(fp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    name_len = pet_registry_read_le32(header + 8);
    files = pet_registry_read_le32(header + 12);
    payload_len = pet_registry_read_le32(header + 20);
    if (name_len == 0 || files == 0 || files > 128 || name_len > 128) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    stride = name_len + 12U;
    table_len = stride * files;
    if (table_len > payload_len ||
            PET_REGISTRY_MMAP_HEADER_LEN + table_len > (uint32_t)file_size) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    table = malloc(table_len);
    if (table == NULL) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    if (fread(table, 1, table_len, fp) != table_len) {
        free(table);
        fclose(fp);
        return ESP_FAIL;
    }

    for (uint32_t i = 0; i < files; i++) {
        const uint8_t *entry = table + i * stride;
        const char *name = (const char *)entry;
        uint32_t size = pet_registry_read_le32(entry + name_len);
        uint32_t offset = pet_registry_read_le32(entry + name_len + 4U);
        uint32_t data_offset = PET_REGISTRY_MMAP_HEADER_LEN + table_len + offset + PET_REGISTRY_ASSET_MAGIC_LEN;

        if (memchr(name, '\0', name_len) == NULL || strcmp(name, PET_REGISTRY_JSON_NAME) != 0) {
            continue;
        }
        if (size == 0 || data_offset + size > (uint32_t)file_size) {
            ret = ESP_ERR_INVALID_SIZE;
            break;
        }
        json = calloc(1, (size_t)size + 1U);
        if (json == NULL) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        if (fseek(fp, (long)data_offset, SEEK_SET) != 0 ||
                fread(json, 1, size, fp) != size) {
            free(json);
            json = NULL;
            ret = ESP_FAIL;
            break;
        }
        *out_json = json;
        *out_len = size;
        ret = ESP_OK;
        break;
    }

    free(table);
    fclose(fp);
    return ret;
}

static esp_err_t pet_registry_fill_entry_from_json(cJSON *root,
                                                   const char *asset_path,
                                                   pet_registry_entry_t *entry)
{
    cJSON *jid;
    cJSON *jname;

    if (root == NULL || asset_path == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    jid = cJSON_GetObjectItem(root, "id");
    jname = cJSON_GetObjectItem(root, "name");
    if (!cJSON_IsString(jid) || jid->valuestring == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(entry, 0, sizeof(*entry));
    pet_registry_copy_str(entry->id, sizeof(entry->id), jid->valuestring);
    pet_registry_copy_str(entry->name, sizeof(entry->name),
                       cJSON_IsString(jname) ? jname->valuestring : jid->valuestring);
    pet_registry_copy_str(entry->title, sizeof(entry->title), pet_registry_json_string(root, "title"));
    pet_registry_copy_str(entry->profile, sizeof(entry->profile), pet_registry_json_profile(root));
    pet_registry_copy_dirname(entry->base_path, sizeof(entry->base_path), asset_path);
    pet_registry_copy_str(entry->asset_path, sizeof(entry->asset_path), asset_path);
    if (!pet_registry_file_exists(entry->asset_path)) {
        ESP_LOGW(TAG, "pet bin not found: %s", entry->asset_path);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t pet_registry_parse_embedded_pet(const char *asset_path,
                                                 pet_registry_entry_t *entry)
{
    char *json = NULL;
    size_t json_len = 0;
    cJSON *root = NULL;
    esp_err_t ret;

    ret = pet_registry_read_embedded_json(asset_path, &json, &json_len);
    if (ret != ESP_OK) {
        return ret;
    }
    root = cJSON_ParseWithLength(json, json_len);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    ret = pet_registry_fill_entry_from_json(root, asset_path, entry);
    cJSON_Delete(root);
    return ret;
}

static esp_err_t pet_registry_parse_sidecar_pet(const char *asset_path,
                                                pet_registry_entry_t *entry)
{
    char sidecar_path[PET_REGISTRY_ASSET_PATH_LEN];
    char *json = NULL;
    size_t json_len = 0;
    cJSON *root = NULL;
    esp_err_t ret;

    if (asset_path == NULL || entry == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pet_registry_copy_sidecar_path(sidecar_path, sizeof(sidecar_path), asset_path);
    if (sidecar_path[0] == '\0') {
        return ESP_ERR_INVALID_SIZE;
    }

    ret = pet_registry_read_text_file(sidecar_path, &json, &json_len);
    if (ret != ESP_OK) {
        return ret;
    }
    root = cJSON_ParseWithLength(json, json_len);
    free(json);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    ret = pet_registry_fill_entry_from_json(root, asset_path, entry);
    cJSON_Delete(root);
    return ret;
}

esp_err_t pet_registry_set_fatfs_base_path(const char *base_path)
{
    ESP_RETURN_ON_FALSE(base_path != NULL && base_path[0] == '/', ESP_ERR_INVALID_ARG,
                        TAG, "invalid fatfs base path");
    ESP_RETURN_ON_FALSE(strlen(base_path) < sizeof(s_registry.fatfs_base_path), ESP_ERR_INVALID_SIZE,
                        TAG, "fatfs base path too long");
    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");

    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    pet_registry_copy_str(s_registry.fatfs_base_path, sizeof(s_registry.fatfs_base_path), base_path);
    s_registry.entry_count = 0;
    xSemaphoreGive(s_registry.lock);
    ESP_LOGI(TAG, "fatfs base path set: %s", base_path);
    return ESP_OK;
}

esp_err_t pet_registry_refresh(void)
{
    pet_registry_scan_ctx_t *scan = NULL;
    DIR *dir;
    struct dirent *ent;

    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");

    scan = calloc(1, sizeof(*scan));
    if (scan == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    (void)pet_registry_load_selected_locked();
    s_registry.entry_count = 0;
    if (snprintf(scan->path,
                 sizeof(scan->path),
                 "%s/skills/%s",
                 s_registry.fatfs_base_path,
                 PET_REGISTRY_MODULE_DIR) >= (int)sizeof(scan->path)) {
        xSemaphoreGive(s_registry.lock);
        ESP_LOGW(TAG, "pet module path too long: base=%s", s_registry.fatfs_base_path);
        free(scan);
        return ESP_ERR_INVALID_SIZE;
    }

    dir = opendir(scan->path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "pet module dir not found: %s", scan->path);
        xSemaphoreGive(s_registry.lock);
        free(scan);
        return ESP_ERR_NOT_FOUND;
    }

    size_t module_root_len = strlen(scan->path);
    while ((ent = readdir(dir)) != NULL) {
        esp_err_t parse_ret;

        if (ent->d_name[0] == '.' || !pet_registry_name_has_suffix(ent->d_name, ".bin")) {
            continue;
        }
        if (s_registry.entry_count >= PET_REGISTRY_MAX) {
            scan->truncated = true;
            scan->skipped++;
            continue;
        }
        if (snprintf(scan->path + module_root_len,
                     sizeof(scan->path) - module_root_len,
                     "/%s",
                     ent->d_name) >= (int)(sizeof(scan->path) - module_root_len)) {
            scan->skipped++;
            ESP_LOGW(TAG, "skip pet bin with long path: %s", ent->d_name);
            continue;
        }
        parse_ret = pet_registry_parse_embedded_pet(scan->path, &scan->entry);
        if (parse_ret != ESP_OK) {
            parse_ret = pet_registry_parse_sidecar_pet(scan->path, &scan->entry);
        }
        if (parse_ret == ESP_OK) {
            s_registry.entries[s_registry.entry_count++] = scan->entry;
            ESP_LOGD(TAG, "pet found: id=%s name=%s bin=%s", scan->entry.id, scan->entry.name,
                     pet_registry_basename(scan->entry.asset_path));
        } else {
            scan->skipped++;
            ESP_LOGW(TAG, "skip pet bin: file=%s err=%s",
                     pet_registry_basename(scan->path), esp_err_to_name(parse_ret));
        }
    }
    closedir(dir);
    scan->path[module_root_len] = '\0';
    ESP_LOGI(TAG, "pet registry refreshed: root=%s count=%u skipped=%u%s",
             scan->path,
             (unsigned)s_registry.entry_count,
             (unsigned)scan->skipped,
             scan->truncated ? " truncated" : "");

    xSemaphoreGive(s_registry.lock);
    free(scan);
    return ESP_OK;
}

size_t pet_registry_count(void)
{
    size_t count = 0;

    if (pet_registry_ensure_lock() != ESP_OK) {
        return 0;
    }
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    count = s_registry.entry_count;
    xSemaphoreGive(s_registry.lock);
    return count;
}

esp_err_t pet_registry_get(size_t index, pet_registry_entry_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");

    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    if (index >= s_registry.entry_count) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_NOT_FOUND;
    }
    *out = s_registry.entries[index];
    xSemaphoreGive(s_registry.lock);
    return ESP_OK;
}

esp_err_t pet_registry_select(const char *pet_id)
{
    esp_err_t ret = ESP_OK;

    if (pet_id == NULL || pet_id[0] == '\0') {
        return pet_registry_clear_selected();
    }
    if (strlen(pet_id) >= PET_REGISTRY_ID_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    if (!pet_registry_find_locked(pet_id, NULL)) {
        xSemaphoreGive(s_registry.lock);
        ret = pet_registry_refresh();
        if (ret != ESP_OK) {
            return ret;
        }
        xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    }
    if (!pet_registry_find_locked(pet_id, NULL)) {
        xSemaphoreGive(s_registry.lock);
        return ESP_ERR_NOT_FOUND;
    }
    pet_registry_copy_str(s_registry.selected_id, sizeof(s_registry.selected_id), pet_id);
    s_registry.loaded_selected = true;
    ret = pet_registry_save_selected_locked();
    xSemaphoreGive(s_registry.lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "selected pet changed: %s", pet_id);
    } else {
        ESP_LOGW(TAG, "save selected pet failed: id=%s err=%s", pet_id, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pet_registry_clear_selected(void)
{
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    s_registry.selected_id[0] = '\0';
    s_registry.loaded_selected = true;
    ret = pet_registry_save_selected_locked();
    xSemaphoreGive(s_registry.lock);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "selected pet cleared");
    } else {
        ESP_LOGW(TAG, "clear selected pet failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t pet_registry_get_selected(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    (void)pet_registry_load_selected_locked();
    pet_registry_copy_str(out, out_size, s_registry.selected_id);
    xSemaphoreGive(s_registry.lock);
    return out[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t pet_registry_get_selected_entry(pet_registry_entry_t *out)
{
    size_t index = 0;
    esp_err_t ret;

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    ret = pet_registry_find_selected_entry_locked(&index);
    if (ret != ESP_OK) {
        memset(out, 0, sizeof(*out));
        xSemaphoreGive(s_registry.lock);
        return ret;
    }
    *out = s_registry.entries[index];
    xSemaphoreGive(s_registry.lock);
    return ESP_OK;
}

esp_err_t pet_registry_get_selected_asset_path(char *out, size_t out_size)
{
    size_t index = 0;
    esp_err_t ret;

    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    ret = pet_registry_find_selected_entry_locked(&index);
    if (ret != ESP_OK) {
        out[0] = '\0';
        xSemaphoreGive(s_registry.lock);
        return ret;
    }
    pet_registry_copy_str(out, out_size, s_registry.entries[index].asset_path);
    xSemaphoreGive(s_registry.lock);
    return ESP_OK;
}

esp_err_t pet_registry_get_default_asset_path(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(pet_registry_ensure_lock(), TAG, "create lock failed");
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    esp_err_t ret = pet_registry_ensure_entries_locked();
    if (ret != ESP_OK || s_registry.entry_count == 0) {
        out[0] = '\0';
        xSemaphoreGive(s_registry.lock);
        return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
    }
    pet_registry_copy_str(out, out_size, s_registry.entries[0].asset_path);
    xSemaphoreGive(s_registry.lock);
    return ESP_OK;
}

bool pet_registry_has_selected(void)
{
    bool has_selected;

    if (pet_registry_ensure_lock() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(s_registry.lock, portMAX_DELAY);
    (void)pet_registry_load_selected_locked();
    has_selected = s_registry.selected_id[0] != '\0';
    xSemaphoreGive(s_registry.lock);
    return has_selected;
}
