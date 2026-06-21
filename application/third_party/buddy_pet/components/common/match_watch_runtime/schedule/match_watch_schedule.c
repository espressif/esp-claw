/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gfx.h"
#include "match_watch_app.h"
#include "match_watch_internal.h"
#include "match_watch_platform.h"
#include "match_watch_pet.h"
#include "match_data.h"
#include "pet_registry.h"

static const char *const TAG = "match_watch";
static const uint32_t NO_NEXT_MATCH_LOG_INTERVAL_MS = 10U * 60U * 1000U;
static uint16_t s_no_next_match_logged_no;
static uint32_t s_no_next_match_logged_ms;
static char s_no_next_match_logged_team[MATCH_DATA_TEAM_NAME_LEN];

void match_watch_app_persist_host_team(void);

size_t match_watch_app_stage_order_count(void)
{
    return MATCH_WATCH_APP_STAGE_ORDER_COUNT;
}

size_t match_watch_app_stage_order_index(match_data_stage_t stage)
{
    for (size_t i = 0; i < match_watch_app_stage_order_count(); i++) {
        if (match_watch_app_stage_order[i] == stage) {
            return i;
        }
    }
    return 0;
}

void match_watch_app_reset_home_timing(void)
{
    s_app->timing.match_no = 0;
    s_app->timing.phase = MATCH_WATCH_HOME_PHASE_FAR;
    s_app->timing.real_base_ms = 0;
    s_app->timing.value_base = 0;
    s_app->timing.live_minute = 0;
    match_watch_pet_logic_reset(&s_app->pet.logic);
}

void match_watch_app_note_user_browse(void)
{
    s_app->selection.user_browse_until_ms = esp_log_timestamp() + MATCH_WATCH_USER_BROWSE_HOLD_MS;
    match_watch_app_cancel_full_time_review_hold();
}

bool match_watch_app_user_browse_hold_active(void)
{
    uint32_t until_ms = s_app->selection.user_browse_until_ms;
    uint32_t now_ms;

    if (until_ms == 0U) {
        return false;
    }

    now_ms = esp_log_timestamp();
    if ((int32_t)(until_ms - now_ms) > 0) {
        return true;
    }

    s_app->selection.user_browse_until_ms = 0;
    return false;
}

bool match_watch_app_team_option_exists(const char *team)
{
    if (team == NULL || team[0] == '\0') {
        return true;
    }

    for (size_t i = 0; i < s_app->team_options.count; i++) {
        if (strcmp(s_app->team_options.values[i], team) == 0) {
            return true;
        }
    }
    return false;
}

void match_watch_app_clear_host_team(bool persist)
{
    s_app->selection.host_team[0] = '\0';
    s_app->selection.host_team_selected = false;
    s_app->selection.host_team_source = MATCH_WATCH_HOST_TEAM_SOURCE_NONE;
    s_app->selection.host_team_index = 0;
    match_watch_app_reset_home_timing();
    if (persist) {
        match_watch_app_persist_host_team();
    }
    match_watch_app_select_default_host_team();
}

void match_watch_app_add_team_option(const char *team)
{
    if (team == NULL || team[0] == '\0' ||
            s_app->team_options.count >= MATCH_WATCH_TEAM_OPTION_MAX ||
            match_watch_app_team_option_exists(team)) {
        return;
    }

    strncpy(s_app->team_options.values[s_app->team_options.count], team, sizeof(s_app->team_options.values[0]) - 1);
    s_app->team_options.values[s_app->team_options.count][sizeof(s_app->team_options.values[0]) - 1] = '\0';
    s_app->team_options.count++;
}

size_t match_watch_app_find_team_index(const char *team)
{
    if (team == NULL || team[0] == '\0') {
        return 0;
    }

    for (size_t i = 0; i < s_app->team_options.count; i++) {
        if (strcmp(s_app->team_options.values[i], team) == 0 ||
                strcmp(match_data_localized_team_name(s_app->team_options.values[i]), team) == 0) {
            return i;
        }
    }
    return 0;
}

static bool match_watch_app_team_label_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0') {
        return false;
    }
    if (strcmp(a, b) == 0) {
        return true;
    }
    if (strcmp(match_data_localized_team_name(a), b) == 0) {
        return true;
    }
    return strcmp(a, match_data_localized_team_name(b)) == 0;
}

const char *match_watch_app_host_team_source_name(match_watch_host_team_source_t source)
{
    switch (source) {
    case MATCH_WATCH_HOST_TEAM_SOURCE_DEFAULT:
        return "default";
    case MATCH_WATCH_HOST_TEAM_SOURCE_PET:
        return "pet";
    case MATCH_WATCH_HOST_TEAM_SOURCE_USER:
        return "user";
    case MATCH_WATCH_HOST_TEAM_SOURCE_PROVIDER:
        return "provider";
    case MATCH_WATCH_HOST_TEAM_SOURCE_NONE:
    default:
        return "none";
    }
}

