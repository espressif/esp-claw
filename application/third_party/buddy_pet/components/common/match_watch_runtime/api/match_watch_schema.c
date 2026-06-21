/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "match_watch_runtime.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "match_watch_app.h"
#include "cJSON.h"
#include "data/match_data.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "match_watch_schema";

void match_watch_runtime_notify_push_success(void);

static int match_watch_schema_json_int(const cJSON *root, const char *key, int default_value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    return cJSON_IsNumber(item) ? item->valueint : default_value;
}

static uint16_t match_watch_schema_normalize_live_minute(int value)
{
    if (value <= 0) {
        return 0;
    }
    if (value > 180 && value <= 120 * 60) {
        value = (value / 60) + 1;
    }
    if (value > 180) {
        return 0;
    }
    return (uint16_t)value;
}

static const char *match_watch_schema_item_string(const cJSON *root, const char *key)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);

    return cJSON_IsString(item) && item->valuestring != NULL ? item->valuestring : NULL;
}

static bool match_watch_schema_stage_is_knockout(match_data_stage_t stage)
{
    return stage != MATCH_DATA_STAGE_GROUP;
}

static match_data_stage_t match_watch_schema_parse_stage(const char *stage)
{
    if (stage == NULL || stage[0] == '\0' ||
            strcasecmp(stage, "group") == 0 ||
            strcasecmp(stage, "group_stage") == 0 ||
            strcmp(stage, "小组赛") == 0) {
        return MATCH_DATA_STAGE_GROUP;
    }
    if (strcasecmp(stage, "round_of_32") == 0 ||
            strcasecmp(stage, "last_32") == 0 ||
            strcmp(stage, "32强") == 0) {
        return MATCH_DATA_STAGE_ROUND_OF_32;
    }
    if (strcasecmp(stage, "round_of_16") == 0 ||
            strcasecmp(stage, "last_16") == 0 ||
            strcmp(stage, "16强") == 0) {
        return MATCH_DATA_STAGE_ROUND_OF_16;
    }
    if (strcasecmp(stage, "quarter_final") == 0 ||
            strcasecmp(stage, "quarterfinal") == 0 ||
            strcasecmp(stage, "quarters") == 0 ||
            strcmp(stage, "8强") == 0 ||
            strcmp(stage, "四分之一决赛") == 0) {
        return MATCH_DATA_STAGE_QUARTER_FINAL;
    }
    if (strcasecmp(stage, "semi_final") == 0 ||
            strcasecmp(stage, "semifinal") == 0 ||
            strcasecmp(stage, "semis") == 0 ||
            strcmp(stage, "4强") == 0 ||
            strcmp(stage, "半决赛") == 0) {
        return MATCH_DATA_STAGE_SEMI_FINAL;
    }
    if (strcasecmp(stage, "third_place") == 0 ||
            strcmp(stage, "季军赛") == 0) {
        return MATCH_DATA_STAGE_THIRD_PLACE;
    }
    if (strcasecmp(stage, "final") == 0 ||
            strcmp(stage, "决赛") == 0) {
        return MATCH_DATA_STAGE_FINAL;
    }
    return MATCH_DATA_STAGE_GROUP;
}

static match_data_match_state_t match_watch_schema_parse_state(const char *state)
{
    if (state == NULL || state[0] == '\0' ||
            strcasecmp(state, "upcoming") == 0 ||
            strcasecmp(state, "scheduled") == 0 ||
            strcasecmp(state, "not_started") == 0 ||
            strcmp(state, "未开赛") == 0) {
        return MATCH_DATA_MATCH_UPCOMING;
    }
    if (strcasecmp(state, "live") == 0 ||
            strcasecmp(state, "in_play") == 0 ||
            strcasecmp(state, "playing") == 0 ||
            strcmp(state, "比赛中") == 0) {
        return MATCH_DATA_MATCH_LIVE;
    }
    if (strcasecmp(state, "goal") == 0 ||
            strcmp(state, "进球") == 0) {
        return MATCH_DATA_MATCH_GOAL;
    }
    if (strcasecmp(state, "half_time") == 0 ||
            strcasecmp(state, "ht") == 0 ||
            strcmp(state, "半场") == 0) {
        return MATCH_DATA_MATCH_HALF_TIME;
    }
    if (strcasecmp(state, "full_time") == 0 ||
            strcasecmp(state, "finished") == 0 ||
            strcasecmp(state, "finish") == 0 ||
            strcasecmp(state, "ft") == 0 ||
            strcasecmp(state, "ended") == 0 ||
            strcasecmp(state, "complete") == 0 ||
            strcasecmp(state, "completed") == 0 ||
            strcmp(state, "已结束") == 0 ||
            strcmp(state, "完场") == 0) {
        return MATCH_DATA_MATCH_FULL_TIME;
    }
    if (strcasecmp(state, "penalty_win") == 0 ||
            strcasecmp(state, "penalties") == 0 ||
            strcmp(state, "点球") == 0) {
        return MATCH_DATA_MATCH_PENALTY_WIN;
    }
    if (strcasecmp(state, "lost") == 0 ||
            strcasecmp(state, "eliminated") == 0 ||
            strcmp(state, "淘汰") == 0) {
        return MATCH_DATA_MATCH_LOST;
    }
    return MATCH_DATA_MATCH_UPCOMING;
}

