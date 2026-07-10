/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory.h"
#include "claw_memory_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_core_llm.h"

#define CLAW_MEMORY_GROUP_ID "claw_memory"

static void cap_memory_copy_string(cJSON *root, const char *key, char *dst, size_t dst_size)
{
    const char *value = cJSON_GetStringValue(cJSON_GetObjectItem(root, key));

    if (value && dst && dst_size > 0) {
        strlcpy(dst, value, dst_size);
    }
}

static void cap_memory_fill_primary_summary(const claw_memory_item_t *item,
                                            char *summary,
                                            size_t summary_size)
{
    if (!summary || summary_size == 0) {
        return;
    }

    summary[0] = '\0';
    if (!item) {
        return;
    }
    if (claw_memory_item_primary_summary_label(item, summary, summary_size) != ESP_OK) {
        summary[0] = '\0';
    }
}

static esp_err_t cap_memory_collect_summary_list(const claw_memory_item_t *item,
                                                 char **out_summary_list)
{
    if (!out_summary_list) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_summary_list = NULL;
    if (!item) {
        return ESP_OK;
    }

    return claw_memory_append_item_summary_labels(item, out_summary_list);
}

static bool cap_memory_json_array_has_string(cJSON *array, const char *value)
{
    cJSON *item = NULL;

    if (!cJSON_IsArray(array) || !value || !value[0]) {
        return false;
    }
    cJSON_ArrayForEach(item, array) {
        const char *existing = cJSON_GetStringValue(item);

        if (existing && strcmp(existing, value) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t cap_memory_render_invalid_summary_labels(const char *invalid_label,
                                                          const char *catalog_json,
                                                          char *output,
                                                          size_t output_size)
{
    cJSON *root = NULL;
    cJSON *catalog = NULL;
    char *rendered = NULL;
    esp_err_t err = ESP_OK;

    if (!invalid_label || !invalid_label[0] || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", "invalid_summary_labels");
    cJSON_AddStringToObject(root, "message", "summary_labels must be copied exactly from the current memory summary label catalog");
    cJSON_AddStringToObject(root, "invalid_label", invalid_label);

    catalog = cJSON_Parse(catalog_json ? catalog_json : "[]");
    if (catalog && cJSON_IsArray(catalog)) {
        cJSON_AddItemToObject(root, "available_summary_labels", catalog);
        catalog = NULL;
    } else {
        cJSON_Delete(catalog);
        cJSON_AddItemToObject(root, "available_summary_labels", cJSON_CreateArray());
    }

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return err;
}

static esp_err_t cap_memory_render_error(const char *error,
                                         char *output,
                                         size_t output_size)
{
    cJSON *root = NULL;
    char *rendered = NULL;

    if (!error || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error);
    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static esp_err_t cap_memory_collect_summary_catalog(char **out_catalog_json)
{
    cJSON *items = NULL;
    cJSON *catalog = NULL;
    cJSON *item = NULL;
    char *items_json = NULL;
    char *catalog_json = NULL;
    esp_err_t err;

    if (!out_catalog_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_catalog_json = NULL;

    err = claw_memory_list(&items_json);
    if (err != ESP_OK) {
        return err;
    }

    items = cJSON_Parse(items_json ? items_json : "[]");
    catalog = cJSON_CreateArray();
    if (!cJSON_IsArray(items) || !catalog) {
        cJSON_Delete(items);
        cJSON_Delete(catalog);
        free(items_json);
        return ESP_FAIL;
    }

    cJSON_ArrayForEach(item, items) {
        cJSON *labels = cJSON_GetObjectItem(item, "summary_labels");
        cJSON *label = NULL;

        if (!cJSON_IsArray(labels)) {
            continue;
        }
        cJSON_ArrayForEach(label, labels) {
            const char *text = cJSON_GetStringValue(label);

            if (text && text[0] && !cap_memory_json_array_has_string(catalog, text)) {
                cJSON_AddItemToArray(catalog, cJSON_CreateString(text));
            }
        }
    }

    if (cJSON_GetArraySize(catalog) == 0) {
        catalog_json = strdup("[]");
    } else {
        catalog_json = cJSON_PrintUnformatted(catalog);
    }

    cJSON_Delete(items);
    cJSON_Delete(catalog);
    free(items_json);
    if (!catalog_json) {
        free(catalog_json);
        return ESP_ERR_NO_MEM;
    }

    *out_catalog_json = catalog_json;
    return ESP_OK;
}

static const char *cap_memory_find_invalid_summary_label(const char *labels[],
                                                         size_t label_count,
                                                         const char *catalog_json)
{
    cJSON *catalog = NULL;
    const char *invalid_label = NULL;
    size_t i;

    if (!labels || label_count == 0) {
        return NULL;
    }

    catalog = cJSON_Parse(catalog_json ? catalog_json : "[]");
    if (!catalog || !cJSON_IsArray(catalog)) {
        cJSON_Delete(catalog);
        return label_count > 0 ? labels[0] : NULL;
    }

    for (i = 0; i < label_count; i++) {
        if (!labels[i] || !labels[i][0]) {
            continue;
        }
        if (!cap_memory_json_array_has_string(catalog, labels[i])) {
            invalid_label = labels[i];
            break;
        }
    }

    cJSON_Delete(catalog);
    return invalid_label;
}

static esp_err_t cap_memory_render_result(const char *action,
                                          const char *memory_id,
                                          const char *summary,
                                          bool changed,
                                          const char *items_json,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = NULL;
    cJSON *items = NULL;
    char *rendered = NULL;

    if (!action || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "action", action);
    cJSON_AddBoolToObject(root, "changed", changed);
    if (memory_id && memory_id[0]) {
        cJSON_AddStringToObject(root, "memory_id", memory_id);
    }
    if (summary && summary[0]) {
        cJSON_AddStringToObject(root, "summary", summary);
    }
    if (items_json) {
        items = cJSON_Parse(items_json);
        if (!items) {
            items = cJSON_CreateArray();
        }
        cJSON_AddItemToObject(root, "items", items);
    }

    rendered = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!rendered) {
        return ESP_ERR_NO_MEM;
    }

    snprintf(output, output_size, "%s", rendered);
    free(rendered);
    return ESP_OK;
}

static void cap_memory_record_pending_summary(const claw_cap_call_context_t *ctx,
                                              const char *summary,
                                              bool changed)
{
    if (!ctx || !changed) {
        return;
    }

    if (ctx->request_id != 0) {
        (void)claw_memory_request_mark_manual_write(ctx->request_id);
    }
    if (!ctx->session_id || !ctx->session_id[0] || !summary || !summary[0]) {
        return;
    }

    (void)claw_memory_note_session_summary(ctx->session_id, summary);
}

static esp_err_t cap_memory_store_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    cJSON *root = NULL;
    claw_memory_item_t *item = NULL;
    char summary[40] = {0};
    char *summary_list = NULL;
    bool changed = false;
    esp_err_t err;

    item = calloc(1, sizeof(*item));
    if (!item) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return ESP_ERR_NO_MEM;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        free(item);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid_json\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cap_memory_copy_string(root, "memory_id", item->id, sizeof(item->id));
    cap_memory_copy_string(root, "source", item->source, sizeof(item->source));
    cap_memory_copy_string(root, "content", item->content, sizeof(item->content));
    cap_memory_copy_string(root, "tags", item->tags, sizeof(item->tags));
    cap_memory_copy_string(root, "keywords", item->keywords, sizeof(item->keywords));
    cJSON_Delete(root);

    if (!item->content[0]) {
        free(item);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"content is required\"}");
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_memory_store_with_result(item, &changed);
    if (err != ESP_OK) {
        free(item);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"memory_store_failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }
    cap_memory_fill_primary_summary(item, summary, sizeof(summary));
    err = cap_memory_collect_summary_list(item, &summary_list);
    if (err != ESP_OK) {
        free(item);
        free(summary_list);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"memory_store_failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }
    cap_memory_record_pending_summary(ctx, summary_list, changed);
    err = cap_memory_render_result("memory_store",
                                   item->id,
                                   summary,
                                   changed,
                                   NULL,
                                   output,
                                   output_size);
    free(summary_list);
    free(item);
    return err;
}

static esp_err_t cap_memory_recall_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    cJSON *root = NULL;
    cJSON *labels = NULL;
    const char *label_values[3] = {0};
    const char *invalid_label = NULL;
    claw_memory_query_t query = {0};
    char *items_json = NULL;
    char *catalog_json = NULL;
    esp_err_t err;
    int label_count = 0;
    int i;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid_json\"}");
        return ESP_ERR_INVALID_ARG;
    }

    query.limit = (size_t)cJSON_GetNumberValue(cJSON_GetObjectItem(root, "limit"));
    labels = cJSON_GetObjectItem(root, "summary_labels");
    if (cJSON_IsArray(labels)) {
        for (i = 0; i < cJSON_GetArraySize(labels) && label_count < 3; i++) {
            const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(labels, i));
            if (label && label[0]) {
                label_values[label_count++] = label;
            }
        }
    }

    err = cap_memory_collect_summary_catalog(&catalog_json);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return cap_memory_render_error("memory_summary_catalog_unavailable", output, output_size);
    }

    invalid_label = cap_memory_find_invalid_summary_label(label_values,
                                                          (size_t)label_count,
                                                          catalog_json);
    if (invalid_label) {
        cJSON_Delete(root);
        err = cap_memory_render_invalid_summary_labels(invalid_label,
                                                       catalog_json,
                                                       output,
                                                       output_size);
        free(catalog_json);
        return err;
    }

    query.summary_labels = label_values;
    query.summary_label_count = (size_t)label_count;
    err = claw_memory_recall(&query, &items_json);
    cJSON_Delete(root);
    free(catalog_json);
    if (err != ESP_OK) {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"memory_recall_failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        free(items_json);
        return err;
    }

    err = cap_memory_render_result("memory_recall",
                                   NULL,
                                   NULL,
                                   true,
                                   items_json,
                                   output,
                                   output_size);
    free(items_json);
    return err;
}

static esp_err_t cap_memory_list_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    cJSON *root = NULL;
    char *items_json = NULL;
    esp_err_t err;

    (void)ctx;

    root = cJSON_Parse(input_json ? input_json : "{}");
    err = claw_memory_list(&items_json);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"memory_list_failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        free(items_json);
        return err;
    }

    err = cap_memory_render_result("memory_list",
                                   NULL,
                                   NULL,
                                   false,
                                   items_json,
                                   output,
                                   output_size);
    free(items_json);
    return err;
}