match_watch_host_team_source_t match_watch_app_host_team_source(void)
{
    return s_app != NULL ? s_app->selection.host_team_source : MATCH_WATCH_HOST_TEAM_SOURCE_NONE;
}

static match_watch_host_team_source_t match_watch_app_host_team_source_from_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return MATCH_WATCH_HOST_TEAM_SOURCE_NONE;
    }
    if (strcmp(name, "default") == 0) {
        return MATCH_WATCH_HOST_TEAM_SOURCE_DEFAULT;
    }
    if (strcmp(name, "pet") == 0) {
        return MATCH_WATCH_HOST_TEAM_SOURCE_PET;
    }
    if (strcmp(name, "user") == 0) {
        return MATCH_WATCH_HOST_TEAM_SOURCE_USER;
    }
    if (strcmp(name, "provider") == 0) {
        return MATCH_WATCH_HOST_TEAM_SOURCE_PROVIDER;
    }
    return MATCH_WATCH_HOST_TEAM_SOURCE_NONE;
}

void match_watch_app_persist_host_team(void)
{
    nvs_handle_t nvs;
    esp_err_t ret;

    ret = nvs_open(MATCH_WATCH_HOST_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "open host nvs failed: %s", esp_err_to_name(ret));
        return;
    }
    if (s_app->selection.host_team_selected && s_app->selection.host_team[0] != '\0') {
        ESP_LOGI(TAG, "save host team: %s source=%s",
                 s_app->selection.host_team,
                 match_watch_app_host_team_source_name(s_app->selection.host_team_source));
        ret = nvs_set_str(nvs, MATCH_WATCH_HOST_NVS_KEY, s_app->selection.host_team);
        if (ret == ESP_OK) {
            ret = nvs_set_str(nvs,
                              MATCH_WATCH_HOST_SOURCE_NVS_KEY,
                              match_watch_app_host_team_source_name(s_app->selection.host_team_source));
        }
    } else {
        ESP_LOGI(TAG, "clear host team");
        ret = nvs_erase_key(nvs, MATCH_WATCH_HOST_NVS_KEY);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
        if (ret == ESP_OK) {
            ret = nvs_erase_key(nvs, MATCH_WATCH_HOST_SOURCE_NVS_KEY);
            if (ret == ESP_ERR_NVS_NOT_FOUND) {
                ret = ESP_OK;
            }
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "save host team failed: %s", esp_err_to_name(ret));
    }
}

void match_watch_app_load_host_team(void)
{
    nvs_handle_t nvs;
    size_t len = sizeof(s_app->selection.host_team);
    char source_name[16] = {0};
    size_t source_len = sizeof(source_name);
    esp_err_t ret;

    if (s_app->selection.host_team_loaded) {
        return;
    }
    s_app->selection.host_team_loaded = true;

    if (s_app->runtime.data_source == MATCH_WATCH_DATA_SOURCE_EXTERNAL &&
            s_app->selection.host_team_selected &&
            s_app->selection.host_team[0] != '\0') {
        ESP_LOGI(TAG, "host team kept from external source: %s", s_app->selection.host_team);
        return;
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ret = nvs_flash_erase();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "nvs erase failed: %s", esp_err_to_name(ret));
            return;
        }
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "nvs init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_open(MATCH_WATCH_HOST_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        strlcpy(s_app->selection.host_team, "Argentina", sizeof(s_app->selection.host_team));
        s_app->selection.host_team_selected = true;
        s_app->selection.host_team_source = MATCH_WATCH_HOST_TEAM_SOURCE_DEFAULT;
        ESP_LOGI(TAG, "host team defaulted: %s", s_app->selection.host_team);
        return;
    }
    ret = nvs_get_str(nvs, MATCH_WATCH_HOST_NVS_KEY, s_app->selection.host_team, &len);
    if (ret == ESP_OK && nvs_get_str(nvs, MATCH_WATCH_HOST_SOURCE_NVS_KEY, source_name, &source_len) == ESP_OK) {
        s_app->selection.host_team_source = match_watch_app_host_team_source_from_name(source_name);
    }
    nvs_close(nvs);
    if (ret == ESP_OK) {
        s_app->selection.host_team[sizeof(s_app->selection.host_team) - 1] = '\0';
        s_app->selection.host_team_selected = true;
        if (s_app->selection.host_team_source == MATCH_WATCH_HOST_TEAM_SOURCE_NONE) {
            s_app->selection.host_team_source = MATCH_WATCH_HOST_TEAM_SOURCE_USER;
        }
        ESP_LOGI(TAG, "host team restored: %s source=%s",
                 s_app->selection.host_team,
                 match_watch_app_host_team_source_name(s_app->selection.host_team_source));
    } else {
        strlcpy(s_app->selection.host_team, "Argentina", sizeof(s_app->selection.host_team));
        s_app->selection.host_team_selected = true;
        s_app->selection.host_team_source = MATCH_WATCH_HOST_TEAM_SOURCE_DEFAULT;
        ESP_LOGI(TAG, "host team defaulted: %s", s_app->selection.host_team);
    }
}

