/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"

static const char *TAG = "claw_memory";

static cJSON *claw_memory_new_index_root(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *summaries = cJSON_CreateArray();
    cJSON *keyword_index = cJSON_CreateObject();

    if (!root || !summaries || !keyword_index) {
        cJSON_Delete(root);
        cJSON_Delete(summaries);
        cJSON_Delete(keyword_index);
        return NULL;
    }
    cJSON_AddNumberToObject(root, "version", 3);
    cJSON_AddNumberToObject(root, "next_summary_id", 1);
    cJSON_AddNumberToObject(root, "last_compact_digest_size", 0);
    cJSON_AddItemToObject(root, "summaries", summaries);
    cJSON_AddItemToObject(root, "keyword_index", keyword_index);
    return root;
}

esp_err_t claw_memory_load_index(cJSON **out_root)
{
    char *raw = NULL;
    cJSON *root = NULL;
    esp_err_t err;

    if (!out_root) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_root = NULL;

    err = read_file_dup(s_memory.index_path, &raw);
    if (err == ESP_ERR_NOT_FOUND) {
        root = claw_memory_new_index_root();
        if (!root) {
            return ESP_ERR_NO_MEM;
        }
        *out_root = root;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_Parse(raw);
    free(raw);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "Failed to parse memory index: %s", s_memory.index_path);
        return ESP_FAIL;
    }

    /* cJSON_ReplaceItemInObject only takes ownership of the new node when the
     * key already exists; on a malformed/legacy index that is MISSING the key
     * entirely it returns false and leaks the freshly-created node. Delete +
     * add handles both the absent and wrong-type cases without leaking. */
    if (!cJSON_IsArray(cJSON_GetObjectItem(root, "summaries"))) {
        cJSON_DeleteItemFromObject(root, "summaries");
        cJSON_AddItemToObject(root, "summaries", cJSON_CreateArray());
    }
    if (!cJSON_IsNumber(cJSON_GetObjectItem(root, "next_summary_id"))) {
        cJSON_AddNumberToObject(root, "next_summary_id", 1);
    }
    if (!cJSON_IsNumber(cJSON_GetObjectItem(root, "last_compact_digest_size"))) {
        cJSON_AddNumberToObject(root, "last_compact_digest_size", 0);
    }
    if (!cJSON_IsObject(cJSON_GetObjectItem(root, "keyword_index"))) {
        cJSON_DeleteItemFromObject(root, "keyword_index");
        cJSON_AddItemToObject(root, "keyword_index", cJSON_CreateObject());
    }
    {
        cJSON *summaries = cJSON_GetObjectItem(root, "summaries");
        int i;

        for (i = cJSON_GetArraySize(summaries) - 1; i >= 0; i--) {
            cJSON *summary = cJSON_GetArrayItem(summaries, i);
            cJSON *clean = NULL;
            const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(summary, "label"));
            int summary_id = cJSON_GetObjectItem(summary, "summary_id") ?
                cJSON_GetObjectItem(summary, "summary_id")->valueint : 0;
            int ref_count = cJSON_GetObjectItem(summary, "ref_count") ?
                cJSON_GetObjectItem(summary, "ref_count")->valueint : 0;

            if (!label || !label[0]) {
                cJSON_DeleteItemFromArray(summaries, i);
                continue;
            }

            clean = cJSON_CreateObject();
            if (!clean) {
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddNumberToObject(clean, "summary_id", summary_id > 0 ? summary_id : 0);
            cJSON_AddStringToObject(clean, "label", label);
            cJSON_AddNumberToObject(clean, "ref_count", ref_count > 0 ? ref_count : 0);
            cJSON_ReplaceItemInArray(summaries, i, clean);
        }
    }

    *out_root = root;
    return ESP_OK;
}

esp_err_t claw_memory_save_index(cJSON *root)
{
    char *text;
    esp_err_t err;

    if (!root) {
        return ESP_ERR_INVALID_ARG;
    }
    text = cJSON_PrintUnformatted(root);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    err = write_file_text(s_memory.index_path, text);
    free(text);
    return err;
}

cJSON *claw_memory_find_summary_by_label(cJSON *index_root, const char *label)
{
    cJSON *summaries;
    cJSON *item;

    if (!index_root || !label || !label[0]) {
        return NULL;
    }
    summaries = cJSON_GetObjectItem(index_root, "summaries");
    cJSON_ArrayForEach(item, summaries) {
        const char *existing = cJSON_GetStringValue(cJSON_GetObjectItem(item, "label"));
        if (existing && strcmp(existing, label) == 0) {
            return item;
        }
    }
    return NULL;
}

