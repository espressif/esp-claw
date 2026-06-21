/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "match_data.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *const TAG = "match_data";
#define MATCH_DATA_LOG_SUMMARY_MAX 3U

typedef struct {
    match_data_schedule_item_t *group_schedule;
    size_t group_count;
    match_data_schedule_item_t *knockout_schedule;
    size_t knockout_count;
    bool has_live_data;
} match_data_live_state_t;

static match_data_live_state_t s_live;

static bool match_data_is_placeholder_team(const char *team)
{
    bool has_digit = false;
    bool has_letter = false;

    if (team == NULL || team[0] == '\0') {
        return false;
    }

    for (const char *p = team; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            has_digit = true;
        } else if (*p >= 'A' && *p <= 'Z') {
            has_letter = true;
        } else if (*p != '/' && *p != ' ') {
            return false;
        }
    }

    return has_digit && has_letter;
}

static match_data_schedule_item_t *match_data_alloc_schedule(size_t count)
{
    match_data_schedule_item_t *items = heap_caps_calloc(count, sizeof(*items),
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (items == NULL) {
        items = heap_caps_calloc(count, sizeof(*items), MALLOC_CAP_8BIT);
    }
    return items;
}

static esp_err_t match_data_ensure_live_storage(void)
{
    if (s_live.group_schedule == NULL) {
        s_live.group_schedule = match_data_alloc_schedule(MATCH_DATA_LIVE_GROUP_MAX);
    }
    if (s_live.knockout_schedule == NULL) {
        s_live.knockout_schedule = match_data_alloc_schedule(MATCH_DATA_LIVE_KNOCKOUT_MAX);
    }

    if (s_live.group_schedule == NULL || s_live.knockout_schedule == NULL) {
        ESP_LOGW(TAG, "live schedule storage alloc failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

const char *match_data_stage_name(match_data_stage_t stage)
{
    switch (stage) {
    case MATCH_DATA_STAGE_GROUP:
        return "小组赛";
    case MATCH_DATA_STAGE_ROUND_OF_32:
        return "32强";
    case MATCH_DATA_STAGE_ROUND_OF_16:
        return "16强";
    case MATCH_DATA_STAGE_QUARTER_FINAL:
        return "8强";
    case MATCH_DATA_STAGE_SEMI_FINAL:
        return "4强";
    case MATCH_DATA_STAGE_THIRD_PLACE:
        return "季军赛";
    case MATCH_DATA_STAGE_FINAL:
        return "决赛";
    default:
        return "未知阶段";
    }
}

static const char *match_data_match_state_name(match_data_match_state_t state)
{
    switch (state) {
    case MATCH_DATA_MATCH_UPCOMING:
        return "upcoming";
    case MATCH_DATA_MATCH_LIVE:
        return "live";
    case MATCH_DATA_MATCH_GOAL:
        return "goal";
    case MATCH_DATA_MATCH_HALF_TIME:
        return "half_time";
    case MATCH_DATA_MATCH_FULL_TIME:
        return "full_time";
    case MATCH_DATA_MATCH_PENALTY_WIN:
        return "penalty_win";
    case MATCH_DATA_MATCH_LOST:
        return "lost";
    default:
        return "unknown";
    }
}

const char *match_data_team_code(const char *team)
{
    if (team == NULL || team[0] == '\0') {
        return "---";
    }
    if (match_data_is_placeholder_team(team)) {
        return "TBD";
    }
    return team;
}

const char *match_data_localized_team_name(const char *team)
{
    if (team == NULL || team[0] == '\0') {
        return "";
    }
    if (match_data_is_placeholder_team(team)) {
        return "待定";
    }
    return team;
}

const char *match_data_localized_venue_name(const char *venue)
{
    if (venue == NULL || venue[0] == '\0') {
        return "";
    }
    return venue;
}

static const char *match_data_item_team_label(const match_data_schedule_item_t *item, bool home_side)
{
    const char *display = home_side ? item->home_display : item->away_display;
    const char *team = home_side ? item->home : item->away;

    if (display != NULL && display[0] != '\0') {
        return display;
    }
    return match_data_localized_team_name(team);
}

const match_data_schedule_item_t *match_data_get_group_schedule(size_t *count)
{
    if (s_live.has_live_data && s_live.group_count > 0U && s_live.group_schedule != NULL) {
        if (count != NULL) {
            *count = s_live.group_count;
        }
        return s_live.group_schedule;
    }

    if (count != NULL) {
        *count = 0;
    }
    return NULL;
}

const match_data_schedule_item_t *match_data_get_knockout_schedule(size_t *count)
{
    if (s_live.has_live_data && s_live.knockout_count > 0U && s_live.knockout_schedule != NULL) {
        if (count != NULL) {
            *count = s_live.knockout_count;
        }
        return s_live.knockout_schedule;
    }

    if (count != NULL) {
        *count = 0;
    }
    return NULL;
}

static int match_data_compare_schedule_time(const void *a, const void *b)
{
    const match_data_schedule_item_t *ma = (const match_data_schedule_item_t *)a;
    const match_data_schedule_item_t *mb = (const match_data_schedule_item_t *)b;

    if (ma->kickoff_ts != mb->kickoff_ts) {
        if (ma->kickoff_ts == 0U) {
            return 1;
        }
        if (mb->kickoff_ts == 0U) {
            return -1;
        }
        return ma->kickoff_ts < mb->kickoff_ts ? -1 : 1;
    }

    if (ma->match_no == mb->match_no) {
        return 0;
    }
    return ma->match_no < mb->match_no ? -1 : 1;
}

static void match_data_format_utc8_time(uint32_t epoch, char *buf, size_t buf_size)
{
    time_t utc8;
    struct tm tm_info;

    if (buf == NULL || buf_size == 0) {
        return;
    }
    if (epoch == 0U) {
        snprintf(buf, buf_size, "--:--:--");
        return;
    }

    utc8 = (time_t)epoch + 8 * 60 * 60;
    if (gmtime_r(&utc8, &tm_info) == NULL) {
        snprintf(buf, buf_size, "--:--:--");
        return;
    }
    snprintf(buf, buf_size, "%02d:%02d:%02d", tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
}

static void match_data_log_group_schedule(void)
{
    ESP_LOGV(TAG, "saved group schedule: count=%u", (unsigned)s_live.group_count);
    for (size_t i = 0; i < s_live.group_count; i++) {
        const match_data_schedule_item_t *m = &s_live.group_schedule[i];
        char kickoff_time[9];

        match_data_format_utc8_time(m->kickoff_ts, kickoff_time, sizeof(kickoff_time));
        ESP_LOGV(TAG,
                 "group[%02u] no=%u kickoff=%s date=%s time=%s UTC+8=%s group=%s %s(%s) vs %s(%s) score=%s state=%s venue=%s",
                 (unsigned)i,
                 (unsigned)m->match_no,
                 kickoff_time,
                 m->date_label,
                 m->time_label,
                 m->beijing_label,
                 m->group,
                 m->home,
                 match_data_item_team_label(m, true),
                 m->away,
                 match_data_item_team_label(m, false),
                 m->score_label,
                 match_data_match_state_name(m->state),
                 m->venue);
    }
}

static void match_data_log_schedule_summary(const char *kind,
                                            const match_data_schedule_item_t *matches,
                                            size_t count)
{
    size_t log_count = count < MATCH_DATA_LOG_SUMMARY_MAX ? count : MATCH_DATA_LOG_SUMMARY_MAX;

    if (count == 0U || matches == NULL) {
        return;
    }

    for (size_t i = 0; i < log_count; i++) {
        const match_data_schedule_item_t *match = &matches[i];
        char kickoff_time[9];

        match_data_format_utc8_time(match->kickoff_ts, kickoff_time, sizeof(kickoff_time));
        ESP_LOGI(TAG, "%s[%u]: no=%u kickoff=%s UTC+8=%s %s(%s) vs %s(%s) state=%s",
                 kind,
                 (unsigned)i,
                 (unsigned)match->match_no,
                 kickoff_time,
                 match->beijing_label,
                 match->home,
                 match_data_item_team_label(match, true),
                 match->away,
                 match_data_item_team_label(match, false),
                 match_data_match_state_name(match->state));
    }
}

void match_data_set_live_data(const match_data_schedule_item_t *group_items, size_t group_count,
                              const match_data_schedule_item_t *knockout_items, size_t knockout_count)
{
    size_t i;

    s_live.group_count = 0;
    s_live.knockout_count = 0;
    if (match_data_ensure_live_storage() != ESP_OK) {
        s_live.has_live_data = false;
        return;
    }

    if (group_items != NULL) {
        for (i = 0; i < group_count && i < MATCH_DATA_LIVE_GROUP_MAX; i++) {
            s_live.group_schedule[i] = group_items[i];
            s_live.group_count++;
        }
    }

    if (knockout_items != NULL) {
        for (i = 0; i < knockout_count && i < MATCH_DATA_LIVE_KNOCKOUT_MAX; i++) {
            s_live.knockout_schedule[i] = knockout_items[i];
            s_live.knockout_count++;
        }
    }

    if (s_live.group_count > 1U) {
        qsort(s_live.group_schedule, s_live.group_count, sizeof(s_live.group_schedule[0]),
              match_data_compare_schedule_time);
    }
    if (s_live.knockout_count > 1U) {
        qsort(s_live.knockout_schedule, s_live.knockout_count, sizeof(s_live.knockout_schedule[0]),
              match_data_compare_schedule_time);
    }

    ESP_LOGI(TAG, "saved live schedule: group=%u knockout=%u",
             (unsigned)s_live.group_count, (unsigned)s_live.knockout_count);
    match_data_log_schedule_summary("group", s_live.group_schedule, s_live.group_count);
    match_data_log_schedule_summary("knockout", s_live.knockout_schedule, s_live.knockout_count);
    match_data_log_group_schedule();
    s_live.has_live_data = (s_live.group_count > 0U || s_live.knockout_count > 0U);
}

bool match_data_has_live_data(void)
{
    return s_live.has_live_data;
}

static bool match_data_team_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0') {
        return false;
    }

    if (strcasecmp(a, b) == 0) {
        return true;
    }
    return false;
}

bool match_data_match_has_team(const match_data_schedule_item_t *match, const char *team)
{
    return match != NULL &&
           (match_data_team_equal(match->home, team) ||
            match_data_team_equal(match->away, team) ||
            match_data_team_equal(match->home_code, team) ||
            match_data_team_equal(match->away_code, team) ||
            match_data_team_equal(match->home_display, team) ||
            match_data_team_equal(match->away_display, team));
}

bool match_data_find_team_match(const char *team, match_data_schedule_item_t *out_match)
{
    size_t group_count = 0;
    size_t knockout_count = 0;
    const match_data_schedule_item_t *group = match_data_get_group_schedule(&group_count);
    const match_data_schedule_item_t *knockout = match_data_get_knockout_schedule(&knockout_count);

    if (team == NULL || team[0] == '\0') {
        return false;
    }

    for (size_t i = 0; i < group_count; i++) {
        if (match_data_match_has_team(&group[i], team)) {
            if (out_match != NULL) {
                *out_match = group[i];
            }
            return true;
        }
    }

    for (size_t i = 0; i < knockout_count; i++) {
        if (match_data_match_has_team(&knockout[i], team)) {
            if (out_match != NULL) {
                *out_match = knockout[i];
            }
            return true;
        }
    }

    return false;
}