void match_watch_app_set_host_team_with_source(const char *team,
                                               bool persist,
                                               match_watch_host_team_source_t source)
{
    match_data_schedule_item_t match;
    bool same_team;

    if (team == NULL || team[0] == '\0') {
        return;
    }

    same_team = s_app->selection.host_team_selected &&
                match_watch_app_team_label_equal(s_app->selection.host_team, team);
    s_app->selection.host_team_index = match_watch_app_find_team_index(team);
    strncpy(s_app->selection.host_team, team, sizeof(s_app->selection.host_team) - 1);
    s_app->selection.host_team[sizeof(s_app->selection.host_team) - 1] = '\0';
    s_app->selection.host_team_selected = true;
    s_app->selection.host_team_source = source != MATCH_WATCH_HOST_TEAM_SOURCE_NONE ?
                                        source : MATCH_WATCH_HOST_TEAM_SOURCE_USER;
    if (same_team) {
        if (persist) {
            match_watch_app_persist_host_team();
        }
        return;
    }

    match_watch_app_reset_home_timing();
    if (persist) {
        match_watch_app_persist_host_team();
    }
    if (match_watch_app_pick_best_team_match(s_app->selection.host_team, &match)) {
        match_watch_app_select_match(&match, false);
        ESP_LOGI(TAG, "favorite team selected: team=%s current=%u %s vs %s state=%d",
                 s_app->selection.host_team, (unsigned)match.match_no,
                 match.home, match.away, (int)match.state);
    } else {
        ESP_LOGW(TAG, "favorite team selected but no preferred match found: team=%s",
                 s_app->selection.host_team);
    }
    ESP_LOGD(TAG, "favorite team label=%s",
             match_data_localized_team_name(s_app->selection.host_team));
}

void match_watch_app_set_host_team(const char *team, bool persist)
{
    match_watch_app_set_host_team_with_source(team, persist, MATCH_WATCH_HOST_TEAM_SOURCE_USER);
}

bool match_watch_app_apply_pet_profile(const pet_registry_entry_t *entry, bool persist)
{
    if (entry == NULL || entry->profile[0] == '\0') {
        return false;
    }

    match_watch_app_set_host_team_with_source(entry->profile, persist, MATCH_WATCH_HOST_TEAM_SOURCE_PET);
    ESP_LOGI(TAG, "pet profile applied: id=%s title=%s profile=%s",
             entry->id, entry->title[0] != '\0' ? entry->title : "<none>", entry->profile);
    return true;
}

bool match_watch_app_apply_selected_pet_theme(bool persist)
{
    pet_registry_entry_t entry;

    if (pet_registry_get_selected_entry(&entry) != ESP_OK) {
        return false;
    }
    return match_watch_app_apply_pet_profile(&entry, persist);
}

static const match_data_schedule_item_t *match_watch_app_pick_first_kickoff_match(void)
{
    size_t group_count = 0;
    size_t knockout_count = 0;
    const match_data_schedule_item_t *group = match_data_get_group_schedule(&group_count);
    const match_data_schedule_item_t *knockout = match_data_get_knockout_schedule(&knockout_count);
    const match_data_schedule_item_t *best = NULL;

    for (size_t pass = 0; pass < 2; pass++) {
        const match_data_schedule_item_t *items = pass == 0 ? group : knockout;
        size_t count = pass == 0 ? group_count : knockout_count;

        for (size_t i = 0; i < count; i++) {
            if (items[i].home[0] == '\0') {
                continue;
            }
            if (best == NULL) {
                best = &items[i];
                continue;
            }
            if (items[i].kickoff_ts != 0U &&
                    (best->kickoff_ts == 0U || items[i].kickoff_ts < best->kickoff_ts)) {
                best = &items[i];
            }
        }
    }

    return best;
}

void match_watch_app_select_default_host_team(void)
{
    const match_data_schedule_item_t *first_match;

    if (s_app->team_options.count == 0U || s_app->selection.host_team_selected) {
        return;
    }

    first_match = match_watch_app_pick_first_kickoff_match();
    if (first_match != NULL && first_match->home[0] != '\0') {
        match_watch_app_select_match(first_match, false);
    } else {
        s_app->selection.selected_match_no = 0;
        s_app->selection.selected_stage = MATCH_DATA_STAGE_GROUP;
    }
    ESP_LOGD(TAG, "default match selected");
}

