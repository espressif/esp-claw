/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "match_watch_pet.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "pet_buddy.h"
#include "match_watch_app.h"
#include "core/match_watch_module.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_gfx_runtime.h"
#include "scene_policy.h"

#define MATCH_WATCH_PET_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define MATCH_WATCH_PET_BOTTOM_MARGIN     2
static const char *TAG = "buddy_pet";

static const scene_pet_policy_t s_match_watch_pet_scene_policy = {
    .idle_action = MATCH_WATCH_PET_ACTION_IDLE,
    .waiting_action = MATCH_WATCH_PET_ACTION_SLEEP,
    .active_action = MATCH_WATCH_PET_ACTION_RUNNING,
    .positive_action = MATCH_WATCH_PET_ACTION_JUMP,
    .negative_action = MATCH_WATCH_PET_ACTION_LOSE,
    .neutral_action = MATCH_WATCH_PET_ACTION_WAVE,
};

static uint32_t match_watch_pet_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static bool match_watch_pet_parse_score(const char *score, int *home_score, int *away_score)
{
    int home;
    int away;

    if (score == NULL || sscanf(score, "%d-%d", &home, &away) != 2) {
        return false;
    }

    if (home_score != NULL) {
        *home_score = home;
    }
    if (away_score != NULL) {
        *away_score = away;
    }
    return true;
}

static bool match_watch_pet_parse_result_score(const char *score, int *home_score, int *away_score)
{
    int home;
    int away;
    int penalty_home;
    int penalty_away;

    if (score == NULL || sscanf(score, "%d-%d 点%d-%d", &home, &away, &penalty_home, &penalty_away) != 4) {
        return match_watch_pet_parse_score(score, home_score, away_score);
    }

    if (home_score != NULL) {
        *home_score = penalty_home;
    }
    if (away_score != NULL) {
        *away_score = penalty_away;
    }
    return true;
}

static bool match_watch_pet_match_is_finished(const match_data_schedule_item_t *match)
{
    return match != NULL &&
           (match->state == MATCH_DATA_MATCH_FULL_TIME ||
            match->state == MATCH_DATA_MATCH_PENALTY_WIN);
}

static bool match_watch_pet_team_equal(const char *a, const char *b)
{
    if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0') {
        return false;
    }
    return strcasecmp(a, b) == 0;
}

static bool match_watch_pet_match_side_is_team(const match_data_schedule_item_t *match,
                                               const char *team, bool home_side)
{
    if (match == NULL || team == NULL || team[0] == '\0') {
        return false;
    }
    return match_watch_pet_team_equal(team, home_side ? match->home : match->away) ||
           match_watch_pet_team_equal(team, home_side ? match->home_code : match->away_code) ||
           match_watch_pet_team_equal(team, home_side ? match->home_display : match->away_display);
}

static bool match_watch_pet_match_team_side(const match_data_schedule_item_t *match,
                                            const char *team, bool *is_home)
{
    if (match_watch_pet_match_side_is_team(match, team, true)) {
        if (is_home != NULL) {
            *is_home = true;
        }
        return true;
    }
    if (match_watch_pet_match_side_is_team(match, team, false)) {
        if (is_home != NULL) {
            *is_home = false;
        }
        return true;
    }
    return false;
}

static const char *match_watch_pet_scene_action(scene_event_kind_t kind,
                                                const match_data_schedule_item_t *match,
                                                const char *host_team)
{
    const scene_event_t event = {
        .kind = kind,
        .scene = "match_watch",
        .subject = host_team,
        .source_id = match != NULL ? match->match_no : 0U,
    };

    return scene_policy_pick_pet_action(&event, &s_match_watch_pet_scene_policy);
}

static bool match_watch_pet_apply_profile(const pet_registry_entry_t *entry, const char *event)
{
    if (entry != NULL && entry->profile[0] != '\0') {
        ESP_LOGI(TAG, "pet %s: id=%s profile=%s", event, entry->id, entry->profile);
        return match_watch_app_apply_pet_profile(entry, true);
    }
    ESP_LOGI(TAG, "pet %s: selected pet has no profile", event);
    return false;
}

