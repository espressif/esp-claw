/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "settings_store.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings_store";

#define SETTINGS_BACKUP_PATH_LEN 192
#define SETTINGS_VALUE_MAX_LEN   2048

static char s_namespace[16];
static bool s_initialized;
static char s_backup_path[SETTINGS_BACKUP_PATH_LEN];
static settings_store_bus_lock_fn   s_bus_lock;
static settings_store_bus_unlock_fn s_bus_unlock;

static bool backup_lock(void)
{
    return s_bus_lock ? s_bus_lock() : false;
}

static void backup_unlock(bool was_locked)
{
    if (s_bus_unlock) {
        s_bus_unlock(was_locked);
    }
}

static esp_err_t settings_store_open(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (!s_initialized || s_namespace[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    return nvs_open(s_namespace, mode, handle);
}

esp_err_t settings_store_init(const settings_store_config_t *config)
{
    if (!config || !config->namespace_name || config->namespace_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlcpy(s_namespace, config->namespace_name, sizeof(s_namespace)) >= sizeof(s_namespace)) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_initialized = true;

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_close(handle);
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = settings_store_open(NVS_READWRITE, &handle);
        if (err == ESP_OK) {
            nvs_close(handle);
        }
    }

    return err;
}

esp_err_t settings_store_get_string(const char *key,
                                    char *buf,
                                    size_t buf_size,
                                    const char *default_value)
{
    if (!key || !buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buf[0] = '\0';

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (default_value) {
            strlcpy(buf, default_value, buf_size);
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = buf_size;
    err = nvs_get_str(handle, key, buf, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (default_value) {
            strlcpy(buf, default_value, buf_size);
        }
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str(%s) failed: %s", key, esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

esp_err_t settings_store_has_key(const char *key, bool *exists)
{
    if (!key || !exists) {
        return ESP_ERR_INVALID_ARG;
    }

    *exists = false;

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t required_size = 0;
    err = nvs_get_str(handle, key, NULL, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_str(%s) failed: %s", key, esp_err_to_name(err));
        return err;
    }

    *exists = true;
    return ESP_OK;
}

esp_err_t settings_store_set_string(const char *key, const char *value)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, key, value ? value : "");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }

    nvs_close(handle);

    /* Best-effort: refresh the SD-card mirror so a subsequent firmware
     * re-flash + erase can restore this value. Failures are logged but
     * do not surface — the NVS write already succeeded. */
    if (err == ESP_OK && s_backup_path[0] != '\0') {
        esp_err_t derr = settings_store_dump_to_backup();
        if (derr != ESP_OK) {
            ESP_LOGW(TAG, "backup dump failed: %s", esp_err_to_name(derr));
        }
    }

    return err;
}

esp_err_t settings_store_erase_key(const char *key)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_erase_key(%s) failed: %s", key, esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
    return err;
}

esp_err_t settings_store_commit(void)
{
    return ESP_OK;
}

/* ---------- SD-card backup layer ----------------------------------- */

esp_err_t settings_store_is_namespace_empty(bool *out_empty)
{
    if (!out_empty) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    *out_empty = true;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, s_namespace,
                                   NVS_TYPE_ANY, &it);
    if (err == ESP_OK && it != NULL) {
        *out_empty = false;
        nvs_release_iterator(it);
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err;
}

static esp_err_t backup_mkparent(const char *path)
{
    /* Create the directory part of @p path if missing. Single level only —
     * good enough for paths like /sdcard/config/app_config.json. */
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        return ESP_OK;
    }
    char dir[SETTINGS_BACKUP_PATH_LEN];
    size_t len = (size_t)(slash - path);
    if (len >= sizeof(dir)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';
    if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir(%s) failed: %s", dir, strerror(errno));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t settings_store_set_backup_path(const char *file_path,
                                         settings_store_bus_lock_fn lock,
                                         settings_store_bus_unlock_fn unlock)
{
    if (!file_path || file_path[0] == '\0') {
        s_backup_path[0] = '\0';
        s_bus_lock = NULL;
        s_bus_unlock = NULL;
        return ESP_OK;
    }
    if (strlcpy(s_backup_path, file_path, sizeof(s_backup_path))
            >= sizeof(s_backup_path)) {
        s_backup_path[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    s_bus_lock = lock;
    s_bus_unlock = unlock;
    bool locked = backup_lock();
    esp_err_t err = backup_mkparent(s_backup_path);
    backup_unlock(locked);
    return err;
}

esp_err_t settings_store_dump_to_backup(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_backup_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Empty namespace — write an empty object so a missing file is
         * distinguishable from "intentionally empty". */
    } else if (err != ESP_OK) {
        return err;
    } else {
        nvs_close(handle);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    nvs_iterator_t it = NULL;
    err = nvs_entry_find(NVS_DEFAULT_PART_NAME, s_namespace,
                         NVS_TYPE_STR, &it);
    char *value_buf = NULL;
    while (err == ESP_OK && it != NULL) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        nvs_handle_t rh;
        if (settings_store_open(NVS_READONLY, &rh) == ESP_OK) {
            size_t needed = 0;
            if (nvs_get_str(rh, info.key, NULL, &needed) == ESP_OK
                    && needed > 0 && needed <= SETTINGS_VALUE_MAX_LEN) {
                char *vbuf = malloc(needed);
                if (vbuf) {
                    if (nvs_get_str(rh, info.key, vbuf, &needed) == ESP_OK) {
                        cJSON_AddStringToObject(root, info.key, vbuf);
                    }
                    free(vbuf);
                }
            }
            nvs_close(rh);
        }
        err = nvs_entry_next(&it);
    }
    if (it) {
        nvs_release_iterator(it);
    }
    (void)value_buf;

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t io_err = ESP_OK;
    bool locked = backup_lock();
    /* Write atomically via tmp + rename so a crash mid-write can't leave
     * a half-truncated config that would then overwrite NVS on next boot. */
    char tmp[SETTINGS_BACKUP_PATH_LEN + 4];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_backup_path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) {
        ESP_LOGW(TAG, "open backup tmp(%s) failed: %s", tmp, strerror(errno));
        io_err = ESP_FAIL;
    } else {
        size_t json_len = strlen(json_str);
        if (fwrite(json_str, 1, json_len, fp) != json_len) {
            ESP_LOGW(TAG, "short write to %s", tmp);
            io_err = ESP_FAIL;
        }
        fclose(fp);
        if (io_err == ESP_OK) {
            remove(s_backup_path);  /* rename() refuses to overwrite on FATFS */
            if (rename(tmp, s_backup_path) != 0) {
                ESP_LOGW(TAG, "rename(%s) failed: %s", s_backup_path,
                         strerror(errno));
                io_err = ESP_FAIL;
            }
        } else {
            remove(tmp);
        }
    }
    backup_unlock(locked);

    free(json_str);
    return io_err;
}

esp_err_t settings_store_restore_from_backup(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_backup_path[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    bool locked = backup_lock();
    FILE *fp = fopen(s_backup_path, "rb");
    if (!fp) {
        backup_unlock(locked);
        return (errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0 || fsize > 64 * 1024) {
        fclose(fp);
        backup_unlock(locked);
        return ESP_ERR_INVALID_SIZE;
    }
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(fp);
        backup_unlock(locked);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, (size_t)fsize, fp);
    fclose(fp);
    backup_unlock(locked);
    buf[got] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "backup parse failed");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = settings_store_open(NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return err;
    }
    int restored = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (!item->string || !cJSON_IsString(item)) {
            continue;
        }
        const char *v = cJSON_GetStringValue(item);
        if (!v) {
            v = "";
        }
        esp_err_t serr = nvs_set_str(handle, item->string, v);
        if (serr == ESP_OK) {
            restored++;
        } else {
            ESP_LOGW(TAG, "restore set(%s) failed: %s", item->string,
                     esp_err_to_name(serr));
        }
    }
    cJSON_Delete(root);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "restored %d keys from %s", restored, s_backup_path);
    return err;
}