size_t match_watch_app_schedule_count(void)
{
    size_t group_count = 0;
    size_t knockout_count = 0;
    size_t count = 0;
    const match_data_schedule_item_t *group = match_data_get_group_schedule(&group_count);
    const match_data_schedule_item_t *knockout = match_data_get_knockout_schedule(&knockout_count);

    for (size_t i = 0; i < group_count; i++) {
        if (!s_app->selection.host_team_selected || match_data_match_has_team(&group[i], s_app->selection.host_team)) {
            count++;
        }
    }
    for (size_t i = 0; i < knockout_count; i++) {
        if (!s_app->selection.host_team_selected || match_data_match_has_team(&knockout[i], s_app->selection.host_team)) {
            count++;
        }
    }
    return count;
}

size_t match_watch_app_stage_schedule_count(match_data_stage_t stage)
{
    size_t count = 0;
    size_t group_count = 0;
    size_t knockout_count = 0;
    const match_data_schedule_item_t *items = stage == MATCH_DATA_STAGE_GROUP ?
                                             match_data_get_group_schedule(&group_count) :
                                             match_data_get_knockout_schedule(&knockout_count);
    size_t total = stage == MATCH_DATA_STAGE_GROUP ? group_count : knockout_count;

    for (size_t i = 0; i < total; i++) {
        if (items != NULL && items[i].stage == stage &&
                (!s_app->selection.host_team_selected || match_data_match_has_team(&items[i], s_app->selection.host_team))) {
            count++;
        }
    }
    return count;
}

static int match_watch_app_stage_priority(match_data_stage_t stage)
{
    switch (stage) {
    case MATCH_DATA_STAGE_FINAL:
        return 6;
    case MATCH_DATA_STAGE_THIRD_PLACE:
        return 5;
    case MATCH_DATA_STAGE_SEMI_FINAL:
        return 4;
    case MATCH_DATA_STAGE_QUARTER_FINAL:
        return 3;
    case MATCH_DATA_STAGE_ROUND_OF_16:
        return 2;
    case MATCH_DATA_STAGE_ROUND_OF_32:
        return 1;
    case MATCH_DATA_STAGE_GROUP:
    default:
        return 0;
    }
}

int match_watch_app_team_match_rank(const match_data_schedule_item_t *match)
{
    if (match == NULL) {
        return 3;
    }

    switch (match->state) {
    case MATCH_DATA_MATCH_LIVE:
    case MATCH_DATA_MATCH_GOAL:
    case MATCH_DATA_MATCH_HALF_TIME:
    case MATCH_DATA_MATCH_LOST:
        return 0;
    case MATCH_DATA_MATCH_UPCOMING:
        return 1;
    case MATCH_DATA_MATCH_FULL_TIME:
    case MATCH_DATA_MATCH_PENALTY_WIN:
        return 2;
    default:
        return 3;
    }
}

bool match_watch_app_is_better_team_match(const match_data_schedule_item_t *candidate,
                                                 const match_data_schedule_item_t *best)
{
    int candidate_rank;
    int best_rank;

    if (candidate == NULL) {
        return false;
    }
    if (best == NULL || best->match_no == 0U) {
        return true;
    }

    candidate_rank = match_watch_app_team_match_rank(candidate);
    best_rank = match_watch_app_team_match_rank(best);
    if (candidate_rank != best_rank) {
        return candidate_rank < best_rank;
    }

    switch (candidate_rank) {
    case 0:
        if (candidate->kickoff_ts != best->kickoff_ts) {
            return candidate->kickoff_ts > best->kickoff_ts;
        }
        break;
    case 1:
        if (candidate->kickoff_ts != 0U && best->kickoff_ts == 0U) {
            return true;
        }
        if (candidate->kickoff_ts == 0U && best->kickoff_ts != 0U) {
            return false;
        }
        if (candidate->kickoff_ts != best->kickoff_ts) {
            return candidate->kickoff_ts < best->kickoff_ts;
        }
        if (match_watch_app_stage_priority(candidate->stage) != match_watch_app_stage_priority(best->stage)) {
            return match_watch_app_stage_priority(candidate->stage) >
                   match_watch_app_stage_priority(best->stage);
        }
        return candidate->match_no > best->match_no;
    case 2:
        if (candidate->kickoff_ts != 0U && best->kickoff_ts == 0U) {
            return true;
        }
        if (candidate->kickoff_ts == 0U && best->kickoff_ts != 0U) {
            return false;
        }
        if (candidate->kickoff_ts != best->kickoff_ts) {
            return candidate->kickoff_ts > best->kickoff_ts;
        }
        return candidate->match_no > best->match_no;
    default:
        break;
    }

    return candidate->match_no < best->match_no;
}

