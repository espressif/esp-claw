/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_match_watch.h"
#include "cap_match_watch_provider.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "claw_event_publisher.h"
#include "esp_check.h"
#include "esp_log.h"
#include "match_watch_runtime.h"

static const char *TAG = "cap_match_watch";

static void cap_match_watch_set_notify_from_ctx(const claw_cap_call_context_t *ctx)
{
    if (ctx != NULL && ctx->chat_id != NULL && ctx->chat_id[0] != '\0') {
        (void)match_watch_runtime_set_notify_target(ctx->channel, ctx->chat_id);
    }
}

static const char *cap_match_watch_bool_str(bool value)
{
    return value ? "true" : "false";
}

static const char *cap_match_watch_status_for_err(esp_err_t ret, const char *ok_status)
{
    if (ret == ESP_OK) {
        return ok_status;
    }
    if (ret == ESP_ERR_NOT_FOUND) {
        return "team_not_found";
    }
    if (ret == ESP_ERR_INVALID_ARG) {
        return "invalid_arg";
    }
    return "error";
}

static const char *cap_match_watch_data_source_name(match_watch_data_source_t source)
{
    switch (source) {
    case MATCH_WATCH_DATA_SOURCE_EXTERNAL:
        return "external";
    case MATCH_WATCH_DATA_SOURCE_LIVE:
    default:
        return "live";
    }
}

static bool cap_match_watch_json_bool(const char *input_json, const char *key, bool default_value)
{
    bool value = default_value;
    cJSON *root = input_json != NULL ? cJSON_Parse(input_json) : NULL;
    cJSON *item;

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return value;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        value = cJSON_IsTrue(item);
    }
    cJSON_Delete(root);
    return value;
}

static bool cap_match_watch_json_has_bool(const char *input_json, const char *key, bool *out_value)
{
    cJSON *root = input_json != NULL ? cJSON_Parse(input_json) : NULL;
    cJSON *item;

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsBool(item)) {
        cJSON_Delete(root);
        return false;
    }
    if (out_value != NULL) {
        *out_value = cJSON_IsTrue(item);
    }
    cJSON_Delete(root);
    return true;
}

static const char *cap_match_watch_json_string(const char *input_json, const char *key,
                                               char *buf, size_t buf_size)
{
    cJSON *root = input_json != NULL ? cJSON_Parse(input_json) : NULL;
    cJSON *item;
    const char *value = NULL;

    if (buf != NULL && buf_size > 0) {
        buf[0] = '\0';
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        value = item->valuestring;
        if (buf != NULL && buf_size > 0) {
            strlcpy(buf, value, buf_size);
            value = buf;
        }
    }
    cJSON_Delete(root);
    return value;
}

static void cap_match_watch_write_status(char *output, size_t output_size,
                                         bool ok, const char *status, bool running)
{
    if (output == NULL || output_size == 0) {
        return;
    }

    snprintf(output,
             output_size,
             "{\"ok\":%s,\"status\":\"%s\",\"running\":%s}",
             ok ? "true" : "false",
             status != NULL ? status : "unknown",
             running ? "true" : "false");
}

static void cap_match_watch_write_runtime_status(char *output, size_t output_size,
                                                 bool ok, const char *status)
{
    match_watch_runtime_status_t runtime_status = {0};

    if (output == NULL || output_size == 0) {
        return;
    }
    (void)match_watch_runtime_get_status(&runtime_status);
    snprintf(output,
             output_size,
             "{\"ok\":%s,\"status\":\"%s\",\"running\":%s,"
             "\"reminders\":%s,\"favorite_team\":\"%s\","
             "\"favorite_team_source\":\"%s\","
             "\"active_competition\":\"%s\","
             "\"data_source\":\"%s\","
             "\"notify_channel\":\"%s\","
             "\"notify_chat_id\":\"%s\"}",
             cap_match_watch_bool_str(ok),
             status != NULL ? status : "unknown",
             cap_match_watch_bool_str(runtime_status.running),
             cap_match_watch_bool_str(runtime_status.reminders_enabled),
             runtime_status.favorite_team,
             runtime_status.favorite_team_source,
             runtime_status.active_competition,
             cap_match_watch_data_source_name(runtime_status.data_source),
             runtime_status.notify_channel,
             runtime_status.notify_chat_id);
}