static void match_watch_schema_copy_item_string(char *dst, size_t dst_size,
                                             const cJSON *root, const char *key)
{
    const char *value = match_watch_schema_item_string(root, key);

    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (value != NULL) {
        strlcpy(dst, value, dst_size);
    }
}

static bool match_watch_schema_parse_match_item(const cJSON *item,
                                             match_data_schedule_item_t *out_match,
                                             match_data_stage_t default_stage,
                                             uint16_t fallback_no)
{
    const char *stage;
    const char *state;

    if (!cJSON_IsObject(item) || out_match == NULL) {
        return false;
    }

    memset(out_match, 0, sizeof(*out_match));
    stage = match_watch_schema_item_string(item, "stage");
    state = match_watch_schema_item_string(item, "state");
    out_match->stage = stage != NULL ? match_watch_schema_parse_stage(stage) : default_stage;
    out_match->round = (uint8_t)match_watch_schema_json_int(item, "round", 0);
    out_match->match_no = (uint16_t)match_watch_schema_json_int(item, "match_no", fallback_no);
    if (out_match->match_no == 0U) {
        out_match->match_no = fallback_no;
    }
    match_watch_schema_copy_item_string(out_match->group, sizeof(out_match->group), item, "group");
    match_watch_schema_copy_item_string(out_match->home, sizeof(out_match->home), item, "home");
    match_watch_schema_copy_item_string(out_match->away, sizeof(out_match->away), item, "away");
    match_watch_schema_copy_item_string(out_match->home_code, sizeof(out_match->home_code), item, "home_code");
    match_watch_schema_copy_item_string(out_match->away_code, sizeof(out_match->away_code), item, "away_code");
    match_watch_schema_copy_item_string(out_match->home_display, sizeof(out_match->home_display), item, "home_display");
    match_watch_schema_copy_item_string(out_match->away_display, sizeof(out_match->away_display), item, "away_display");
    match_watch_schema_copy_item_string(out_match->date_label, sizeof(out_match->date_label), item, "date_label");
    match_watch_schema_copy_item_string(out_match->time_label, sizeof(out_match->time_label), item, "time_label");
    match_watch_schema_copy_item_string(out_match->beijing_label, sizeof(out_match->beijing_label), item, "beijing_label");
    match_watch_schema_copy_item_string(out_match->venue, sizeof(out_match->venue), item, "venue");
    match_watch_schema_copy_item_string(out_match->score_label, sizeof(out_match->score_label), item, "score");
    if (out_match->score_label[0] == '\0') {
        match_watch_schema_copy_item_string(out_match->score_label, sizeof(out_match->score_label), item, "score_label");
    }
    out_match->kickoff_ts = (uint32_t)match_watch_schema_json_int(item, "kickoff_ts", 0);
    out_match->live_minute = match_watch_schema_normalize_live_minute(
            match_watch_schema_json_int(item, "live_minute", 0));
    out_match->state = match_watch_schema_parse_state(state);

    return out_match->home[0] != '\0' && out_match->away[0] != '\0';
}

static void match_watch_schema_parse_match_array(const cJSON *array,
                                              match_data_stage_t default_stage,
                                              match_data_schedule_item_t *group_items,
                                              size_t *group_count,
                                              match_data_schedule_item_t *knockout_items,
                                              size_t *knockout_count,
                                              uint16_t *fallback_no)
{
    cJSON *entry;

    if (!cJSON_IsArray(array)) {
        return;
    }

    cJSON_ArrayForEach(entry, array) {
        match_data_schedule_item_t match;
        bool force_knockout = false;
        cJSON *knockout_item = cJSON_GetObjectItemCaseSensitive(entry, "knockout");

        if (fallback_no != NULL) {
            (*fallback_no)++;
        }
        if (!match_watch_schema_parse_match_item(entry, &match, default_stage,
                                              fallback_no != NULL ? *fallback_no : 1U)) {
            continue;
        }
        if (cJSON_IsBool(knockout_item)) {
            force_knockout = cJSON_IsTrue(knockout_item);
        }
        if (force_knockout || match_watch_schema_stage_is_knockout(match.stage)) {
            if (knockout_items != NULL && knockout_count != NULL &&
                    *knockout_count < MATCH_DATA_LIVE_KNOCKOUT_MAX) {
                knockout_items[*knockout_count] = match;
                (*knockout_count)++;
            }
        } else {
            if (group_items != NULL && group_count != NULL &&
                    *group_count < MATCH_DATA_LIVE_GROUP_MAX) {
                group_items[*group_count] = match;
                (*group_count)++;
            }
        }
    }
}