bool match_watch_app_pick_best_team_match(const char *team, match_data_schedule_item_t *out_match)
{
    size_t group_count = 0;
    size_t knockout_count = 0;
    const match_data_schedule_item_t *group = match_data_get_group_schedule(&group_count);
    const match_data_schedule_item_t *knockout = match_data_get_knockout_schedule(&knockout_count);
    match_data_schedule_item_t best = {0};
    bool found = false;

    if (team == NULL || team[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < group_count; i++) {
        if (!match_data_match_has_team(&group[i], team)) {
            continue;
        }
        if (!found || match_watch_app_is_better_team_match(&group[i], &best)) {
            best = group[i];
            found = true;
        }
    }

    for (size_t i = 0; i < knockout_count; i++) {
        if (!match_data_match_has_team(&knockout[i], team)) {
            continue;
        }
        if (!found || match_watch_app_is_better_team_match(&knockout[i], &best)) {
            best = knockout[i];
            found = true;
        }
    }

    if (found && out_match != NULL) {
        *out_match = best;
    }
    return found;
}

bool match_watch_app_should_switch_from_current(const match_data_schedule_item_t *current,
                                                       const match_data_schedule_item_t *best)
{
    if (best == NULL || best->match_no == 0U) {
        return false;
    }
    if (current == NULL || current->match_no == 0U) {
        return true;
    }
    if (current->match_no == best->match_no) {
        return false;
    }
    if (!match_watch_app_match_is_finished(current)) {
        return false;
    }
    if (match_watch_app_full_time_review_hold_active() &&
            current->match_no == s_app->notify.match_no) {
        return false;
    }
    if (best->state == MATCH_DATA_MATCH_LIVE ||
            best->state == MATCH_DATA_MATCH_GOAL ||
            best->state == MATCH_DATA_MATCH_HALF_TIME ||
            best->state == MATCH_DATA_MATCH_UPCOMING) {
        return true;
    }
    if (match_watch_app_match_is_finished(best) &&
            best->kickoff_ts != 0U && current->kickoff_ts != 0U &&
            best->kickoff_ts > current->kickoff_ts) {
        return true;
    }
    return false;
}

bool match_watch_app_stage_has_matches(match_data_stage_t stage)
{
    return match_watch_app_stage_schedule_count(stage) > 0U;
}

bool match_watch_app_schedule_get(size_t index, match_data_schedule_item_t *out_match)
{
    size_t group_count = 0;
    size_t knockout_count = 0;
    const match_data_schedule_item_t *group = match_data_get_group_schedule(&group_count);
    const match_data_schedule_item_t *knockout = match_data_get_knockout_schedule(&knockout_count);
    size_t seen = 0;

    for (size_t i = 0; i < group_count; i++) {
        if (s_app->selection.host_team_selected && !match_data_match_has_team(&group[i], s_app->selection.host_team)) {
            continue;
        }
        if (seen == index) {
            if (out_match != NULL) {
                *out_match = group[i];
            }
            return true;
        }
        seen++;
    }

    for (size_t i = 0; i < knockout_count; i++) {
        if (s_app->selection.host_team_selected && !match_data_match_has_team(&knockout[i], s_app->selection.host_team)) {
            continue;
        }
        if (seen == index) {
            if (out_match != NULL) {
                *out_match = knockout[i];
            }
            return true;
        }
        seen++;
    }
    return false;
}

bool match_watch_app_stage_schedule_get(match_data_stage_t stage, size_t index,
                                               match_data_schedule_item_t *out_match)
{
    size_t seen = 0;
    size_t group_count = 0;
    size_t knockout_count = 0;
    const match_data_schedule_item_t *items = stage == MATCH_DATA_STAGE_GROUP ?
                                             match_data_get_group_schedule(&group_count) :
                                             match_data_get_knockout_schedule(&knockout_count);
    size_t total = stage == MATCH_DATA_STAGE_GROUP ? group_count : knockout_count;

    for (size_t i = 0; i < total; i++) {
        if (items == NULL || items[i].stage != stage ||
                (s_app->selection.host_team_selected && !match_data_match_has_team(&items[i], s_app->selection.host_team))) {
            continue;
        }
        if (seen == index) {
            if (out_match != NULL) {
                *out_match = items[i];
            }
            return true;
        }
        seen++;
    }
    return false;
}

bool match_watch_app_find_match_index_by_no(uint16_t match_no, size_t *out_index)
{
    size_t total = match_watch_app_schedule_count();

    if (match_no == 0U) {
        return false;
    }

    for (size_t i = 0; i < total; i++) {
        match_data_schedule_item_t match;
        if (match_watch_app_schedule_get(i, &match) && match.match_no == match_no) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return true;
        }
    }
    return false;
}