static void match_watch_pet_on_mount(const pet_registry_entry_t *entry, void *user_data)
{
    (void)user_data;

    if (match_watch_app_host_team_source() == MATCH_WATCH_HOST_TEAM_SOURCE_USER) {
        ESP_LOGI(TAG, "pet mount: keep user favorite team");
        return;
    }
    (void)match_watch_pet_apply_profile(entry, "mount");
}

static void match_watch_pet_on_unmount(void *user_data)
{
    (void)user_data;

    ESP_LOGI(TAG, "pet unmount");
}

static void match_watch_pet_on_pet_changed(const pet_registry_entry_t *entry, void *user_data)
{
    esp_err_t ret;

    (void)user_data;

    (void)match_watch_pet_apply_profile(entry, "changed");

    ret = match_watch_app_request_pet_reload();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "request pet reload failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t match_watch_pet_open(match_watch_pet_t *pet, gfx_disp_t *disp)
{
    const pet_buddy_scene_host_config_t config = {
        .display = disp,
        .owner = "match_watch",
        .hooks = &(const pet_buddy_scene_hooks_t) {
            .on_mount = match_watch_pet_on_mount,
            .on_unmount = match_watch_pet_on_unmount,
            .on_pet_changed = match_watch_pet_on_pet_changed,
        },
    };

    return pet_buddy_attach_scene_pet(pet, &config);
}

void match_watch_pet_close(match_watch_pet_t *pet)
{
    pet_buddy_detach_scene_pet(pet);
}

esp_err_t match_watch_pet_set_action(match_watch_pet_t *pet, const char *action, bool keep_pos)
{
    return pet_host_set_action(pet, action, keep_pos);
}

void match_watch_pet_set_visible(match_watch_pet_t *pet, bool visible)
{
    pet_host_set_visible(pet, visible);
}

void match_watch_pet_place(match_watch_pet_t *pet, match_watch_pet_page_t page)
{
    gfx_obj_t *obj;
    uint16_t disp_w;
    uint16_t disp_h;
    uint16_t obj_w = 0;
    uint16_t obj_h = 0;

    pet_host_place(pet, page);
    obj = pet_host_object(pet);
    if (obj != NULL) {
        disp_w = hw_gfx_runtime_width();
        disp_h = hw_gfx_runtime_height();
        if (gfx_obj_get_size(obj, &obj_w, &obj_h) == ESP_OK && obj_w > 0 && obj_h > 0) {
            gfx_coord_t x = (gfx_coord_t)((int32_t)disp_w / 2 - (int32_t)obj_w / 2);
            int32_t y = (int32_t)disp_h - (int32_t)obj_h - (int32_t)MATCH_WATCH_PET_BOTTOM_MARGIN;

            if (y < 0) {
                y = 0;
            }
            (void)gfx_obj_set_pos(obj, x, (gfx_coord_t)y);
        }
    }
}

void match_watch_pet_handle_touch(match_watch_pet_t *pet, const gfx_touch_event_t *event)
{
    pet_host_handle_touch(pet, event);
}

gfx_obj_t *match_watch_pet_object(match_watch_pet_t *pet)
{
    return pet_host_object(pet);
}

void match_watch_pet_logic_reset(match_watch_pet_logic_t *logic)
{
    if (logic == NULL) {
        return;
    }

    memset(logic, 0, sizeof(*logic));
    logic->limited_home_score = -1;
    logic->limited_away_score = -1;
    logic->last_host_score = -1;
}

const char *match_watch_pet_base_action(const match_data_schedule_item_t *match,
                                        const char *host_team,
                                        match_watch_home_timing_t timing)
{
    int home_score;
    int away_score;
    bool is_home_host;

    if (match == NULL) {
        return match_watch_pet_scene_action(SCENE_EVENT_KIND_IDLE, NULL, host_team);
    }

    if (match_watch_pet_match_is_finished(match)) {
        if (host_team != NULL && host_team[0] != '\0' &&
                match_watch_pet_parse_result_score(match->score_label, &home_score, &away_score)) {
            if (match_watch_pet_match_team_side(match, host_team, &is_home_host)) {
                int team_score = is_home_host ? home_score : away_score;
                int rival_score = is_home_host ? away_score : home_score;
                if (team_score > rival_score) {
                    return match_watch_pet_scene_action(SCENE_EVENT_KIND_POSITIVE, match, host_team);
                }
                if (team_score < rival_score) {
                    return match_watch_pet_scene_action(SCENE_EVENT_KIND_NEGATIVE, match, host_team);
                }
                return match_watch_pet_scene_action(SCENE_EVENT_KIND_NEUTRAL, match, host_team);
            }
        }
        return match_watch_pet_scene_action(SCENE_EVENT_KIND_NEUTRAL, match, host_team);
    }

    switch (match->state) {
    case MATCH_DATA_MATCH_GOAL:
        return match_watch_pet_scene_action(SCENE_EVENT_KIND_POSITIVE, match, host_team);
    case MATCH_DATA_MATCH_LOST:
        return match_watch_pet_scene_action(SCENE_EVENT_KIND_NEGATIVE, match, host_team);
    case MATCH_DATA_MATCH_LIVE:
    case MATCH_DATA_MATCH_HALF_TIME:
        if (host_team != NULL && host_team[0] != '\0' &&
                match_watch_pet_parse_result_score(match->score_label, &home_score, &away_score)) {
            if (match_watch_pet_match_team_side(match, host_team, &is_home_host)) {
                int team_score = is_home_host ? home_score : away_score;
                int rival_score = is_home_host ? away_score : home_score;
                return match_watch_pet_scene_action(team_score < rival_score ?
                                                    SCENE_EVENT_KIND_NEGATIVE :
                                                    SCENE_EVENT_KIND_ACTIVE,
                                                    match, host_team);
            }
        }
        return match_watch_pet_scene_action(SCENE_EVENT_KIND_ACTIVE, match, host_team);
    default:
        break;
    }

    if (timing.phase == MATCH_WATCH_HOME_PHASE_COUNTDOWN) {
        return match_watch_pet_scene_action(SCENE_EVENT_KIND_WAITING, match, host_team);
    }

    return match_watch_pet_scene_action(SCENE_EVENT_KIND_IDLE, match, host_team);
}

const char *match_watch_pet_live_score_action(match_watch_pet_logic_t *logic,
                                              const match_data_schedule_item_t *match,
                                              const char *team,
                                              bool finished)
{
    int home_score;
    int away_score;
    int team_score;
    int rival_score;
    bool is_home;

    if (logic == NULL || match == NULL || team == NULL || team[0] == '\0' || finished ||
            !match_watch_pet_parse_score(match->score_label, &home_score, &away_score)) {
        return NULL;
    }

    if (!match_watch_pet_match_team_side(match, team, &is_home)) {
        return NULL;
    }

    team_score = is_home ? home_score : away_score;
    rival_score = is_home ? away_score : home_score;
    if (logic->last_host_match_no != match->match_no) {
        logic->last_host_match_no = match->match_no;
        logic->last_host_score = team_score;
    } else if (team_score > logic->last_host_score) {
        logic->last_host_score = team_score;
        return MATCH_WATCH_PET_ACTION_JUMP;
    }

    logic->last_host_score = team_score;
    if (team_score < rival_score) {
        return MATCH_WATCH_PET_ACTION_LOSE;
    }
    return NULL;
}

static const char *match_watch_pet_random_far_action(match_watch_pet_logic_t *logic,
                                                     const match_watch_pet_policy_t *policy,
                                                     uint32_t now_ms)
{
    static const char *const s_actions[] = {
        MATCH_WATCH_PET_ACTION_WAVE,
        MATCH_WATCH_PET_ACTION_JUMP,
        MATCH_WATCH_PET_ACTION_RUNNING_LEFT,
        MATCH_WATCH_PET_ACTION_RUNNING_RIGHT,
    };

    if (logic->random_until_ms != 0U && now_ms < logic->random_until_ms) {
        return logic->random_action != NULL ? logic->random_action : MATCH_WATCH_PET_ACTION_IDLE;
    }

    logic->random_action = NULL;
    logic->random_until_ms = 0;
    if (logic->random_next_ms == 0U) {
        logic->random_next_ms = now_ms + policy->random_min_ms +
                                (esp_random() % policy->random_span_ms);
        return MATCH_WATCH_PET_ACTION_IDLE;
    }
    if (now_ms < logic->random_next_ms) {
        return MATCH_WATCH_PET_ACTION_IDLE;
    }

    logic->random_action = s_actions[esp_random() % MATCH_WATCH_PET_ARRAY_SIZE(s_actions)];
    logic->random_until_ms = now_ms + policy->random_hold_ms;
    logic->random_next_ms = logic->random_until_ms + policy->random_min_ms +
                            (esp_random() % policy->random_span_ms);
    return logic->random_action;
}

static const char *match_watch_pet_countdown_wait_action(match_watch_pet_logic_t *logic,
                                                         const match_watch_pet_policy_t *policy,
                                                         uint32_t now_ms)
{
    if (logic->wait_wave_until_ms != 0U && now_ms < logic->wait_wave_until_ms) {
        return MATCH_WATCH_PET_ACTION_WAVE;
    }

    logic->wait_wave_until_ms = 0;
    if (logic->wait_wave_next_ms == 0U) {
        logic->wait_wave_next_ms = now_ms + policy->wait_wave_min_ms +
                                   (esp_random() % policy->wait_wave_span_ms);
        return MATCH_WATCH_PET_ACTION_SLEEP;
    }
    if (now_ms < logic->wait_wave_next_ms) {
        return MATCH_WATCH_PET_ACTION_SLEEP;
    }

    logic->wait_wave_until_ms = now_ms + policy->wait_wave_hold_ms;
    logic->wait_wave_next_ms = logic->wait_wave_until_ms + policy->wait_wave_min_ms +
                               (esp_random() % policy->wait_wave_span_ms);
    return MATCH_WATCH_PET_ACTION_WAVE;
}

static const char *match_watch_pet_live_running_action(match_watch_pet_logic_t *logic,
                                                       const match_watch_pet_policy_t *policy,
                                                       uint32_t now_ms)
{
    static const char *const s_actions[] = {
        MATCH_WATCH_PET_ACTION_RUNNING,
        MATCH_WATCH_PET_ACTION_RUNNING_LEFT,
        MATCH_WATCH_PET_ACTION_RUNNING_RIGHT,
    };

    if (logic->live_run_action != NULL && now_ms < logic->live_run_next_ms) {
        return logic->live_run_action;
    }

    logic->live_run_action = s_actions[esp_random() % MATCH_WATCH_PET_ARRAY_SIZE(s_actions)];
    logic->live_run_next_ms = now_ms + policy->live_run_min_ms +
                              (esp_random() % policy->live_run_span_ms);
    return logic->live_run_action;
}

static void match_watch_pet_start_limited_action(match_watch_pet_logic_t *logic,
                                                 const match_watch_pet_policy_t *policy,
                                                 const match_data_schedule_item_t *match,
                                                 const char *action,
                                                 int home_score, int away_score,
                                                 uint32_t now_ms)
{
    if (logic == NULL || policy == NULL || match == NULL || action == NULL) {
        return;
    }

    logic->limited_action = action;
    logic->limited_until_ms = now_ms + policy->limited_ms;
    logic->limited_match_no = match->match_no;
    logic->limited_home_score = home_score;
    logic->limited_away_score = away_score;
}

const char *match_watch_pet_pick_action(match_watch_pet_logic_t *logic,
                                        const match_watch_pet_policy_t *policy,
                                        const match_data_schedule_item_t *match,
                                        bool has_match,
                                        bool waiting_for_data,
                                        match_watch_home_timing_t timing,
                                        const char *base_action,
                                        const char *score_action)
{
    uint32_t now_ms = match_watch_pet_now_ms();
    int home_score = -1;
    int away_score = -1;
    bool score_known;

    if (logic == NULL || policy == NULL || waiting_for_data || !has_match || match == NULL) {
        if (logic != NULL) {
            logic->limited_action = NULL;
        }
        return MATCH_WATCH_PET_ACTION_IDLE;
    }

    if (timing.phase == MATCH_WATCH_HOME_PHASE_FAR) {
        logic->limited_action = NULL;
        logic->wait_wave_next_ms = 0;
        logic->wait_wave_until_ms = 0;
        logic->live_run_action = NULL;
        logic->live_run_next_ms = 0;
        return match_watch_pet_random_far_action(logic, policy, now_ms);
    }

    logic->random_action = NULL;
    logic->random_next_ms = 0;
    logic->random_until_ms = 0;

    if (timing.phase == MATCH_WATCH_HOME_PHASE_COUNTDOWN) {
        logic->limited_action = NULL;
        logic->live_run_action = NULL;
        logic->live_run_next_ms = 0;
        return match_watch_pet_countdown_wait_action(logic, policy, now_ms);
    }

    logic->wait_wave_next_ms = 0;
    logic->wait_wave_until_ms = 0;

    if (timing.phase == MATCH_WATCH_HOME_PHASE_FINISHED) {
        logic->limited_action = NULL;
        logic->live_run_action = NULL;
        logic->live_run_next_ms = 0;
        return base_action != NULL ? base_action : MATCH_WATCH_PET_ACTION_WAVE;
    }

    score_known = match_watch_pet_parse_score(match->score_label, &home_score, &away_score);
    if (timing.phase == MATCH_WATCH_HOME_PHASE_LIVE && score_known) {
        bool score_changed = match->match_no != logic->limited_match_no ||
                             home_score != logic->limited_home_score ||
                             away_score != logic->limited_away_score;
        if ((score_action != NULL && strcmp(score_action, MATCH_WATCH_PET_ACTION_JUMP) == 0) ||
                (base_action != NULL && strcmp(base_action, MATCH_WATCH_PET_ACTION_JUMP) == 0 && score_changed)) {
            match_watch_pet_start_limited_action(logic, policy, match, MATCH_WATCH_PET_ACTION_JUMP,
                                                 home_score, away_score, now_ms);
        } else if (base_action != NULL && strcmp(base_action, MATCH_WATCH_PET_ACTION_LOSE) == 0 && score_changed) {
            match_watch_pet_start_limited_action(logic, policy, match, MATCH_WATCH_PET_ACTION_LOSE,
                                                 home_score, away_score, now_ms);
        }
    }

    if (logic->limited_action != NULL && match->match_no == logic->limited_match_no &&
            now_ms < logic->limited_until_ms) {
        return logic->limited_action;
    }
    logic->limited_action = NULL;
    return match_watch_pet_live_running_action(logic, policy, now_ms);
}

const char *match_watch_pet_update_action(match_watch_pet_logic_t *logic,
                                          const match_watch_pet_policy_t *policy,
                                          const match_data_schedule_item_t *match,
                                          bool has_match,
                                          bool waiting_for_data,
                                          match_watch_home_timing_t timing,
                                          const char *host_team)
{
    const char *base_action;
    const char *score_action;

    base_action = match_watch_pet_base_action(has_match ? match : NULL, host_team, timing);
    score_action = match_watch_pet_live_score_action(logic, has_match ? match : NULL,
                                                     host_team, false);
    return match_watch_pet_pick_action(logic, policy, has_match ? match : NULL, has_match,
                                       waiting_for_data, timing, base_action, score_action);
}