cJSON *claw_memory_find_summary_by_id(cJSON *index_root, int summary_id)
{
    cJSON *summaries;
    cJSON *item;

    if (!index_root || summary_id <= 0) {
        return NULL;
    }
    summaries = cJSON_GetObjectItem(index_root, "summaries");
    cJSON_ArrayForEach(item, summaries) {
        cJSON *id = cJSON_GetObjectItem(item, "summary_id");
        if (cJSON_IsNumber(id) && id->valueint == summary_id) {
            return item;
        }
    }
    return NULL;
}

static int claw_memory_next_summary_id(cJSON *index_root)
{
    cJSON *item = cJSON_GetObjectItem(index_root, "next_summary_id");

    if (!cJSON_IsNumber(item) || item->valueint <= 0) {
        cJSON_DeleteItemFromObject(index_root, "next_summary_id");
        cJSON_AddNumberToObject(index_root, "next_summary_id", 1);
        item = cJSON_GetObjectItem(index_root, "next_summary_id");
    }
    return item->valueint;
}

static void claw_memory_set_next_summary_id(cJSON *index_root, int next_id)
{
    cJSON_DeleteItemFromObject(index_root, "next_summary_id");
    cJSON_AddNumberToObject(index_root, "next_summary_id", next_id);
}

esp_err_t claw_memory_ensure_summary_label(cJSON *index_root,
                                           const char *label,
                                           int preferred_id,
                                           int *out_summary_id)
{
    cJSON *summary;
    cJSON *summaries;
    int next_id;

    if (!index_root || !label || !label[0] || !out_summary_id) {
        return ESP_ERR_INVALID_ARG;
    }

    summary = claw_memory_find_summary_by_label(index_root, label);
    if (summary) {
        *out_summary_id = cJSON_GetObjectItem(summary, "summary_id")->valueint;
        return ESP_OK;
    }

    summaries = cJSON_GetObjectItem(index_root, "summaries");
    if (!cJSON_IsArray(summaries)) {
        return ESP_FAIL;
    }

    next_id = claw_memory_next_summary_id(index_root);
    if (preferred_id > 0 && !claw_memory_find_summary_by_id(index_root, preferred_id)) {
        next_id = preferred_id;
    }

    summary = cJSON_CreateObject();
    if (!summary) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(summary, "summary_id", next_id);
    cJSON_AddStringToObject(summary, "label", label);
    cJSON_AddNumberToObject(summary, "ref_count", 0);
    cJSON_AddItemToArray(summaries, summary);
    if (next_id >= claw_memory_next_summary_id(index_root)) {
        claw_memory_set_next_summary_id(index_root, next_id + 1);
    }
    *out_summary_id = next_id;
    return ESP_OK;
}

void claw_memory_adjust_summary_stats(cJSON *index_root,
                                      const claw_memory_item_t *item,
                                      int ref_delta)
{
    size_t i;

    if (!index_root || !item) {
        return;
    }

    for (i = 0; i < item->summary_id_count && i < CLAW_MEMORY_MAX_SUMMARIES; i++) {
        cJSON *summary = claw_memory_find_summary_by_id(index_root, item->summary_ids[i]);
        cJSON *ref_count;

        if (!summary) {
            continue;
        }
        ref_count = cJSON_GetObjectItem(summary, "ref_count");
        if (!cJSON_IsNumber(ref_count)) {
            cJSON_DeleteItemFromObject(summary, "ref_count");
            cJSON_AddNumberToObject(summary, "ref_count", 0);
            ref_count = cJSON_GetObjectItem(summary, "ref_count");
        }

        ref_count->valueint += ref_delta;
        if (ref_count->valueint < 0) {
            ref_count->valueint = 0;
        }
        ref_count->valuedouble = (double)ref_count->valueint;
    }
}

void claw_memory_remove_unused_summaries(cJSON *index_root)
{
    cJSON *summaries;
    int i;

    if (!index_root) {
        return;
    }

    summaries = cJSON_GetObjectItem(index_root, "summaries");
    if (!cJSON_IsArray(summaries)) {
        return;
    }

    for (i = cJSON_GetArraySize(summaries) - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(summaries, i);
        cJSON *ref_count = cJSON_GetObjectItem(item, "ref_count");
        if (!cJSON_IsNumber(ref_count) || ref_count->valueint <= 0) {
            cJSON_DeleteItemFromArray(summaries, i);
        }
    }
}

