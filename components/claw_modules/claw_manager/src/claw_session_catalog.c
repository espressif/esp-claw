/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_session_catalog_priv.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CATALOG_MAX_BYTES (256 * 1024)
#define CATALOG_MAX_ITEMS 512

static struct {
    SemaphoreHandle_t lock;
    char path[CLAW_SESSION_MGR_PATH_SIZE];
    cJSON *items;
    uint64_t activity;
    bool dirty;
    void (*changed)(void *);
    void *changed_ctx;
} s_catalog;

static const char *str(const cJSON *item, const char *key)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(item, key));
    return value ? value : "";
}

static esp_err_t save_locked(void)
{
    s_catalog.dirty = true;
    char *tmp = malloc(CLAW_SESSION_MGR_PATH_SIZE + 8);
    char *text = cJSON_PrintUnformatted(s_catalog.items);
    if (!tmp || !text) { free(tmp); free(text); return ESP_ERR_NO_MEM; }
    snprintf(tmp, CLAW_SESSION_MGR_PATH_SIZE + 8, "%s.tmp", s_catalog.path);
    FILE *f = fopen(tmp, "wb");
    esp_err_t err = ESP_FAIL;
    if (f) {
        bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
        if (fclose(f) != 0) ok = false;
        if (ok) err = claw_session_file_replace(tmp, s_catalog.path);
    }
    if (err != ESP_OK) ESP_LOGE("session_catalog", "Cannot save catalog");
    if (err == ESP_OK) { s_catalog.dirty = false; if (s_catalog.changed) s_catalog.changed(s_catalog.changed_ctx); }
    free(tmp); free(text);
    return err;
}

esp_err_t claw_session_catalog_init(const char *root)
{
    if (!s_catalog.lock) s_catalog.lock = xSemaphoreCreateRecursiveMutex();
    if (!s_catalog.lock) return ESP_ERR_NO_MEM;
    xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
    int n = snprintf(s_catalog.path, sizeof(s_catalog.path), "%s/catalog.json", root);
    esp_err_t err = n < 0 || n >= sizeof(s_catalog.path) ? ESP_ERR_INVALID_SIZE : ESP_OK;
    if (err == ESP_OK && !s_catalog.items) {
        FILE *f = fopen(s_catalog.path, "rb");
        if (f) {
            if (fseek(f, 0, SEEK_END) != 0) err = ESP_FAIL;
            long size = ftell(f);
            if (size < 0 || size > CATALOG_MAX_BYTES) err = ESP_ERR_INVALID_SIZE;
            char *text = err == ESP_OK ? calloc(1, (size_t)size + 1) : NULL;
            if (err == ESP_OK && !text) err = ESP_ERR_NO_MEM;
            if (err == ESP_OK && (fseek(f, 0, SEEK_SET) != 0 || fread(text, 1, size, f) != size)) err = ESP_FAIL;
            if (err == ESP_OK) s_catalog.items = cJSON_Parse(text);
            free(text); fclose(f);
            if (err == ESP_OK && !cJSON_IsArray(s_catalog.items)) err = ESP_ERR_INVALID_RESPONSE;
        } else if (errno == ENOENT) {
            s_catalog.items = cJSON_CreateArray();
            if (!s_catalog.items) err = ESP_ERR_NO_MEM;
        } else err = ESP_FAIL;
        if (cJSON_GetArraySize(s_catalog.items) > CATALOG_MAX_ITEMS) err = ESP_ERR_INVALID_SIZE;
        cJSON *item;
        cJSON_ArrayForEach(item, s_catalog.items) {
            cJSON *order = cJSON_GetObjectItem(item, "activity_order");
            if (strlen(str(item, "session")) != 32 || !str(item, "session_id")[0] || !cJSON_IsNumber(order) || order->valuedouble < 0) { err = ESP_ERR_INVALID_RESPONSE; break; }
            uint64_t activity = order->valuedouble;
            if (activity > s_catalog.activity) s_catalog.activity = activity;
        }
    }
    xSemaphoreGiveRecursive(s_catalog.lock);
    return err;
}

static cJSON *find_locked(const char *key, const char *value)
{
    cJSON *item;
    cJSON_ArrayForEach(item, s_catalog.items) {
        if (strcmp(str(item, key), value) == 0) return item;
    }
    return NULL;
}