bool match_watch_app_find_any_match_by_no(uint16_t match_no, match_data_schedule_item_t *out_match)
{
    size_t group_count = 0;
    size_t knockout_count = 0;
    const match_data_schedule_item_t *group = match_data_get_group_schedule(&group_count);
    const match_data_schedule_item_t *knockout = match_data_get_knockout_schedule(&knockout_count);

    if (match_no == 0U) {
        return false;
    }

    for (size_t i = 0; i < group_count; i++) {
        if (group[i].match_no == match_no) {
            if (out_match != NULL) {
                *out_match = group[i];
            }
            return true;
        }
    }
    for (size_t i = 0; i < knockout_count; i++) {
        if (knockout[i].match_no == match_no) {
            if (out_match != NULL) {
                *out_match = knockout[i];
            }
            return true;
        }
    }
    return false;
}

bool match_watch_app_find_stage_match_index_by_no(match_data_stage_t stage, uint16_t match_no,
                                                         size_t *out_index)
{
    size_t total = match_watch_app_stage_schedule_count(stage);

    if (match_no == 0U) {
        return false;
    }

    for (size_t i = 0; i < total; i++) {
        match_data_schedule_item_t match;
        if (match_watch_app_stage_schedule_get(stage, i, &match) && match.match_no == match_no) {
            if (out_index != NULL) {
                *out_index = i;
            }
            return true;
        }
    }
    return false;
}

bool match_watch_app_get_current_match(match_data_schedule_item_t *out_match)
{
    size_t index;
    match_data_schedule_item_t current;
    match_data_schedule_item_t best;
    bool hold_active;

    if (match_watch_app_find_stage_match_index_by_no(s_app->selection.selected_stage, s_app->selection.selected_match_no, &index)) {
        if (match_watch_app_stage_schedule_get(s_app->selection.selected_stage, index, out_match)) {
            return true;
        }
        return false;
    }
    hold_active = match_watch_app_user_browse_hold_active();
    if (match_watch_app_find_any_match_by_no(s_app->selection.selected_match_no, &current)) {
        s_app->selection.selected_stage = current.stage;
        if (out_match != NULL) {
            *out_match = current;
        }
        return true;
    }
    if (!hold_active && !match_watch_app_full_time_review_hold_active() &&
            s_app->selection.host_team_selected &&
            match_watch_app_pick_best_team_match(s_app->selection.host_team, &best)) {
        if (s_app->selection.selected_match_no != best.match_no) {
            match_watch_app_select_match(&best, false);
        }
        if (out_match != NULL) {
            *out_match = best;
        }
        return true;
    }
    if (hold_active) {
        if (match_watch_app_stage_schedule_get(s_app->selection.selected_stage, 0, out_match)) {
            return true;
        }
        if (match_watch_app_find_match_index_by_no(s_app->selection.selected_match_no, &index)) {
            return match_watch_app_schedule_get(index, out_match);
        }
        return false;
    }
    if (match_watch_app_stage_schedule_get(s_app->selection.selected_stage, 0, out_match)) {
        return true;
    }
    if (match_watch_app_find_match_index_by_no(s_app->selection.selected_match_no, &index)) {
        return match_watch_app_schedule_get(index, out_match);
    }
    return false;
}

bool match_watch_app_get_home_match_state(match_data_schedule_item_t *out_match, bool *waiting_for_data)
{
    bool has_data = match_data_has_live_data();

    if (waiting_for_data != NULL) {
        *waiting_for_data = !has_data;
    }
    if (!has_data) {
        return false;
    }
    return match_watch_app_get_current_match(out_match);
}

bool match_watch_app_find_current_match_index(size_t *out_index)
{
    match_data_schedule_item_t match;

    if (match_watch_app_find_stage_match_index_by_no(s_app->selection.selected_stage,
            s_app->selection.selected_match_no, out_index)) {
        return true;
    }
    if (s_app->selection.selected_match_no != 0U &&
            match_watch_app_find_any_match_by_no(s_app->selection.selected_match_no, &match)) {
        s_app->selection.selected_stage = match.stage;
        if (match_watch_app_find_stage_match_index_by_no(s_app->selection.selected_stage,
                s_app->selection.selected_match_no, out_index)) {
            return true;
        }
    }
    return false;
}

void match_watch_app_select_match(const match_data_schedule_item_t *match, bool persist)
{
    if (match == NULL) {
        return;
    }

    if (persist) {
        match_watch_app_cancel_full_time_review_hold();
    } else if (match->match_no != s_app->selection.selected_match_no &&
            match_watch_app_full_time_review_hold_active()) {
        match_watch_app_cancel_full_time_review_hold();
    }

    s_app->selection.selected_match_no = match->match_no;
    s_app->selection.selected_stage = match->stage;
    match_watch_app_reset_home_timing();
}