void claw_memory_item_list_free(claw_memory_item_list_t *list)
{
    if (!list) {
        return;
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

esp_err_t claw_memory_item_list_push(claw_memory_item_list_t *list,
                                     const claw_memory_item_t *item)
{
    claw_memory_item_t *new_items;
    size_t new_capacity;

    if (!list || !item) {
        return ESP_ERR_INVALID_ARG;
    }
    if (list->count < list->capacity) {
        list->items[list->count++] = *item;
        return ESP_OK;
    }

    new_capacity = list->capacity == 0 ? 8 : list->capacity * 2;
    new_items = realloc(list->items, new_capacity * sizeof(*new_items));
    if (!new_items) {
        return ESP_ERR_NO_MEM;
    }
    list->items = new_items;
    list->capacity = new_capacity;
    list->items[list->count++] = *item;
    return ESP_OK;
}

int claw_memory_find_item_index(const claw_memory_item_list_t *list, const char *memory_id)
{
    size_t i;

    if (!list || !memory_id || !memory_id[0]) {
        return -1;
    }
    for (i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].id, memory_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void claw_memory_read_item_field_string(cJSON *json,
                                               const char *key,
                                               char *dst,
                                               size_t dst_size)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(json, key));

    if (value) {
        safe_copy(dst, dst_size, value);
    }
}

static void claw_memory_item_from_json(cJSON *json, claw_memory_item_t *item)
{
    cJSON *summary_ids;
    size_t i;

    memset(item, 0, sizeof(*item));
    claw_memory_read_item_field_string(json, "id", item->id, sizeof(item->id));
    claw_memory_read_item_field_string(json, "source", item->source, sizeof(item->source));
    claw_memory_read_item_field_string(json, "content", item->content, sizeof(item->content));
    claw_memory_read_item_field_string(json, "tags", item->tags, sizeof(item->tags));
    claw_memory_read_item_field_string(json, "keywords", item->keywords, sizeof(item->keywords));
    item->created_at = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(json, "created_at"));
    item->updated_at = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(json, "updated_at"));
    item->access_count = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(json, "access_count"));
    item->deleted = (uint8_t)cJSON_IsTrue(cJSON_GetObjectItem(json, "deleted"));

    summary_ids = cJSON_GetObjectItem(json, "summary_ids");
    if (cJSON_IsArray(summary_ids)) {
        item->summary_id_count = (uint8_t)cJSON_GetArraySize(summary_ids);
        if (item->summary_id_count > CLAW_MEMORY_MAX_SUMMARIES) {
            item->summary_id_count = CLAW_MEMORY_MAX_SUMMARIES;
        }
        for (i = 0; i < item->summary_id_count; i++) {
            cJSON *entry = cJSON_GetArrayItem(summary_ids, (int)i);
            item->summary_ids[i] = (uint16_t)cJSON_GetNumberValue(entry);
        }
    }
}

cJSON *claw_memory_item_to_json(const claw_memory_item_t *item, cJSON *index_root)
{
    cJSON *json = NULL;
    cJSON *summary_ids = NULL;
    cJSON *summary_labels = NULL;
    size_t i;

    if (!item) {
        return NULL;
    }

    json = cJSON_CreateObject();
    summary_ids = cJSON_CreateArray();
    summary_labels = cJSON_CreateArray();
    if (!json || !summary_ids || !summary_labels) {
        cJSON_Delete(json);
        cJSON_Delete(summary_ids);
        cJSON_Delete(summary_labels);
        return NULL;
    }

    cJSON_AddStringToObject(json, "id", item->id);
    cJSON_AddStringToObject(json, "source", item->source);
    cJSON_AddStringToObject(json, "content", item->content);
    cJSON_AddStringToObject(json, "tags", item->tags);
    cJSON_AddStringToObject(json, "keywords", item->keywords);
    cJSON_AddNumberToObject(json, "created_at", item->created_at);
    cJSON_AddNumberToObject(json, "updated_at", item->updated_at);
    cJSON_AddNumberToObject(json, "access_count", item->access_count);
    cJSON_AddBoolToObject(json, "deleted", item->deleted != 0);

    for (i = 0; i < item->summary_id_count && i < CLAW_MEMORY_MAX_SUMMARIES; i++) {
        cJSON *summary = index_root ? claw_memory_find_summary_by_id(index_root, item->summary_ids[i]) : NULL;
        const char *label = summary ? cJSON_GetStringValue(cJSON_GetObjectItem(summary, "label")) : NULL;

        cJSON_AddItemToArray(summary_ids, cJSON_CreateNumber(item->summary_ids[i]));
        if (label) {
            cJSON_AddItemToArray(summary_labels, cJSON_CreateString(label));
        }
    }
    cJSON_AddItemToObject(json, "summary_ids", summary_ids);
    cJSON_AddItemToObject(json, "summary_labels", summary_labels);
    return json;
}

