/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "app_registry.h"
#include "cap_app_mgr.h"
#include "claw_cap.h"
#include "esp_log.h"

static const char *TAG = "cap_app_mgr";

#define CAP_APP_STRINGIFY_INNER(value) #value
#define CAP_APP_STRINGIFY(value) CAP_APP_STRINGIFY_INNER(value)
#define CAP_APP_ID_SCHEMA \
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{" \
    "\"app_id\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":" CAP_APP_STRINGIFY(APP_REGISTRY_ID_MAX_LEN) "," \
    "\"pattern\":\"^[A-Za-z0-9_-]+$\"}},\"required\":[\"app_id\"]}"

static void cap_app_write_error(char *output, size_t output_size, const char *code, const char *message, const char *app_id)
{
    if (!output || output_size == 0) {
        return;
    }
    if (app_id && app_id[0]) {
        snprintf(output, output_size, "{\"ok\":false,\"code\":\"%s\",\"error\":\"%s\",\"app_id\":\"%s\"}", code, message, app_id);
    } else {
        snprintf(output, output_size, "{\"ok\":false,\"code\":\"%s\",\"error\":\"%s\"}", code, message);
    }
}

static esp_err_t cap_app_parse_id(const char *input_json, char app_id[APP_REGISTRY_ID_MAX_LEN + 1], char *output, size_t output_size)
{
    cJSON *root = cJSON_ParseWithOpts(input_json ? input_json : "{}", NULL, true);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        cap_app_write_error(output, output_size, "invalid_input", "input must be a JSON object", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    cJSON *field = NULL;
    cJSON_ArrayForEach(field, root) {
        if (!field->string || strcmp(field->string, "app_id") != 0) {
            cJSON_Delete(root);
            cap_app_write_error(output, output_size, "invalid_input", "unknown input field", NULL);
            return ESP_ERR_INVALID_ARG;
        }
    }
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "app_id");
    if (!cJSON_IsString(item) || !app_registry_id_is_valid(item->valuestring)) {
        cJSON_Delete(root);
        cap_app_write_error(output, output_size, "invalid_app_id", "app_id must match ^[A-Za-z0-9_-]{1,63}$", NULL);
        return ESP_ERR_INVALID_ARG;
    }
    snprintf(app_id, APP_REGISTRY_ID_MAX_LEN + 1, "%s", item->valuestring);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t cap_app_write_success(const char *app_id, char *output, size_t output_size)
{
    int written;

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    written = snprintf(output, output_size, "{\"ok\":true,\"app_id\":\"%s\"}", app_id);
    return written >= 0 && (size_t)written < output_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t cap_app_publish_execute(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    char app_id[APP_REGISTRY_ID_MAX_LEN + 1] = {0};
    esp_err_t err;

    (void)ctx;
    err = cap_app_parse_id(input_json, app_id, output, output_size);
    if (err != ESP_OK) {
        return err;
    }
    err = app_registry_publish(app_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "publish %s failed: %s", app_id, esp_err_to_name(err));
        cap_app_write_error(output, output_size, err == ESP_ERR_NOT_FOUND ? "app_not_found" :
                            err == ESP_ERR_INVALID_ARG ? "invalid_app" : err == ESP_ERR_TIMEOUT ? "busy" : "publish_failed",
                            "failed to publish runtime app", app_id);
        return err;
    }
    return cap_app_write_success(app_id, output, output_size);
}

static esp_err_t cap_app_remove_execute(const char *input_json, const claw_cap_call_context_t *ctx, char *output, size_t output_size)
{
    char app_id[APP_REGISTRY_ID_MAX_LEN + 1] = {0};
    esp_err_t err;

    (void)ctx;
    err = cap_app_parse_id(input_json, app_id, output, output_size);
    if (err != ESP_OK) {
        return err;
    }
    err = app_registry_remove(app_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "remove %s failed: %s", app_id, esp_err_to_name(err));
        cap_app_write_error(output, output_size, err == ESP_ERR_NOT_FOUND ? "app_not_found" :
                            err == ESP_ERR_INVALID_STATE ? "readonly_app" : err == ESP_ERR_TIMEOUT ? "busy" : "remove_failed",
                            "failed to remove runtime app", app_id);
        return err;
    }
    return cap_app_write_success(app_id, output, output_size);
}

static const claw_cap_descriptor_t s_app_manage_descriptors[] = {
    {
        .id = "publish_app",
        .name = "publish_app",
        .family = "app",
        .description = "Validate and publish an existing runtime App after its files have been created or updated.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_APP_ID_SCHEMA,
        .execute = cap_app_publish_execute,
    },
    {
        .id = "remove_app",
        .name = "remove_app",
        .family = "app",
        .description = "Recursively remove a runtime App directory from writable storage and refresh the App registry.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = CAP_APP_ID_SCHEMA,
        .execute = cap_app_remove_execute,
    },
};

static const claw_cap_group_t s_app_manage_group = {
    .group_id = "cap_app_manage",
    .descriptors = s_app_manage_descriptors,
    .descriptor_count = sizeof(s_app_manage_descriptors) / sizeof(s_app_manage_descriptors[0]),
};

esp_err_t cap_app_mgr_register_group(void)
{
    if (claw_cap_group_exists(s_app_manage_group.group_id)) {
        return ESP_OK;
    }
    esp_err_t err = claw_cap_register_group(&s_app_manage_group);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", s_app_manage_group.group_id, esp_err_to_name(err));
    }
    return err;
}