static void random_id(char *id, size_t size)
{
    snprintf(id, size, "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random(), (unsigned long)esp_random());
}

static esp_err_t visit_map(const claw_session_mgr_alias_map_t *map, void *ctx)
{
    bool *changed = ctx;
    const char *key = map->chat_key;
    uint32_t agent = 0;
    if (strncmp(key, "agent:", 6) == 0) {
        char *end;
        agent = strtoul(key + 6, &end, 10);
        if (*end != ':') return ESP_ERR_INVALID_RESPONSE;
        key = end + 1;
    }
    if (strncmp(key, "chat:", 5) != 0) return ESP_OK;
    const char *split = strchr(key + 5, ':');
    if (!split || split - key - 5 >= 16 || !split[1]) return ESP_ERR_INVALID_RESPONSE;
    claw_session_info_t *info = calloc(1, sizeof(*info));
    if (!info) return ESP_ERR_NO_MEM;
    memcpy(info->channel, key + 5, split - key - 5);
    strlcpy(info->chat_id, split + 1, sizeof(info->chat_id));
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < map->session_count; i++) {
        snprintf(info->session_id, sizeof(info->session_id), "%s:%s", map->chat_key, map->sessions[i]);
        cJSON *item = find_locked("session_id", info->session_id);
        if (!item) {
            if (cJSON_GetArraySize(s_catalog.items) >= CATALOG_MAX_ITEMS) { err = ESP_ERR_NO_MEM; break; }
            item = cJSON_CreateObject();
            random_id(info->id, sizeof(info->id));
            bool ok = item && cJSON_AddStringToObject(item, "session", info->id) &&
                cJSON_AddStringToObject(item, "session_id", info->session_id) &&
                cJSON_AddStringToObject(item, "source", info->channel) &&
                cJSON_AddStringToObject(item, "chat_id", info->chat_id) &&
                cJSON_AddStringToObject(item, "alias", map->sessions[i]) &&
                cJSON_AddStringToObject(item, "title", "") &&
                cJSON_AddNumberToObject(item, "agent_id", agent) &&
                cJSON_AddNumberToObject(item, "activity_order", 0) && cJSON_AddNullToObject(item, "updated_at");
            if (!ok || !cJSON_AddItemToArray(s_catalog.items, item)) { cJSON_Delete(item); err = ESP_ERR_NO_MEM; break; }
            *changed = true;
        }
        cJSON_DeleteItemFromObject(item, "seen");
        if (!cJSON_AddBoolToObject(item, "seen", true)) { err = ESP_ERR_NO_MEM; break; }
    }
    free(info);
    return err;
}

static esp_err_t refresh_locked(void)
{
    bool changed = false;
    cJSON *item;
    cJSON_ArrayForEach(item, s_catalog.items) cJSON_DeleteItemFromObject(item, "seen");
    esp_err_t err = claw_session_mgr_visit(visit_map, &changed);
    if (err != ESP_OK) return err;
    for (int i = cJSON_GetArraySize(s_catalog.items) - 1; i >= 0; i--) {
        item = cJSON_GetArrayItem(s_catalog.items, i);
        if (!cJSON_IsTrue(cJSON_GetObjectItem(item, "seen"))) {
            cJSON_DeleteItemFromArray(s_catalog.items, i);
            changed = true;
        } else cJSON_DeleteItemFromObject(item, "seen");
    }
    return changed || s_catalog.dirty ? save_locked() : ESP_OK;
}

static void copy_info(const cJSON *item, claw_session_info_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy(out->id, str(item, "session"), sizeof(out->id));
    strlcpy(out->session_id, str(item, "session_id"), sizeof(out->session_id));
    strlcpy(out->channel, str(item, "source"), sizeof(out->channel));
    strlcpy(out->chat_id, str(item, "chat_id"), sizeof(out->chat_id));
    strlcpy(out->alias, str(item, "alias"), sizeof(out->alias));
    strlcpy(out->title, str(item, "title"), sizeof(out->title));
    strlcpy(out->delivery_state, str(item, "delivery_state"), sizeof(out->delivery_state));
    out->agent_id = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "agent_id"));
    out->activity = cJSON_GetNumberValue(cJSON_GetObjectItem(item, "activity_order"));
}

static int compare_activity(const void *a, const void *b)
{
    const cJSON *left = *(cJSON *const *)a, *right = *(cJSON *const *)b;
    double l = cJSON_GetNumberValue(cJSON_GetObjectItem(left, "activity_order"));
    double r = cJSON_GetNumberValue(cJSON_GetObjectItem(right, "activity_order"));
    return l != r ? (l > r ? -1 : 1) : strcmp(str(left, "session"), str(right, "session"));
}