esp_err_t claw_memory_append_record(const claw_memory_item_t *item)
{
    cJSON *json = NULL;
    char *text = NULL;
    char *line = NULL;
    esp_err_t err;

    if (!item) {
        return ESP_ERR_INVALID_ARG;
    }

    json = cJSON_CreateObject();
    if (!json) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(json, "id", item->id);
    cJSON_AddStringToObject(json, "source", item->source);
    cJSON_AddStringToObject(json, "content", item->content);
    cJSON_AddStringToObject(json, "tags", item->tags);
    cJSON_AddStringToObject(json, "keywords", item->keywords);
    cJSON_AddNumberToObject(json, "created_at", item->created_at);
    cJSON_AddNumberToObject(json, "updated_at", item->updated_at);
    cJSON_AddNumberToObject(json, "access_count", item->access_count);
    cJSON_AddBoolToObject(json, "deleted", item->deleted != 0);
    {
        cJSON *summary_ids = cJSON_CreateArray();
        size_t i;

        if (!summary_ids) {
            cJSON_Delete(json);
            return ESP_ERR_NO_MEM;
        }
        for (i = 0; i < item->summary_id_count && i < CLAW_MEMORY_MAX_SUMMARIES; i++) {
            cJSON_AddItemToArray(summary_ids, cJSON_CreateNumber(item->summary_ids[i]));
        }
        cJSON_AddItemToObject(json, "summary_ids", summary_ids);
    }

    text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }

    line = dup_printf("%s\n", text);
    free(text);
    if (!line) {
        return ESP_ERR_NO_MEM;
    }

    err = append_file_text(s_memory.records_path, line);
    free(line);
    return err;
}

esp_err_t claw_memory_load_current_items(claw_memory_item_list_t *out_list)
{
    FILE *file = NULL;
    char *line = NULL;
    claw_memory_item_t *item = NULL;
    claw_memory_item_list_t list = {0};
    const size_t line_size = 1024;

    if (!out_list) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_list, 0, sizeof(*out_list));

    line = calloc(1, line_size);
    item = calloc(1, sizeof(*item));
    if (!line || !item) {
        free(line);
        free(item);
        return ESP_ERR_NO_MEM;
    }

    file = fopen(s_memory.records_path, "r");
    if (!file) {
        free(line);
        free(item);
        if (errno == ENOENT) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Failed to open memory records: %s", s_memory.records_path);
        return ESP_FAIL;
    }

    while (fgets(line, (int)line_size, file)) {
        cJSON *json = NULL;
        int idx;

        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) {
            continue;
        }

        json = cJSON_Parse(line);
        if (!json) {
            continue;
        }
        claw_memory_item_from_json(json, item);
        cJSON_Delete(json);
        if (!item->id[0]) {
            continue;
        }

        idx = claw_memory_find_item_index(&list, item->id);
        if (idx >= 0) {
            list.items[idx] = *item;
        } else if (claw_memory_item_list_push(&list, item) != ESP_OK) {
            fclose(file);
            free(line);
            free(item);
            claw_memory_item_list_free(&list);
            return ESP_ERR_NO_MEM;
        }
    }
    fclose(file);
    free(line);
    free(item);
    *out_list = list;
    return ESP_OK;
}

int claw_memory_sort_by_priority_desc(const void *a, const void *b)
{
    const claw_memory_item_t *ia = (const claw_memory_item_t *)a;
    const claw_memory_item_t *ib = (const claw_memory_item_t *)b;

    if (ia->access_count != ib->access_count) {
        return (int)ib->access_count - (int)ia->access_count;
    }
    if (ia->updated_at != ib->updated_at) {
        return (ib->updated_at > ia->updated_at) ? 1 : -1;
    }
    return strcmp(ia->id, ib->id);
}