esp_err_t match_watch_runtime_push_data_json(const char *input_json)
{
    cJSON *root = input_json != NULL ? cJSON_Parse(input_json) : NULL;
    cJSON *schema_version;
    cJSON *matches;
    cJSON *group_matches;
    cJSON *knockout_matches;
    match_data_schedule_item_t *group_items = NULL;
    match_data_schedule_item_t *knockout_items = NULL;
    size_t group_count = 0;
    size_t knockout_count = 0;
    uint16_t fallback_no = 0;
    char team[MATCH_DATA_TEAM_NAME_LEN] = {0};
    char competition[48] = {0};
    const char *team_value;
    const char *competition_value;
    const char *provider_value;
    const match_data_schedule_item_t *first_match;
    esp_err_t ret = ESP_OK;

    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "external match data invalid: bytes=%u",
                 input_json != NULL ? (unsigned)strlen(input_json) : 0U);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    schema_version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (schema_version != NULL &&
            (!cJSON_IsNumber(schema_version) || schema_version->valueint != 1)) {
        ESP_LOGW(TAG, "external match data unsupported schema_version");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    group_items = heap_caps_calloc(MATCH_DATA_LIVE_GROUP_MAX, sizeof(*group_items),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    knockout_items = heap_caps_calloc(MATCH_DATA_LIVE_KNOCKOUT_MAX, sizeof(*knockout_items),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (group_items == NULL) {
        group_items = heap_caps_calloc(MATCH_DATA_LIVE_GROUP_MAX, sizeof(*group_items), MALLOC_CAP_8BIT);
    }
    if (knockout_items == NULL) {
        knockout_items = heap_caps_calloc(MATCH_DATA_LIVE_KNOCKOUT_MAX, sizeof(*knockout_items), MALLOC_CAP_8BIT);
    }
    if (group_items == NULL || knockout_items == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    matches = cJSON_GetObjectItemCaseSensitive(root, "matches");
    group_matches = cJSON_GetObjectItemCaseSensitive(root, "group_matches");
    knockout_matches = cJSON_GetObjectItemCaseSensitive(root, "knockout_matches");
    provider_value = match_watch_schema_item_string(root, "provider");
    ESP_LOGD(TAG, "received: bytes=%u provider=%s matches=%d group=%d knockout=%d",
             input_json != NULL ? (unsigned)strlen(input_json) : 0U,
             provider_value != NULL && provider_value[0] != '\0' ? provider_value : "<none>",
             cJSON_IsArray(matches) ? cJSON_GetArraySize(matches) : 0,
             cJSON_IsArray(group_matches) ? cJSON_GetArraySize(group_matches) : 0,
             cJSON_IsArray(knockout_matches) ? cJSON_GetArraySize(knockout_matches) : 0);
    match_watch_schema_parse_match_array(group_matches, MATCH_DATA_STAGE_GROUP,
                                      group_items, &group_count,
                                      knockout_items, &knockout_count,
                                      &fallback_no);
    match_watch_schema_parse_match_array(knockout_matches, MATCH_DATA_STAGE_ROUND_OF_16,
                                      group_items, &group_count,
                                      knockout_items, &knockout_count,
                                      &fallback_no);
    match_watch_schema_parse_match_array(matches, MATCH_DATA_STAGE_GROUP,
                                      group_items, &group_count,
                                      knockout_items, &knockout_count,
                                      &fallback_no);
    if (group_count == 0U && knockout_count == 0U) {
        ESP_LOGW(TAG, "external match data ignored: no valid matches provider=%s",
                 provider_value != NULL && provider_value[0] != '\0' ? provider_value : "<none>");
        ret = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    first_match = group_count > 0U ? &group_items[0] : &knockout_items[0];
    ESP_LOGD(TAG,
             "parsed: group=%u knockout=%u first=%u %s(%s) vs %s(%s) state=%d score=%s minute=%u",
             (unsigned)group_count,
             (unsigned)knockout_count,
             (unsigned)first_match->match_no,
             first_match->home,
             first_match->home_display[0] != '\0' ? first_match->home_display : first_match->home,
             first_match->away,
             first_match->away_display[0] != '\0' ? first_match->away_display : first_match->away,
             (int)first_match->state,
             first_match->score_label,
             (unsigned)first_match->live_minute);

    (void)match_watch_app_set_data_source(MATCH_WATCH_DATA_SOURCE_EXTERNAL);
    match_data_set_live_data(group_items, group_count, knockout_items, knockout_count);

    team_value = match_watch_schema_item_string(root, "team");
    if (team_value != NULL && team_value[0] != '\0') {
        strlcpy(team, team_value, sizeof(team));
    }
    competition_value = match_watch_schema_item_string(root, "competition");
    if (competition_value != NULL && competition_value[0] != '\0') {
        strlcpy(competition, competition_value, sizeof(competition));
        (void)match_watch_runtime_set_competition(competition);
    }

    ret = match_watch_app_request_data_changed();
    if (ret == ESP_OK) {
        match_watch_runtime_notify_push_success();
    }
    ESP_LOGD(TAG, "pushed: group=%u knockout=%u team=%s competition=%s",
             (unsigned)group_count, (unsigned)knockout_count,
             team[0] != '\0' ? team : "<unchanged>",
             competition[0] != '\0' ? competition : "<unchanged>");

cleanup:
    free(group_items);
    free(knockout_items);
    cJSON_Delete(root);
    return ret;
}