bool match_watch_app_select_match_by_offset(int offset)
{
    size_t total = match_watch_app_stage_schedule_count(s_app->selection.selected_stage);
    size_t current = 0;
    size_t next;
    match_data_schedule_item_t match;

    if (total <= 1U || offset == 0) {
        return false;
    }

    if (!match_watch_app_find_current_match_index(&current)) {
        return false;
    }

    if (offset < 0) {
        size_t back = (size_t)(-offset) % total;
        next = (current + total - back) % total;
    } else {
        next = (current + (size_t)offset) % total;
    }
    if (next == current) {
        return false;
    }

    if (!match_watch_app_stage_schedule_get(s_app->selection.selected_stage, next, &match)) {
        return false;
    }

    match_watch_app_select_match(&match, true);
    return true;
}

bool match_watch_app_select_next_host_match(match_data_schedule_item_t *out_match)
{
    size_t total;
    size_t current = 0;
    match_data_schedule_item_t match;

    total = match_watch_app_schedule_count();
    if (total == 0U) {
        return false;
    }

    if (!match_watch_app_find_match_index_by_no(s_app->selection.selected_match_no, &current)) {
        current = 0;
    }

    for (size_t i = current + 1; i < total; i++) {
        if (match_watch_app_schedule_get(i, &match) && !match_watch_app_match_is_finished(&match)) {
            match_watch_app_select_match(&match, true);
            if (out_match != NULL) {
                *out_match = match;
            }
            ESP_LOGI(TAG, "auto switched to next match: team=%s no=%u %s vs %s",
                     s_app->selection.host_team_selected ? s_app->selection.host_team : "<none>",
                     (unsigned)match.match_no, match.home, match.away);
            return true;
        }
    }

    const char *team = s_app->selection.host_team_selected ? s_app->selection.host_team : "<none>";
    uint32_t now_ms = esp_log_timestamp();
    bool new_match = s_app->selection.selected_match_no != s_no_next_match_logged_no ||
                     strcmp(team, s_no_next_match_logged_team) != 0;

    if (new_match || now_ms - s_no_next_match_logged_ms >= NO_NEXT_MATCH_LOG_INTERVAL_MS) {
        ESP_LOGI(TAG, "no next match: team=%s current=%u",
                 team, (unsigned)s_app->selection.selected_match_no);
        s_no_next_match_logged_no = s_app->selection.selected_match_no;
        s_no_next_match_logged_ms = now_ms;
        strlcpy(s_no_next_match_logged_team, team, sizeof(s_no_next_match_logged_team));
    }
    return false;
}

bool match_watch_app_select_best_match_if_user_idle(void)
{
    match_data_schedule_item_t current;
    match_data_schedule_item_t best;
    bool has_current;

    if (match_watch_app_user_browse_hold_active()) {
        return false;
    }
    if (!s_app->selection.host_team_selected ||
            !match_watch_app_pick_best_team_match(s_app->selection.host_team, &best)) {
        return false;
    }

    has_current = match_watch_app_get_current_match(&current);
    if (has_current && match_watch_app_full_time_review_hold_active() &&
            match_watch_app_match_is_finished(&current) &&
            current.match_no == s_app->notify.match_no) {
        return false;
    }
    if (has_current && current.match_no == best.match_no && current.stage == best.stage) {
        return false;
    }

    match_watch_app_select_match(&best, false);
    ESP_LOGI(TAG, "user idle, switched to best match: team=%s no=%u %s vs %s state=%d",
             s_app->selection.host_team,
             (unsigned)best.match_no,
             best.home,
             best.away,
             (int)best.state);
    return true;
}

bool match_watch_app_select_stage_by_offset(int offset)
{
    size_t stage_count = match_watch_app_stage_order_count();
    size_t current = match_watch_app_stage_order_index(s_app->selection.selected_stage);
    match_data_schedule_item_t match;

    if (!match_data_has_live_data() || offset == 0) {
        return false;
    }

    if (offset < 0) {
        for (size_t next = current; next > 0; next--) {
            size_t index = next - 1;
            if (!match_watch_app_stage_has_matches(match_watch_app_stage_order[index])) {
                continue;
            }
            s_app->selection.selected_stage = match_watch_app_stage_order[index];
            if (match_watch_app_stage_schedule_get(s_app->selection.selected_stage, 0, &match)) {
                match_watch_app_select_match(&match, true);
            }
            ESP_LOGD(TAG, "stage switched: %s", match_data_stage_name(s_app->selection.selected_stage));
            return true;
        }
    } else {
        for (size_t next = current + 1; next < stage_count; next++) {
            if (!match_watch_app_stage_has_matches(match_watch_app_stage_order[next])) {
                continue;
            }
            s_app->selection.selected_stage = match_watch_app_stage_order[next];
            if (match_watch_app_stage_schedule_get(s_app->selection.selected_stage, 0, &match)) {
                match_watch_app_select_match(&match, true);
            }
            ESP_LOGD(TAG, "stage switched: %s", match_data_stage_name(s_app->selection.selected_stage));
            return true;
        }
    }
    return false;
}