static void claw_memory_format_timestamp(uint32_t ts, char *buf, size_t buf_size)
{
    time_t raw_ts;
    struct tm tm_info;

    if (!buf || buf_size == 0) {
        return;
    }

    buf[0] = '\0';
    if (ts == 0) {
        safe_copy(buf, buf_size, "-");
        return;
    }

    raw_ts = (time_t)ts;
    if (!localtime_r(&raw_ts, &tm_info) ||
        strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S", &tm_info) == 0) {
        snprintf(buf, buf_size, "%" PRIu32, ts);
    }
}

void claw_memory_append_digest_line(const char *action,
                                    const claw_memory_item_t *item,
                                    const char *extra)
{
    char timestamp[16];
    char *line;

    snprintf(timestamp, sizeof(timestamp), "%" PRIu32, claw_memory_now_sec());
    line = dup_printf("%s\t%s\t%s%s%s\n",
                      timestamp,
                      action ? action : "unknown",
                      item && item->id[0] ? item->id : "-",
                      extra ? "\t" : "",
                      extra ? extra : "");
    if (!line) {
        return;
    }
    append_file_text(s_memory.digest_path, line);
    free(line);
}

static void claw_memory_keyword_index_add_term(cJSON *keyword_index,
                                               const char *memory_id,
                                               const char *term)
{
    cJSON *entry = NULL;
    cJSON *id_item = NULL;
    char normalized[48];
    bool exists = false;

    if (!keyword_index || !memory_id || !memory_id[0] || !term || !term[0]) {
        return;
    }

    safe_copy(normalized, sizeof(normalized), term);
    trim_whitespace(normalized);
    if (!normalized[0]) {
        return;
    }

    entry = cJSON_GetObjectItem(keyword_index, normalized);
    if (!cJSON_IsArray(entry)) {
        entry = cJSON_CreateArray();
        if (!entry) {
            return;
        }
        cJSON_AddItemToObject(keyword_index, normalized, entry);
    }

    cJSON_ArrayForEach(id_item, entry) {
        const char *existing = cJSON_GetStringValue(id_item);
        if (existing && strcmp(existing, memory_id) == 0) {
            exists = true;
            break;
        }
    }
    if (!exists) {
        cJSON_AddItemToArray(entry, cJSON_CreateString(memory_id));
    }
}

static void claw_memory_keyword_index_add_csv(cJSON *keyword_index,
                                              const char *memory_id,
                                              const char *csv_text)
{
    char csv_copy[160];
    char *token = NULL;
    char *saveptr = NULL;

    if (!keyword_index || !memory_id || !memory_id[0] || !csv_text || !csv_text[0]) {
        return;
    }

    safe_copy(csv_copy, sizeof(csv_copy), csv_text);
    token = strtok_r(csv_copy, ",;/|", &saveptr);
    while (token) {
        claw_memory_keyword_index_add_term(keyword_index, memory_id, token);
        token = strtok_r(NULL, ",;/|", &saveptr);
    }
}

void claw_memory_rebuild_keyword_index(cJSON *index_root,
                                       const claw_memory_item_list_t *items)
{
    cJSON *keyword_index = cJSON_CreateObject();
    size_t i;

    if (!index_root || !keyword_index) {
        cJSON_Delete(keyword_index);
        return;
    }

    for (i = 0; items && i < items->count; i++) {
        const claw_memory_item_t *item = &items->items[i];

        if (item->deleted || !item->id[0]) {
            continue;
        }
        claw_memory_keyword_index_add_csv(keyword_index, item->id, item->keywords);
        if (!item->keywords[0]) {
            claw_memory_keyword_index_add_csv(keyword_index, item->id, item->tags);
        }
    }
    /* Replace only takes ownership when the key exists; delete+add handles a
     * root missing keyword_index without leaking the freshly built object
     * (consistent with claw_memory_load_index). */
    cJSON_DeleteItemFromObject(index_root, "keyword_index");
    cJSON_AddItemToObject(index_root, "keyword_index", keyword_index);
}