static void cap_match_watch_notification_cb(const match_watch_runtime_notification_t *notification,
                                            void *user_data)
{
    claw_event_t event = {0};
    cJSON *payload = NULL;
    char *payload_json = NULL;
    uint32_t now_ms;
    esp_err_t ret;

    (void)user_data;
    if (notification == NULL || notification->channel[0] == '\0' ||
            notification->chat_id[0] == '\0' || notification->message[0] == '\0') {
        return;
    }

    now_ms = esp_log_timestamp();
    strlcpy(event.source_cap, "match_watch", sizeof(event.source_cap));
    strlcpy(event.event_type, "match_watch_notify", sizeof(event.event_type));
    strlcpy(event.source_channel, notification->channel, sizeof(event.source_channel));
    strlcpy(event.chat_id, notification->chat_id, sizeof(event.chat_id));
    strlcpy(event.sender_id, "match_watch", sizeof(event.sender_id));
    strlcpy(event.content_type, "text", sizeof(event.content_type));
    event.timestamp_ms = now_ms;
    event.session_policy = CLAW_SESSION_POLICY_TRIGGER;
    snprintf(event.event_id, sizeof(event.event_id), "match-%s-%u-%u",
             notification->kind, (unsigned)notification->match_no, (unsigned)now_ms);
    strlcpy(event.message_id, event.event_id, sizeof(event.message_id));
    strlcpy(event.correlation_id, event.event_id, sizeof(event.correlation_id));
    event.text = (char *)notification->message;
    payload = cJSON_CreateObject();
    if (payload != NULL) {
        cJSON_AddStringToObject(payload, "kind", notification->kind);
        cJSON_AddNumberToObject(payload, "match_no", notification->match_no);
        cJSON_AddStringToObject(payload, "source", cap_match_watch_data_source_name(notification->source));
        cJSON_AddStringToObject(payload, "raw_message", notification->message);
        payload_json = cJSON_PrintUnformatted(payload);
        cJSON_Delete(payload);
        payload = NULL;
    }
    event.payload_json = payload_json;

    ESP_LOGD(TAG, "match notification raw message: %s", notification->message);
    ret = claw_event_router_publish(&event);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "publish match notification: kind=%s match=%u event=%s",
                 notification->kind, (unsigned)notification->match_no, event.event_id);
    } else {
        ESP_LOGW(TAG, "publish match notification failed: kind=%s channel=%s chat=%s err=%s",
                 notification->kind, notification->channel, notification->chat_id, esp_err_to_name(ret));
    }
    free(payload_json);
}

static esp_err_t cap_match_watch_open_execute(const char *input_json,
                                              const claw_cap_call_context_t *ctx,
                                              char *output,
                                              size_t output_size)
{
    esp_err_t ret;
    bool already_running = false;
    bool reminders_enabled = false;
    match_watch_runtime_open_config_t config = {0};
    char team[MATCH_WATCH_RUNTIME_TEAM_NAME_LEN] = {0};
    char competition[48] = {0};
    char favorite_source[16] = {0};

    cap_match_watch_set_notify_from_ctx(ctx);

    config.team = cap_match_watch_json_string(input_json, "team", team, sizeof(team));
    config.competition = cap_match_watch_json_string(input_json, "competition", competition, sizeof(competition));
    config.favorite_source = cap_match_watch_json_string(input_json,
                                                         "favorite_source",
                                                         favorite_source,
                                                         sizeof(favorite_source));
    if (cap_match_watch_json_has_bool(input_json, "reminders", &reminders_enabled)) {
        config.reminders_set = true;
        config.reminders_enabled = reminders_enabled;
    }

    ret = match_watch_runtime_open(&config, &already_running);
    cap_match_watch_write_runtime_status(output, output_size, ret == ESP_OK,
                                         ret == ESP_OK ? (already_running ? "already_running" : "started") : "no_mem");
    return ret;
}

static esp_err_t cap_match_watch_set_favorite_execute(const char *input_json,
                                                      const claw_cap_call_context_t *ctx,
                                                      char *output,
                                                      size_t output_size)
{
    char team[MATCH_WATCH_RUNTIME_TEAM_NAME_LEN] = {0};
    char competition[48] = {0};
    const char *value;
    const char *competition_value;
    esp_err_t ret;

    cap_match_watch_set_notify_from_ctx(ctx);

    value = cap_match_watch_json_string(input_json, "team", team, sizeof(team));
    competition_value = cap_match_watch_json_string(input_json, "competition", competition, sizeof(competition));
    ret = match_watch_runtime_set_favorite(value, competition_value);
    cap_match_watch_write_runtime_status(output, output_size, ret == ESP_OK,
                                         cap_match_watch_status_for_err(ret, "favorite_updated"));
    return ret;
}