esp_err_t claw_session_mgr_catalog_list(const char *channel, size_t offset, size_t limit, cJSON **out)
{
    if (!out || !s_catalog.items || !limit || limit > 50) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
    esp_err_t err = refresh_locked();
    size_t count = cJSON_GetArraySize(s_catalog.items), used = 0;
    cJSON **sorted = calloc(count ? count : 1, sizeof(*sorted));
    cJSON *response = cJSON_CreateObject(), *items = cJSON_CreateArray(), *item;
    if (!sorted || !response || !items) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        cJSON_ArrayForEach(item, s_catalog.items) {
            if (cJSON_IsTrue(cJSON_GetObjectItem(item, "placeholder"))) continue;
            if (!channel || !channel[0] || strcmp(channel, "all") == 0 || strcmp(channel, str(item, "source")) == 0) sorted[used++] = item;
        }
        qsort(sorted, used, sizeof(*sorted), compare_activity);
        for (size_t i = offset; i < used && i - offset < limit; i++) {
            item = cJSON_Duplicate(sorted[i], true);
            if (!item || !cJSON_AddItemToArray(items, item)) { cJSON_Delete(item); err = ESP_ERR_NO_MEM; break; }
            cJSON_DeleteItemFromObject(item, "session_id");
        }
        cJSON_AddBoolToObject(response, "has_more", offset + limit < used);
        cJSON_AddNumberToObject(response, "next_cursor", offset + cJSON_GetArraySize(items));
        cJSON_AddItemToObject(response, "items", items); items = NULL;
    }
    if (err == ESP_OK) { *out = response; response = NULL; }
    cJSON_Delete(response); cJSON_Delete(items); free(sorted);
    xSemaphoreGiveRecursive(s_catalog.lock);
    return err;
}

esp_err_t claw_session_mgr_catalog_get(const char *id, claw_session_info_t *out)
{
    if (!id || !out || !s_catalog.items) return ESP_ERR_INVALID_ARG;
    xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
    cJSON *item = find_locked("session", id);
    esp_err_t err = item ? (s_catalog.dirty ? save_locked() : ESP_OK) : refresh_locked();
    item = find_locked("session", id);
    if (err == ESP_OK && !item) err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) {
        copy_info(item, out);
        size_t length = 0;
        err = claw_session_mgr_resolve_chat_session_id(out->agent_id, out->channel, out->chat_id, out->alias, out->session_id, sizeof(out->session_id), &length);
    }
    xSemaphoreGiveRecursive(s_catalog.lock);
    return err;
}

esp_err_t claw_session_mgr_catalog_create(claw_session_info_t *out)
{
    if (!out || !s_catalog.items) return ESP_ERR_INVALID_STATE;
    char alias[33];
    random_id(alias, sizeof(alias));
    xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
    cJSON *empty;
    cJSON_ArrayForEach(empty, s_catalog.items) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(empty, "placeholder")) && strcmp(str(empty, "source"), "web") == 0 && strcmp(str(empty, "chat_id"), "web-ui") == 0) {
            cJSON_DeleteItemFromObject(empty, "placeholder");
            esp_err_t err = save_locked();
            if (err == ESP_OK) copy_info(empty, out);
            xSemaphoreGiveRecursive(s_catalog.lock);
            return err;
        }
    }
    esp_err_t err = claw_session_mgr_new_chat_session(0, "web", "web-ui", alias, true, NULL, 0);
    if (err == ESP_OK) err = refresh_locked();
    cJSON *item;
    cJSON_ArrayForEach(item, s_catalog.items) {
        if (strcmp(str(item, "alias"), alias) == 0 && strcmp(str(item, "source"), "web") == 0) {
            copy_info(item, out);
            break;
        }
    }
    xSemaphoreGiveRecursive(s_catalog.lock);
    return err;
}

esp_err_t claw_session_mgr_catalog_rename(const char *id, const char *title)
{
    if (!id || !title || !title[0] || strlen(title) >= CLAW_SESSION_TITLE_SIZE || !s_catalog.items) return ESP_ERR_INVALID_ARG;
    xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
    cJSON *item = find_locked("session", id);
    esp_err_t err = item ? ESP_OK : ESP_ERR_NOT_FOUND;
    cJSON *value = cJSON_CreateString(title);
    if (err == ESP_OK && (!value || !cJSON_ReplaceItemInObject(item, "title", value))) { cJSON_Delete(value); value = NULL; err = ESP_ERR_NO_MEM; }
    else if (err != ESP_OK) cJSON_Delete(value);
    if (err == ESP_OK) err = save_locked();
    xSemaphoreGiveRecursive(s_catalog.lock);
    return err;
}