esp_err_t claw_memory_export_markdown_internal(char **out_markdown,
                                               const claw_memory_item_list_t *items,
                                               cJSON *index_root)
{
    cJSON *summaries = NULL;
    cJSON *summary = NULL;
    size_t buf_size;
    size_t off = 0;
    char *buf;
    claw_memory_item_list_t sorted = {0};
    size_t i;

    if (!out_markdown) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_markdown = NULL;

    buf_size = 1024 + ((items ? items->count : 0) * 512);
    buf = calloc(1, buf_size);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    off += snprintf(buf + off, buf_size - off, "# Long-term Memory\n\n");
    summaries = index_root ? cJSON_GetObjectItem(index_root, "summaries") : NULL;
    if (cJSON_IsArray(summaries) && cJSON_GetArraySize(summaries) > 0) {
        off += snprintf(buf + off,
                        buf_size - off,
                        "## Summary Labels\n\n");
        cJSON_ArrayForEach(summary, summaries) {
            const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(summary, "label"));
            int ref_count = cJSON_GetObjectItem(summary, "ref_count") ?
                cJSON_GetObjectItem(summary, "ref_count")->valueint : 0;

            if (!label) {
                continue;
            }
            off += snprintf(buf + off,
                            buf_size - off,
                            "- %s (refs=%d)\n",
                            label,
                            ref_count);
            if (off >= buf_size) {
                break;
            }
        }
        off += snprintf(buf + off, buf_size - off, "\n");
    }

    if (!items || items->count == 0) {
        off += snprintf(buf + off,
                        buf_size - off,
                        "(empty - Claw will write memories here as it learns)\n");
        *out_markdown = buf;
        return ESP_OK;
    }

    sorted.capacity = items->count;
    sorted.count = items->count;
    sorted.items = calloc(items->count, sizeof(*sorted.items));
    if (!sorted.items) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(sorted.items, items->items, items->count * sizeof(*sorted.items));
    qsort(sorted.items, sorted.count, sizeof(*sorted.items), claw_memory_sort_by_priority_desc);

    off += snprintf(buf + off, buf_size - off, "## Active Memories\n\n");
    for (i = 0; i < sorted.count && off + 64 < buf_size; i++) {
        cJSON *item_json;
        char *labels_json;
        char updated_at_text[24];
        const claw_memory_item_t *item = &sorted.items[i];

        if (item->deleted) {
            continue;
        }
        item_json = claw_memory_item_to_json(item, index_root);
        labels_json = item_json ? cJSON_PrintUnformatted(cJSON_GetObjectItem(item_json, "summary_labels")) : NULL;
        claw_memory_format_timestamp(item->updated_at, updated_at_text, sizeof(updated_at_text));
        off += snprintf(buf + off,
                        buf_size - off,
                        "- `%s` %s\n"
                        "  time=%s access=%u labels=%s\n",
                        item->id,
                        item->content,
                        updated_at_text,
                        item->access_count,
                        labels_json ? labels_json : "[]");
        free(labels_json);
        cJSON_Delete(item_json);
    }

    claw_memory_item_list_free(&sorted);
    *out_markdown = buf;
    return ESP_OK;
}

esp_err_t claw_memory_sync_markdown(const claw_memory_item_list_t *items, cJSON *index_root)
{
    char *markdown = NULL;
    esp_err_t err;

    err = claw_memory_export_markdown_internal(&markdown, items, index_root);
    if (err != ESP_OK) {
        return err;
    }
    err = write_file_text(s_memory.markdown_path, markdown);
    free(markdown);
    return err;
}

static int claw_memory_capacity_score(const claw_memory_item_t *item)
{
    int score = 0;

    if (!item) {
        return -1000000;
    }
    score += (int)item->access_count * 3;
    score += (int)(item->updated_at / 3600U);
    return score;
}

static void claw_memory_trim_to_capacity(claw_memory_item_list_t *items)
{
    while (items && items->count > CLAW_MEMORY_MAX_ACTIVE_ITEMS) {
        size_t i;
        size_t remove_idx = 0;
        int lowest_score = claw_memory_capacity_score(&items->items[0]);

        for (i = 1; i < items->count; i++) {
            int score = claw_memory_capacity_score(&items->items[i]);
            if (score < lowest_score) {
                lowest_score = score;
                remove_idx = i;
            }
        }
        if (remove_idx + 1 < items->count) {
            memmove(&items->items[remove_idx],
                    &items->items[remove_idx + 1],
                    (items->count - remove_idx - 1) * sizeof(*items->items));
        }
        items->count--;
    }
}

static size_t claw_memory_index_last_compact_digest_size(cJSON *index_root)
{
    cJSON *item;

    if (!index_root) {
        return 0;
    }
    item = cJSON_GetObjectItem(index_root, "last_compact_digest_size");
    if (!cJSON_IsNumber(item) || item->valuedouble < 0) {
        return 0;
    }
    return (size_t)item->valuedouble;
}