static esp_err_t cap_match_watch_push_data_execute(const char *input_json,
                                                   const claw_cap_call_context_t *ctx,
                                                   char *output,
                                                   size_t output_size)
{
    esp_err_t ret;

    ESP_LOGI(TAG, "match_watch_push_data request: caller=%d source=%s channel=%s chat=%s bytes=%u",
             ctx != NULL ? (int)ctx->caller : -1,
             ctx != NULL && ctx->source_cap != NULL && ctx->source_cap[0] != '\0' ? ctx->source_cap : "<none>",
             ctx != NULL && ctx->channel != NULL && ctx->channel[0] != '\0' ? ctx->channel : "<none>",
             ctx != NULL && ctx->chat_id != NULL && ctx->chat_id[0] != '\0' ? ctx->chat_id : "<none>",
             input_json != NULL ? (unsigned)strlen(input_json) : 0U);
    cap_match_watch_set_notify_from_ctx(ctx);
    ret = match_watch_runtime_push_data_json(input_json);
    cap_match_watch_write_runtime_status(output, output_size, ret == ESP_OK,
                                         ret == ESP_OK ? "data_pushed" :
                                         (ret == ESP_ERR_NOT_FOUND ? "no_matches" : "invalid_data"));
    return ret;
}

static esp_err_t cap_match_watch_set_reminders_execute(const char *input_json,
                                                       const claw_cap_call_context_t *ctx,
                                                       char *output,
                                                       size_t output_size)
{
    bool enabled;
    esp_err_t ret;

    cap_match_watch_set_notify_from_ctx(ctx);

    enabled = cap_match_watch_json_bool(input_json, "enabled", true);
    ret = match_watch_runtime_set_reminders(enabled);
    cap_match_watch_write_runtime_status(output, output_size, ret == ESP_OK,
                                         ret == ESP_OK ? (enabled ? "reminders_enabled" : "reminders_disabled") : "error");
    return ret;
}

static esp_err_t cap_match_watch_close_execute(const char *input_json,
                                               const claw_cap_call_context_t *ctx,
                                               char *output,
                                               size_t output_size)
{
    bool was_running;
    esp_err_t ret;

    (void)input_json;
    (void)ctx;

    was_running = match_watch_runtime_is_running();
    ret = match_watch_runtime_close();
    cap_match_watch_write_status(output, output_size, ret == ESP_OK,
                                 ret == ESP_OK ? (was_running ? "stopping" : "idle") : "error",
                                 match_watch_runtime_is_running());
    return ret;
}

static esp_err_t cap_match_watch_status_execute(const char *input_json,
                                                const claw_cap_call_context_t *ctx,
                                                char *output,
                                                size_t output_size)
{
    (void)input_json;
    (void)ctx;

    cap_match_watch_write_runtime_status(output, output_size, true,
                                         match_watch_runtime_is_running() ? "running" : "idle");
    return ESP_OK;
}