static esp_err_t cap_memory_update_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    cJSON *root = NULL;
    claw_memory_item_t *item = NULL;
    char summary[40] = {0};
    char *summary_list = NULL;
    bool changed = false;
    esp_err_t err;

    item = calloc(1, sizeof(*item));
    if (!item) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return ESP_ERR_NO_MEM;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        free(item);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid_json\"}");
        return ESP_ERR_INVALID_ARG;
    }

    cap_memory_copy_string(root, "memory_id", item->id, sizeof(item->id));
    cap_memory_copy_string(root, "source", item->source, sizeof(item->source));
    cap_memory_copy_string(root, "content", item->content, sizeof(item->content));
    cap_memory_copy_string(root, "tags", item->tags, sizeof(item->tags));
    cap_memory_copy_string(root, "keywords", item->keywords, sizeof(item->keywords));
    cJSON_Delete(root);

    if (!item->id[0]) {
        free(item);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"memory_id is required\"}");
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_memory_update_with_result(item, &changed);
    if (err != ESP_OK) {
        free(item);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"memory_update_failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }
    cap_memory_fill_primary_summary(item, summary, sizeof(summary));
    err = cap_memory_collect_summary_list(item, &summary_list);
    if (err != ESP_OK) {
        free(item);
        free(summary_list);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"memory_update_failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }
    cap_memory_record_pending_summary(ctx, summary_list, changed);
    err = cap_memory_render_result("memory_update",
                                   item->id,
                                   summary,
                                   changed,
                                   NULL,
                                   output,
                                   output_size);
    free(summary_list);
    free(item);
    return err;
}

static esp_err_t cap_memory_forget_execute(const char *input_json,
                                           const claw_cap_call_context_t *ctx,
                                           char *output,
                                           size_t output_size)
{
    cJSON *root = NULL;
    const char *memory_id = NULL;
    char memory_id_copy[40] = {0};
    claw_memory_item_t *item = NULL;
    bool changed = false;
    char summary[40] = {0};
    char *summary_list = NULL;
    esp_err_t err;

    item = calloc(1, sizeof(*item));
    if (!item) {
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"out_of_memory\"}");
        return ESP_ERR_NO_MEM;
    }

    root = cJSON_Parse(input_json ? input_json : "{}");
    if (!root) {
        free(item);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"invalid_json\"}");
        return ESP_ERR_INVALID_ARG;
    }
    memory_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "memory_id"));
    if (memory_id && memory_id[0]) {
        strlcpy(memory_id_copy, memory_id, sizeof(memory_id_copy));
        memory_id = memory_id_copy;
    }
    cJSON_Delete(root);

    if (!memory_id || !memory_id[0]) {
        free(item);
        snprintf(output, output_size, "{\"ok\":false,\"error\":\"memory_id is required\"}");
        return ESP_ERR_INVALID_ARG;
    }

    err = claw_memory_forget_with_result(memory_id, item, &changed);
    if (err != ESP_OK) {
        free(item);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"memory_forget_failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }
    cap_memory_fill_primary_summary(item, summary, sizeof(summary));
    err = cap_memory_collect_summary_list(item, &summary_list);
    if (err != ESP_OK) {
        free(item);
        free(summary_list);
        snprintf(output,
                 output_size,
                 "{\"ok\":false,\"error\":\"memory_forget_failed\",\"code\":\"%s\"}",
                 esp_err_to_name(err));
        return err;
    }
    cap_memory_record_pending_summary(ctx, summary_list, changed);
    err = cap_memory_render_result("memory_forget",
                                   memory_id,
                                   summary,
                                   changed,
                                   NULL,
                                   output,
                                   output_size);
    free(summary_list);
    free(item);
    return err;
}