static void claw_memory_set_index_last_compact_digest_size(cJSON *index_root, size_t size)
{
    if (!index_root) {
        return;
    }
    cJSON_DeleteItemFromObject(index_root, "last_compact_digest_size");
    cJSON_AddNumberToObject(index_root, "last_compact_digest_size", (double)size);
}

static void claw_memory_apply_digest_recall_deltas(claw_memory_item_list_t *items,
                                                   cJSON *index_root,
                                                   size_t digest_size)
{
    FILE *file = NULL;
    char line[256];
    size_t start_offset;
    size_t i;

    if (!items || !index_root) {
        return;
    }

    start_offset = claw_memory_index_last_compact_digest_size(index_root);
    if (start_offset >= digest_size) {
        claw_memory_set_index_last_compact_digest_size(index_root, digest_size);
        return;
    }

    file = fopen(s_memory.digest_path, "r");
    if (!file) {
        claw_memory_set_index_last_compact_digest_size(index_root, digest_size);
        return;
    }
    if (fseek(file, (long)start_offset, SEEK_SET) != 0) {
        fclose(file);
        claw_memory_set_index_last_compact_digest_size(index_root, digest_size);
        return;
    }

    while (fgets(line, sizeof(line), file)) {
        char *fields[5] = {0};
        char *saveptr = NULL;
        char *token = strtok_r(line, "\t\r\n", &saveptr);
        int field_count = 0;

        while (token && field_count < 5) {
            fields[field_count++] = token;
            token = strtok_r(NULL, "\t\r\n", &saveptr);
        }
        if (field_count < 4) {
            continue;
        }
        if (strcmp(fields[1], "recall") != 0 || strcmp(fields[3], "access_count+1") != 0) {
            continue;
        }

        for (i = 0; i < items->count; i++) {
            claw_memory_item_t *item = &items->items[i];

            if (strcmp(item->id, fields[2]) != 0) {
                continue;
            }
            if (item->access_count < UINT16_MAX) {
                item->access_count++;
            }
            if (fields[0] && fields[0][0]) {
                unsigned long ts = strtoul(fields[0], NULL, 10);

                if (ts > item->updated_at) {
                    item->updated_at = (uint32_t)ts;
                }
            }
            break;
        }
    }

    fclose(file);
    claw_memory_set_index_last_compact_digest_size(index_root, digest_size);
}