bool match_watch_app_can_select_stage_by_offset(int offset)
{
    size_t stage_count = match_watch_app_stage_order_count();
    size_t current = match_watch_app_stage_order_index(s_app->selection.selected_stage);

    if (!match_data_has_live_data() || offset == 0) {
        return false;
    }

    if (offset < 0) {
        for (size_t next = current; next > 0; next--) {
            if (match_watch_app_stage_has_matches(match_watch_app_stage_order[next - 1])) {
                return true;
            }
        }
    } else {
        for (size_t next = current + 1; next < stage_count; next++) {
            if (match_watch_app_stage_has_matches(match_watch_app_stage_order[next])) {
                return true;
            }
        }
    }
    return false;
}

void match_watch_app_refresh_team_options(bool keep_current_host)
{
    size_t group_count = 0;
    size_t knockout_count = 0;
    const match_data_schedule_item_t *group = match_data_get_group_schedule(&group_count);
    const match_data_schedule_item_t *knockout = match_data_get_knockout_schedule(&knockout_count);
    char current[MATCH_DATA_TEAM_NAME_LEN];

    strncpy(current, s_app->selection.host_team, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    s_app->team_options.count = 0;
    for (size_t i = 0; i < group_count && s_app->team_options.count < MATCH_WATCH_TEAM_OPTION_MAX; i++) {
        match_watch_app_add_team_option(group[i].home);
        match_watch_app_add_team_option(group[i].away);
    }
    for (size_t i = 0; i < knockout_count && s_app->team_options.count < MATCH_WATCH_TEAM_OPTION_MAX; i++) {
        match_watch_app_add_team_option(knockout[i].home);
        match_watch_app_add_team_option(knockout[i].away);
    }

    if (s_app->team_options.count > 0U) {
        if (keep_current_host && current[0] != '\0') {
            bool found = false;
            for (size_t i = 0; i < s_app->team_options.count; i++) {
                if (strcmp(s_app->team_options.values[i], current) == 0) {
                    s_app->selection.host_team_index = i;
                    strncpy(s_app->selection.host_team, s_app->team_options.values[i], sizeof(s_app->selection.host_team) - 1);
                    s_app->selection.host_team[sizeof(s_app->selection.host_team) - 1] = '\0';
                    found = true;
                    break;
                }
            }
            if (!found) {
                match_watch_app_clear_host_team(false);
            }
        } else if (s_app->selection.host_team_selected) {
            s_app->selection.host_team_index = match_watch_app_find_team_index(s_app->selection.host_team);
        } else if (s_app->selection.selected_match_no == 0U) {
            match_watch_app_select_default_host_team();
        } else {
            s_app->selection.host_team_index = 0;
        }
    } else {
        s_app->selection.host_team_index = 0;
        if (!s_app->selection.host_team_selected && s_app->selection.selected_match_no == 0U) {
            ESP_LOGV(TAG, "default host team pending: waiting for match data");
        }
    }

    ESP_LOGV(TAG, "team options refreshed: count=%u live=%d host=%s",
             (unsigned)s_app->team_options.count, match_data_has_live_data(),
             s_app->selection.host_team[0] != '\0' ? s_app->selection.host_team : "<none>");
}


bool match_watch_app_match_is_finished(const match_data_schedule_item_t *match)
{
    return match != NULL &&
           (match->state == MATCH_DATA_MATCH_FULL_TIME ||
            match->state == MATCH_DATA_MATCH_PENALTY_WIN);
}

bool match_watch_app_full_time_review_hold_active(void)
{
    if (!s_app->notify.full_time_sent || s_app->notify.full_time_review_until_ms == 0U) {
        return false;
    }
    return esp_log_timestamp() < s_app->notify.full_time_review_until_ms;
}

void match_watch_app_cancel_full_time_review_hold(void)
{
    s_app->notify.full_time_review_until_ms = 0U;
}

void match_watch_app_sync_full_time_review(const match_data_schedule_item_t *match)
{
    if (match == NULL || match->match_no == 0U || !match_watch_app_match_is_finished(match)) {
        return;
    }
    if (match_watch_app_user_browse_hold_active()) {
        return;
    }
    if (s_app->selection.selected_match_no != 0U &&
            match->match_no != s_app->selection.selected_match_no) {
        return;
    }

    if (s_app->notify.match_no != match->match_no) {
        s_app->notify.match_no = match->match_no;
        s_app->notify.full_time_sent = false;
        s_app->notify.match_switched_sent = false;
    }
    if (s_app->notify.full_time_sent) {
        return;
    }

    s_app->notify.full_time_sent = true;
    s_app->notify.full_time_review_until_ms = esp_log_timestamp() + MATCH_WATCH_FULL_TIME_REVIEW_MS;
    ESP_LOGI(TAG, "full time review hold: match=%u hold_ms=%u",
             (unsigned)match->match_no, (unsigned)MATCH_WATCH_FULL_TIME_REVIEW_MS);
}
