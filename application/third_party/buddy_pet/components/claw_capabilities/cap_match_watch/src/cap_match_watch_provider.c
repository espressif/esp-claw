/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_match_watch_provider.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "match_watch_runtime.h"

static void cap_match_watch_provider_set_notify_from_ctx(const claw_cap_call_context_t *ctx)
{
    if (ctx != NULL && ctx->chat_id != NULL && ctx->chat_id[0] != '\0') {
        (void)match_watch_runtime_set_notify_target(ctx->channel, ctx->chat_id);
    }
}

static void cap_match_watch_provider_write_status(char *output,
                                                  size_t output_size,
                                                  bool ok,
                                                  const char *status)
{
    if (output == NULL || output_size == 0) {
        return;
    }
    snprintf(output,
             output_size,
             "{\"ok\":%s,\"status\":\"%s\"}",
             ok ? "true" : "false",
             status != NULL ? status : "unknown");
}

static int cap_match_watch_json_int(const cJSON *root, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    return cJSON_IsNumber(item) ? item->valueint : default_value;
}

esp_err_t cap_match_watch_provider_start_execute(const char *input_json,
                                                 const claw_cap_call_context_t *ctx,
                                                 char *output,
                                                 size_t output_size)
{
    cJSON *root = NULL;
    cJSON *args = NULL;
    char *args_json = NULL;
    const char *path;
    const char *name;
    esp_err_t ret;

    cap_match_watch_provider_set_notify_from_ctx(ctx);

    root = cJSON_Parse(input_json);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        match_watch_runtime_provider_record_error("invalid provider start json");
        cap_match_watch_provider_write_status(output, output_size, false, "invalid_arg");
        return ESP_ERR_INVALID_ARG;
    }

    path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    name = cJSON_GetStringValue(cJSON_GetObjectItem(root, "name"));
    if (path == NULL || path[0] == '\0') {
        cJSON_Delete(root);
        match_watch_runtime_provider_record_error("provider path is required");
        cap_match_watch_provider_write_status(output, output_size, false, "missing_path");
        return ESP_ERR_INVALID_ARG;
    }
    if (name == NULL || name[0] == '\0') {
        cJSON_Delete(root);
        match_watch_runtime_provider_record_error("provider name is required");
        cap_match_watch_provider_write_status(output, output_size, false, "missing_name");
        return ESP_ERR_INVALID_ARG;
    }

    args = cJSON_GetObjectItemCaseSensitive(root, "args");
    if (args == NULL) {
        cJSON_Delete(root);
        match_watch_runtime_provider_record_error("provider args are required");
        cap_match_watch_provider_write_status(output, output_size, false, "missing_args");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsObject(args)) {
        cJSON_Delete(root);
        match_watch_runtime_provider_record_error("provider args must be an object");
        cap_match_watch_provider_write_status(output, output_size, false, "invalid_args");
        return ESP_ERR_INVALID_ARG;
    }
    args_json = cJSON_PrintUnformatted(args);
    if (args_json == NULL) {
        cJSON_Delete(root);
        match_watch_runtime_provider_record_error("failed to encode provider args");
        cap_match_watch_provider_write_status(output, output_size, false, "no_mem");
        return ESP_ERR_NO_MEM;
    }

    const char *team = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "team"));
    if (team == NULL || team[0] == '\0') {
        team = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "team_slug"));
    }
    if (team == NULL || team[0] == '\0') {
        team = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "team_code"));
    }
    if (team != NULL && team[0] != '\0') {
        const char *source = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "favorite_source"));
        (void)match_watch_runtime_set_favorite_with_source(team, NULL, source);
    }

    ret = match_watch_runtime_provider_start(path, args_json, name, output, output_size);
    free(args_json);
    cJSON_Delete(root);
    return ret;
}

esp_err_t cap_match_watch_provider_stop_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    cJSON *root = input_json != NULL ? cJSON_Parse(input_json) : NULL;
    uint32_t wait_ms = MATCH_WATCH_RUNTIME_PROVIDER_STOP_WAIT_MS;

    (void)ctx;

    if (cJSON_IsObject(root)) {
        int requested_wait = cap_match_watch_json_int(root, "wait_ms", (int)MATCH_WATCH_RUNTIME_PROVIDER_STOP_WAIT_MS);
        if (requested_wait > 0) {
            wait_ms = (uint32_t)requested_wait;
        }
    }
    cJSON_Delete(root);

    return match_watch_runtime_provider_stop(wait_ms, output, output_size);
}

esp_err_t cap_match_watch_provider_status_execute(const char *input_json,
                                                  const claw_cap_call_context_t *ctx,
                                                  char *output,
                                                  size_t output_size)
{
    (void)input_json;
    (void)ctx;

    return match_watch_runtime_provider_status(output, output_size);
}