esp_err_t claw_memory_compact_internal(bool append_digest)
{
    claw_memory_item_list_t items = {0};
    claw_memory_item_list_t compacted = {0};
    cJSON *old_index = NULL;
    cJSON *new_index = NULL;
    char *records_text = NULL;
    claw_memory_item_t *scratch_item = NULL;
    size_t digest_size = 0;
    size_t i;
    esp_err_t err;

    scratch_item = calloc(1, sizeof(*scratch_item));
    if (!scratch_item) {
        return ESP_ERR_NO_MEM;
    }

    err = claw_memory_load_current_items(&items);
    if (err != ESP_OK) {
        free(scratch_item);
        return err;
    }
    err = claw_memory_load_index(&old_index);
    if (err != ESP_OK) {
        free(scratch_item);
        claw_memory_item_list_free(&items);
        return err;
    }
    digest_size = file_size_bytes(s_memory.digest_path);
    claw_memory_apply_digest_recall_deltas(&items, old_index, digest_size);

    for (i = 0; i < items.count; i++) {
        const claw_memory_item_t *src_item = &items.items[i];
        char dedup_key[80];
        int existing;

        if (src_item->deleted) {
            continue;
        }

        *scratch_item = *src_item;
        claw_memory_build_item_key(scratch_item, dedup_key, sizeof(dedup_key));
        if (!dedup_key[0]) {
            normalize_text_for_key(scratch_item->content, dedup_key, sizeof(dedup_key));
        }
        existing = -1;
        if (dedup_key[0]) {
            size_t j;
            for (j = 0; j < compacted.count; j++) {
                char other_key[80];

                claw_memory_build_item_key(&compacted.items[j], other_key, sizeof(other_key));
                if (!other_key[0]) {
                    normalize_text_for_key(compacted.items[j].content, other_key, sizeof(other_key));
                }
                if (strcmp(other_key, dedup_key) == 0) {
                    existing = (int)j;
                    break;
                }
            }
        }

        if (existing >= 0) {
            claw_memory_item_t *dst = &compacted.items[existing];

            if (scratch_item->access_count > dst->access_count) {
                dst->access_count = scratch_item->access_count;
            }
            if (scratch_item->updated_at > dst->updated_at) {
                dst->updated_at = scratch_item->updated_at;
                safe_copy(dst->content, sizeof(dst->content), scratch_item->content);
                safe_copy(dst->tags, sizeof(dst->tags), scratch_item->tags);
                safe_copy(dst->source, sizeof(dst->source), scratch_item->source);
            }
            if (scratch_item->created_at &&
                (!dst->created_at || scratch_item->created_at < dst->created_at)) {
                dst->created_at = scratch_item->created_at;
            }
        } else if ((err = claw_memory_item_list_push(&compacted, scratch_item)) != ESP_OK) {
            free(scratch_item);
            cJSON_Delete(old_index);
            claw_memory_item_list_free(&items);
            claw_memory_item_list_free(&compacted);
            return err;
        }
    }

    claw_memory_trim_to_capacity(&compacted);
    new_index = claw_memory_new_index_root();
    if (!new_index) {
        free(scratch_item);
        cJSON_Delete(old_index);
        claw_memory_item_list_free(&items);
        claw_memory_item_list_free(&compacted);
        return ESP_ERR_NO_MEM;
    }

    for (i = 0; i < compacted.count; i++) {
        claw_memory_item_t *item = &compacted.items[i];
        char labels[CLAW_MEMORY_MAX_SUMMARIES][CLAW_MEMORY_MAX_LABEL_TEXT];
        size_t label_count = 0;
        size_t j;

        item->summary_id_count = 0;
        claw_memory_collect_summary_labels(item, labels, &label_count);
        for (j = 0; j < label_count && j < CLAW_MEMORY_MAX_SUMMARIES; j++) {
            int old_id = 0;
            int new_id = 0;

            if (old_index) {
                cJSON *old_summary = claw_memory_find_summary_by_label(old_index, labels[j]);
                if (old_summary) {
                    old_id = cJSON_GetObjectItem(old_summary, "summary_id")->valueint;
                }
            }
            if (claw_memory_ensure_summary_label(new_index, labels[j], old_id, &new_id) != ESP_OK) {
                continue;
            }
            item->summary_ids[item->summary_id_count++] = (uint16_t)new_id;
        }
        claw_memory_adjust_summary_stats(new_index, item, 1);
    }
    claw_memory_rebuild_keyword_index(new_index, &compacted);
    claw_memory_set_index_last_compact_digest_size(new_index, digest_size);

    {
        size_t est_size = (compacted.count * 512) + 16;
        size_t off = 0;

        records_text = calloc(1, est_size);
        if (!records_text) {
            free(scratch_item);
            cJSON_Delete(old_index);
            cJSON_Delete(new_index);
            claw_memory_item_list_free(&items);
            claw_memory_item_list_free(&compacted);
            return ESP_ERR_NO_MEM;
        }
        for (i = 0; i < compacted.count; i++) {
            cJSON *json = claw_memory_item_to_json(&compacted.items[i], NULL);
            char *text = json ? cJSON_PrintUnformatted(json) : NULL;

            cJSON_Delete(json);
            if (!text) {
                continue;
            }
            off += snprintf(records_text + off, est_size - off, "%s\n", text);
            free(text);
            if (off >= est_size) {
                break;
            }
        }
    }

    err = write_file_text(s_memory.records_path, records_text ? records_text : "");
    if (err == ESP_OK) {
        err = claw_memory_save_index(new_index);
    }
    if (err == ESP_OK) {
        err = claw_memory_sync_markdown(&compacted, new_index);
    }
    if (err == ESP_OK && append_digest) {
        claw_memory_append_digest_line("compact", NULL, "rebuild=index+markdown");
    }

    free(records_text);
    free(scratch_item);
    cJSON_Delete(old_index);
    cJSON_Delete(new_index);
    claw_memory_item_list_free(&items);
    claw_memory_item_list_free(&compacted);
    s_memory.write_changes_since_compact = 0;
    return err;
}

esp_err_t claw_memory_maybe_compact(void)
{
    if (s_memory.write_changes_since_compact >= CLAW_MEMORY_COMPACT_CHANGE_THRESHOLD ||
        file_size_bytes(s_memory.records_path) >= CLAW_MEMORY_COMPACT_SIZE_THRESHOLD) {
        return claw_memory_compact_internal(true);
    }
    return ESP_OK;
}