esp_err_t claw_session_mgr_catalog_touch(const char *session_id, const char *text)
{
    if (!session_id || !s_catalog.items) return ESP_ERR_INVALID_STATE;
    xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
    cJSON *item = find_locked("session_id", session_id);
    if (!item && refresh_locked() == ESP_OK) item = find_locked("session_id", session_id);
    esp_err_t err = ESP_OK;
    if (item) {
        cJSON_DeleteItemFromObject(item, "placeholder");
        if (!str(item, "title")[0] && text && text[0]) {
            char title[CLAW_SESSION_TITLE_SIZE];
            size_t n = strnlen(text, sizeof(title) - 1);
            while (n && ((unsigned char)text[n] & 0xc0) == 0x80) n--;
            memcpy(title, text, n); title[n] = 0;
            cJSON_ReplaceItemInObject(item, "title", cJSON_CreateString(title));
        }
        cJSON_SetNumberValue(cJSON_GetObjectItem(item, "activity_order"), ++s_catalog.activity);
        time_t now = time(NULL);
        if (now > 1700000000) cJSON_ReplaceItemInObject(item, "updated_at", cJSON_CreateNumber((double)now * 1000));
        err = save_locked();
    }
    xSemaphoreGiveRecursive(s_catalog.lock);
    return err;
}

esp_err_t claw_session_mgr_catalog_delete(const char *id)
{
    claw_session_info_t *info = calloc(1, sizeof(*info));
    claw_session_mgr_alias_map_t *map = calloc(1, sizeof(*map));
    if (!info || !map) { free(info); free(map); return ESP_ERR_NO_MEM; }
    char replacement[33] = {0};
    esp_err_t err = claw_session_mgr_catalog_get(id, info);
    if (err == ESP_OK) err = claw_session_mgr_list_chat_sessions(info->agent_id, info->channel, info->chat_id, map);
    if (err == ESP_OK && strcmp(map->current_alias, info->alias) == 0) {
        if (map->session_count == 1) {
            random_id(replacement, sizeof(replacement));
            err = claw_session_mgr_new_chat_session(info->agent_id, info->channel, info->chat_id, replacement, true, NULL, 0);
        } else {
            const char *other = map->sessions[strcmp(map->sessions[0], info->alias) == 0 ? 1 : 0];
            err = claw_session_mgr_switch_chat_session(info->agent_id, info->channel, info->chat_id, other, NULL, 0);
        }
    }
    if (err == ESP_OK) err = claw_session_mgr_delete_chat_session(info->agent_id, info->channel, info->chat_id, info->alias, NULL, 0);
    if (err == ESP_OK) {
        xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
        err = refresh_locked();
        if (err == ESP_OK && replacement[0]) {
            cJSON *item;
            cJSON_ArrayForEach(item, s_catalog.items) {
                if (strcmp(str(item, "alias"), replacement) == 0 && strcmp(str(item, "source"), info->channel) == 0 && strcmp(str(item, "chat_id"), info->chat_id) == 0) cJSON_AddBoolToObject(item, "placeholder", true);
            }
            err = save_locked();
        }
        xSemaphoreGiveRecursive(s_catalog.lock);
    }
    free(info); free(map);
    return err;
}

void claw_session_mgr_set_change_callback(void (*callback)(void *), void *ctx)
{
    if (!s_catalog.lock) return;
    xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
    s_catalog.changed = callback;
    s_catalog.changed_ctx = ctx;
    xSemaphoreGiveRecursive(s_catalog.lock);
}

esp_err_t claw_session_mgr_set_delivery(const char *session_id, const char *state)
{
    if (!session_id || !state || !s_catalog.lock) return ESP_ERR_INVALID_ARG;
    xSemaphoreTakeRecursive(s_catalog.lock, portMAX_DELAY);
    cJSON *item = find_locked("session_id", session_id);
    esp_err_t err = ESP_OK;
    if (item) {
        cJSON *value = cJSON_CreateString(state);
        if (!value) err = ESP_ERR_NO_MEM;
        else if (cJSON_HasObjectItem(item, "delivery_state")) cJSON_ReplaceItemInObject(item, "delivery_state", value);
        else cJSON_AddItemToObject(item, "delivery_state", value);
        if (err == ESP_OK) err = save_locked();
    }
    xSemaphoreGiveRecursive(s_catalog.lock);
    return err;
}