static const claw_cap_descriptor_t s_match_watch_descriptors[] = {
    {
        .id = "match_watch_open",
        .name = "match_watch_open",
        .family = "match_watch",
        .description = "Open the football match watch UI on the device display. Optional team selects the displayed favorite team.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"team\":{\"type\":\"string\",\"description\":\"Favorite team name. Supported examples: Argentina/argentina/阿根廷, Brazil/brazil/巴西.\"},\"competition\":{\"type\":\"string\",\"description\":\"Competition label. Defaults to FIFA World Cup.\"},\"reminders\":{\"type\":\"boolean\"}}}",
        .execute = cap_match_watch_open_execute,
    },
    {
        .id = "match_watch_close",
        .name = "match_watch_close",
        .family = "match_watch",
        .description = "Close the football match watch UI if it is running.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_match_watch_close_execute,
    },
    {
        .id = "match_watch_status",
        .name = "match_watch_status",
        .family = "match_watch",
        .description = "Get match watch UI, reminder, favorite team, live team slug, provider data source, and notification status.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_match_watch_status_execute,
    },
    {
        .id = "match_watch_set_favorite",
        .name = "match_watch_set_favorite",
        .family = "match_watch",
        .description = "Set or clear the favorite team for match watch reminders and UI filtering.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"team\":{\"type\":\"string\",\"description\":\"Team name. Empty clears favorite. Supported examples: Argentina/argentina/阿根廷, Brazil/brazil/巴西.\"},\"competition\":{\"type\":\"string\",\"description\":\"Competition label. Defaults to FIFA World Cup.\"}}}",
        .execute = cap_match_watch_set_favorite_execute,
    },
    {
        .id = "match_watch_set_reminders",
        .name = "match_watch_set_reminders",
        .family = "match_watch",
        .description = "Enable or disable kickoff, goal, and full-time match reminders.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"enabled\":{\"type\":\"boolean\"}},\"required\":[\"enabled\"]}",
        .execute = cap_match_watch_set_reminders_execute,
    },
    {
        .id = "match_watch_push_data",
        .name = "match_watch_push_data",
        .family = "match_watch",
        .description = "Push normalized football match data from an external competition/provider skill into Match Watch UI. This switches the UI to external data source; source fetching belongs in Lua providers.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"provider\":{\"type\":\"string\"},\"competition\":{\"type\":\"string\"},\"team\":{\"type\":\"string\"},\"matches\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{\"match_no\":{\"type\":\"integer\"},\"stage\":{\"type\":\"string\",\"description\":\"group, round_of_16, quarter_final, semi_final, final\"},\"round\":{\"type\":\"integer\"},\"group\":{\"type\":\"string\"},\"home\":{\"type\":\"string\"},\"away\":{\"type\":\"string\"},\"home_code\":{\"type\":\"string\"},\"away_code\":{\"type\":\"string\"},\"home_display\":{\"type\":\"string\"},\"away_display\":{\"type\":\"string\"},\"date_label\":{\"type\":\"string\"},\"time_label\":{\"type\":\"string\"},\"beijing_label\":{\"type\":\"string\"},\"venue\":{\"type\":\"string\"},\"score\":{\"type\":\"string\"},\"kickoff_ts\":{\"type\":\"integer\"},\"live_minute\":{\"type\":\"integer\"},\"state\":{\"type\":\"string\",\"description\":\"upcoming, live, goal, half_time, full_time, finished, lost\"},\"knockout\":{\"type\":\"boolean\"}},\"required\":[\"home\",\"away\"]}},\"group_matches\":{\"type\":\"array\"},\"knockout_matches\":{\"type\":\"array\"}},\"anyOf\":[{\"required\":[\"matches\"]},{\"required\":[\"group_matches\"]},{\"required\":[\"knockout_matches\"]}]}",
        .execute = cap_match_watch_push_data_execute,
    },
    {
        .id = "match_watch_provider_start",
        .name = "match_watch_provider_start",
        .family = "match_watch",
        .description = "Start/replace Match Watch provider in one call. Required: path,args,name. Provider-specific args are validated inside the provider script. On ESP_OK/provider_started, the user request is complete. On failure, report the returned status to the user.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute provider watch.lua path, for example /fatfs/skills/match_watch_worldcup/scripts/watch.lua\"},\"args\":{\"type\":\"object\",\"description\":\"Provider arguments passed to Lua. Provider scripts validate provider-specific required fields; include open=true when the provider should open the UI after pushing data.\"},\"name\":{\"type\":\"string\",\"description\":\"Provider session name, for example match_watch_worldcup.\"}},\"required\":[\"path\",\"args\",\"name\"]}",
        .execute = cap_match_watch_provider_start_execute,
    },
    {
        .id = "match_watch_provider_stop",
        .name = "match_watch_provider_stop",
        .family = "match_watch",
        .description = "Stop only the active Match Watch provider session. It filters Lua async jobs by exclusive=match_watch_provider and does not stop unrelated Lua jobs.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{\"wait_ms\":{\"type\":\"integer\",\"minimum\":1,\"description\":\"Maximum time to wait for provider Lua job shutdown. Defaults to 4000.\"}}}",
        .execute = cap_match_watch_provider_stop_execute,
    },
    {
        .id = "match_watch_provider_status",
        .name = "match_watch_provider_status",
        .family = "match_watch",
        .description = "Get the active Match Watch provider session name, path, job id, runtime, last push time, and last error.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_match_watch_provider_status_execute,
    },
};

static const claw_cap_group_t s_match_watch_group = {
    .group_id = "cap_match_watch",
    .plugin_name = "match_watch",
    .version = "1",
    .descriptors = s_match_watch_descriptors,
    .descriptor_count = sizeof(s_match_watch_descriptors) / sizeof(s_match_watch_descriptors[0]),
};

esp_err_t cap_match_watch_register_group(void)
{
    (void)match_watch_runtime_set_notification_callback(cap_match_watch_notification_cb, NULL);
    (void)match_watch_runtime_set_push_success_callback(match_watch_runtime_provider_record_push_success, NULL);

    if (claw_cap_group_exists(s_match_watch_group.group_id)) {
        return ESP_OK;
    }

    return claw_cap_register_group(&s_match_watch_group);
}
