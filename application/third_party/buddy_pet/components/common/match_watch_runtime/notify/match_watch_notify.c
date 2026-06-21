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

static void match_watch_app_format_match_title(const match_data_schedule_item_t *match,
                                               char *buf, size_t buf_size)
{
    if (buf == NULL || buf_size == 0) {
        return;
    }
    snprintf(buf, buf_size, "%s vs %s",
             match_data_localized_team_name(match != NULL ? match->home : ""),
             match_data_localized_team_name(match != NULL ? match->away : ""));
}

bool match_watch_app_match_is_live_state(match_data_match_state_t state)
{
    return state == MATCH_DATA_MATCH_LIVE ||
           state == MATCH_DATA_MATCH_GOAL ||
           state == MATCH_DATA_MATCH_LOST ||
           state == MATCH_DATA_MATCH_HALF_TIME;
}

uint32_t match_watch_app_match_elapsed_minute(const match_data_schedule_item_t *match)
{
    return match_watch_app_live_display_minute(match);
}

void match_watch_app_check_notifications(void)
{
    static const uint32_t reminder_minutes[] = {10U, 1U};
    match_data_schedule_item_t match;
    bool waiting_for_data;
    bool has_match;
    time_t now;
    int home_score = 0;
    int away_score = 0;
    uint32_t elapsed_minute;
    uint32_t goal_total;
    bool has_score;
    bool can_notify;
    char title[80];
    char message[160];

    has_match = match_watch_app_get_home_match_state(&match, &waiting_for_data);
    if (waiting_for_data || !has_match || match.match_no == 0U) {
        return;
    }

    if (s_app->notify.match_no != match.match_no) {
        memset(&s_app->notify, 0, sizeof(s_app->notify));
        s_app->notify.match_no = match.match_no;
        if (!match_watch_app_parse_score(match.score_label,
                                         &s_app->notify.home_score,
                                         &s_app->notify.away_score)) {
            s_app->notify.home_score = 0;
            s_app->notify.away_score = 0;
        }
        s_app->notify.last_goal_total =
                (uint32_t)(s_app->notify.home_score + s_app->notify.away_score);
        s_app->notify.state = MATCH_DATA_MATCH_UPCOMING;
    }

    match_watch_app_format_match_title(&match, title, sizeof(title));
    now = time(NULL);
    elapsed_minute = match_watch_app_match_elapsed_minute(&match);
    has_score = match_watch_app_parse_score(match.score_label, &home_score, &away_score);
    goal_total = has_score ? (uint32_t)(home_score + away_score) : 0U;
    can_notify = s_app->runtime.reminders_enabled && s_app->runtime.notify_chat_id[0] != '\0';

    if (can_notify && match.state == MATCH_DATA_MATCH_UPCOMING && match.kickoff_ts != 0U && now > 0) {
        int64_t remain_s = (int64_t)match.kickoff_ts - (int64_t)now;
        for (size_t i = 0; i < MATCH_WATCH_ARRAY_SIZE(reminder_minutes); i++) {
            uint32_t minutes = reminder_minutes[i];
            uint32_t bit = 1U << i;
            if ((s_app->notify.reminder_mask & bit) == 0U &&
                    remain_s > 0 && remain_s <= (int64_t)minutes * 60) {
                s_app->notify.reminder_mask |= bit;
                snprintf(message, sizeof(message), "比赛提醒：%s 将在 %u 分钟后开赛", title, (unsigned)minutes);
                match_watch_app_emit_notification("kickoff_reminder", &match, message);
            }
        }
    }

    if (can_notify && !s_app->notify.kickoff_sent &&
            (match.state == MATCH_DATA_MATCH_LIVE || match.state == MATCH_DATA_MATCH_GOAL ||
             match.state == MATCH_DATA_MATCH_LOST || match.state == MATCH_DATA_MATCH_HALF_TIME)) {
        s_app->notify.kickoff_sent = true;
        snprintf(message, sizeof(message), "比赛开始：%s", title);
        match_watch_app_emit_notification("kickoff", &match, message);
    }

    if (can_notify && has_score) {
        bool score_increased = goal_total > s_app->notify.last_goal_total;
        bool goal_state_entered = match.state == MATCH_DATA_MATCH_GOAL &&
                                  s_app->notify.state != MATCH_DATA_MATCH_GOAL &&
                                  goal_total > 0U;

        if (score_increased || goal_state_entered) {
            if (elapsed_minute > 0U) {
                snprintf(message, sizeof(message), "进球提醒：%s 第 %u 分钟，当前比分 %d-%d",
                         title, (unsigned)elapsed_minute, home_score, away_score);
            } else {
                snprintf(message, sizeof(message), "进球提醒：%s 当前比分 %d-%d",
                         title, home_score, away_score);
            }
            match_watch_app_emit_notification("goal", &match, message);
        }
        s_app->notify.home_score = home_score;
        s_app->notify.away_score = away_score;
        s_app->notify.last_goal_total = goal_total;
    }

    if (can_notify && has_score && match_watch_app_match_is_live_state(match.state)) {
        if (!s_app->notify.half_time_sent &&
                (match.state == MATCH_DATA_MATCH_HALF_TIME ||
                 (elapsed_minute >= 45U && elapsed_minute < 60U))) {
            s_app->notify.half_time_sent = true;
            snprintf(message, sizeof(message), "半场战报：%s 半场比分 %d-%d",
                     title, home_score, away_score);
            match_watch_app_emit_notification("half_time", &match, message);
        }
    }

    if (!s_app->notify.full_time_sent && match_watch_app_match_is_finished(&match)) {
        s_app->notify.full_time_sent = true;
        s_app->notify.full_time_review_until_ms = esp_log_timestamp() + MATCH_WATCH_FULL_TIME_REVIEW_MS;
        snprintf(message, sizeof(message), "全场结束：%s 最终比分 %s",
                 title, match.score_label[0] != '\0' ? match.score_label : "-");
        if (can_notify) {
            match_watch_app_emit_notification("full_time", &match, message);
        }
        match_watch_app_render_page(s_app->runtime.active_page);
        s_app->notify.state = match.state;
        return;
    }

    if (s_app->notify.full_time_sent && !s_app->notify.match_switched_sent &&
            match_watch_app_match_is_finished(&match) &&
            !match_watch_app_user_browse_hold_active() &&
            (s_app->notify.full_time_review_until_ms == 0U ||
             esp_log_timestamp() >= s_app->notify.full_time_review_until_ms)) {
        if (match_watch_app_select_next_host_match(&match)) {
            s_app->notify.match_switched_sent = true;
            match_watch_app_format_match_title(&match, title, sizeof(title));
            snprintf(message, sizeof(message), "已切换到下一场：%s", title);
            ESP_LOGI(TAG, "%s", message);
            if (can_notify) {
                match_watch_app_emit_notification("match_switched", &match, message);
            }
            match_watch_app_render_page(s_app->runtime.active_page);
            return;
        }
        s_app->notify.match_switched_sent = true;
    }

    s_app->notify.state = match.state;
}