static const claw_cap_descriptor_t s_memory_descriptors[] = {
    {
        .id = "memory_store",
        .name = "memory_store",
        .family = "memory",
        .description = "Store one on-device long-term memory item. Prefer normalized content plus precise LLM-generated tags and keywords.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"memory_id\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},\"tags\":{\"type\":\"string\"},\"keywords\":{\"type\":\"string\"}},\"required\":[\"content\"]}",
        .execute = cap_memory_store_execute,
    },
    {
        .id = "memory_recall",
        .name = "memory_recall",
        .family = "memory",
        .description = "Recall detailed long-term memories by exact summary_labels from the current catalog.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"summary_labels\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},\"limit\":{\"type\":\"integer\"}}}",
        .execute = cap_memory_recall_execute,
    },
    {
        .id = "memory_list",
        .name = "memory_list",
        .family = "memory",
        .description = "List active long-term memories.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_memory_list_execute,
    },
    {
        .id = "memory_update",
        .name = "memory_update",
        .family = "memory",
        .description = "Update one long-term memory item. Prefer normalized content plus precise LLM-generated tags and keywords.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"memory_id\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"},\"tags\":{\"type\":\"string\"},\"keywords\":{\"type\":\"string\"}},\"required\":[\"memory_id\"]}",
        .execute = cap_memory_update_execute,
    },
    {
        .id = "memory_forget",
        .name = "memory_forget",
        .family = "memory",
        .description = "Forget one long-term memory item by exact memory_id.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{\"memory_id\":{\"type\":\"string\"}},\"required\":[\"memory_id\"]}",
        .execute = cap_memory_forget_execute,
    },
};

static const claw_cap_group_t s_memory_group = {
    .group_id = CLAW_MEMORY_GROUP_ID,
    .descriptors = s_memory_descriptors,
    .descriptor_count = sizeof(s_memory_descriptors) / sizeof(s_memory_descriptors[0]),
};

esp_err_t claw_memory_register_group(void)
{
    if (claw_cap_group_exists(s_memory_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_memory_group);
}
