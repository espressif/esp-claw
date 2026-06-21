/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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

static const uint32_t s_match_watch_left_team_palette[] = {
    0xFCDB02,
    0x326FFF,
    0x78DAEC,
    0x1D9E75,
    0xF04D4D,
};

#define MATCH_WATCH_CARD_SCORE_Y            28
#define MATCH_WATCH_CARD_SUB_Y              96
#define MATCH_WATCH_CARD_TOP_FINISHED_Y     6
#define MATCH_WATCH_CARD_TOP_FINISHED_H     38
#define MATCH_WATCH_DETAIL_META_ROW_Y       204U
#define MATCH_WATCH_DETAIL_META_ROW_H       30U

static uint32_t match_watch_app_hash_str(uint32_t hash, const char *text)
{
    const unsigned char *p = (const unsigned char *)(text != NULL ? text : "");

    while (*p != '\0') {
        hash ^= *p++;
        hash *= 16777619U;
    }
    return hash;
}

static gfx_color_t match_watch_app_team_palette_color(const char *team)
{
    uint32_t key;
    size_t color_index;

    if (team == NULL || team[0] == '\0') {
        return MATCH_WATCH_COLOR_ACCENT;
    }
    key = match_watch_app_hash_str(2166136261U, team);
    color_index = key % MATCH_WATCH_ARRAY_SIZE(s_match_watch_left_team_palette);
    return GFX_COLOR_HEX(s_match_watch_left_team_palette[color_index]);
}

static bool match_watch_app_team_side_is_host(const match_data_schedule_item_t *match,
                                              const char *team, bool home_side)
{
    if (match == NULL || team == NULL || team[0] == '\0') {
        return false;
    }
    return strcasecmp(team, home_side ? match->home : match->away) == 0 ||
           strcasecmp(team, home_side ? match->home_code : match->away_code) == 0 ||
           strcasecmp(team, home_side ? match->home_display : match->away_display) == 0;
}

static bool match_watch_app_host_is_home_side(const match_data_schedule_item_t *match)
{
    if (match == NULL) {
        return true;
    }
    if (s_app == NULL || !s_app->selection.host_team_selected ||
            s_app->selection.host_team[0] == '\0') {
        return true;
    }
    if (match_watch_app_team_side_is_host(match, s_app->selection.host_team, true)) {
        return true;
    }
    if (match_watch_app_team_side_is_host(match, s_app->selection.host_team, false)) {
        return false;
    }
    return true;
}

static gfx_color_t match_watch_app_host_team_color(const match_data_schedule_item_t *match, bool has_match)
{
    const char *team;
    uint32_t key;
    gfx_color_t resolved_color;

    if (!has_match || match == NULL) {
        s_app->ui.left_team_color_valid = false;
        return MATCH_WATCH_COLOR_ACCENT;
    }

    if (s_app->selection.host_team_selected && s_app->selection.host_team[0] != '\0') {
        team = s_app->selection.host_team;
    } else {
        team = match->home;
    }

    key = match_watch_app_hash_str(2166136261U, team);
    resolved_color = match_watch_app_team_palette_color(team);
    if (!s_app->ui.left_team_color_valid || s_app->ui.left_team_color_key != key ||
            s_app->ui.left_team_color.full != resolved_color.full) {
        s_app->ui.left_team_color = resolved_color;
        s_app->ui.left_team_color_key = key;
        s_app->ui.left_team_color_valid = true;
        ESP_LOGD(TAG, "host team color resolved: team=%s match=%u",
                 team, (unsigned)match->match_no);
    }
    return s_app->ui.left_team_color;
}

static void match_watch_app_apply_card_score_layout(bool finished_result_top, uint32_t screen_w)
{
    bool left_bg = !finished_result_top;

    if (s_app->ui.card_left_main != NULL) {
        (void)gfx_obj_set_pos(s_app->ui.card_left_main, 0, MATCH_WATCH_CARD_SCORE_Y);
        (void)gfx_label_set_bg_enable(s_app->ui.card_left_main, left_bg);
    }
    if (s_app->ui.card_right_main != NULL) {
        (void)gfx_obj_set_pos(s_app->ui.card_right_main, (gfx_coord_t)(screen_w - 128U), MATCH_WATCH_CARD_SCORE_Y);
    }
    if (s_app->ui.card_left_sub != NULL) {
        (void)gfx_obj_set_pos(s_app->ui.card_left_sub, 0, MATCH_WATCH_CARD_SUB_Y);
        (void)gfx_label_set_bg_enable(s_app->ui.card_left_sub, left_bg);
    }
    if (s_app->ui.card_right_sub != NULL) {
        (void)gfx_obj_set_pos(s_app->ui.card_right_sub, (gfx_coord_t)(screen_w - 128U), MATCH_WATCH_CARD_SUB_Y);
    }
}

static bool match_watch_app_is_countdown_subs(const char *left_sub, const char *right_sub)
{
    return left_sub != NULL && right_sub != NULL &&
           strcmp(left_sub, "MINUTE") == 0 && strcmp(right_sub, "SECOND") == 0;
}

static bool match_watch_app_is_finished_result_top(const char *top)
{
    return top != NULL &&
           (strcmp(top, "WIN") == 0 || strcmp(top, "LOSS") == 0 || strcmp(top, "DRAW") == 0);
}

static void match_watch_app_format_finished_top(const match_data_schedule_item_t *match,
                                                int home_score, int away_score,
                                                char *buf, size_t buf_size)
{
    const char *host_team;
    bool host_is_home;
    int host_score;
    int rival_score;

    if (buf == NULL || buf_size == 0) {
        return;
    }
    if (s_app == NULL || !s_app->selection.host_team_selected ||
            s_app->selection.host_team[0] == '\0' || match == NULL) {
        if (home_score > away_score) {
            snprintf(buf, buf_size, "WIN");
        } else if (home_score < away_score) {
            snprintf(buf, buf_size, "LOSS");
        } else {
            snprintf(buf, buf_size, "DRAW");
        }
        return;
    }

    host_team = s_app->selection.host_team;
    if (match_watch_app_team_side_is_host(match, host_team, true)) {
        host_is_home = true;
    } else if (match_watch_app_team_side_is_host(match, host_team, false)) {
        host_is_home = false;
    } else {
        snprintf(buf, buf_size, "DRAW");
        return;
    }

    host_score = host_is_home ? home_score : away_score;
    rival_score = host_is_home ? away_score : home_score;
    if (host_score > rival_score) {
        snprintf(buf, buf_size, "WIN");
    } else if (host_score < rival_score) {
        snprintf(buf, buf_size, "LOSS");
    } else {
        snprintf(buf, buf_size, "DRAW");
    }
}

static void match_watch_app_set_main_font_for_text(gfx_obj_t *obj, const char *text, bool score_number_enabled)
{
    (void)text;
    (void)score_number_enabled;
    if (obj == NULL) {
        return;
    }
    (void)gfx_label_set_font(obj, (gfx_font_t)&font_match_bogle_70_4);
}

static void match_watch_app_set_sub_font_for_text(gfx_obj_t *obj, const char *text, bool countdown_subs)
{
    (void)text;
    (void)countdown_subs;
    if (obj == NULL) {
        return;
    }
    (void)gfx_label_set_font(obj, (gfx_font_t)&font_match_bogle_24_4);
}

static const char *match_watch_app_stage_english_label(match_data_stage_t stage)
{
    switch (stage) {
    case MATCH_DATA_STAGE_GROUP:
        return "GROUP";
    case MATCH_DATA_STAGE_ROUND_OF_32:
        return "ROUND OF 32";
    case MATCH_DATA_STAGE_ROUND_OF_16:
        return "ROUND OF 16";
    case MATCH_DATA_STAGE_QUARTER_FINAL:
        return "QUARTER FINAL";
    case MATCH_DATA_STAGE_SEMI_FINAL:
        return "SEMI FINAL";
    case MATCH_DATA_STAGE_THIRD_PLACE:
        return "THIRD PLACE";
    case MATCH_DATA_STAGE_FINAL:
        return "FINAL";
    default:
        return "MATCH";
    }
}

static void match_watch_app_format_detail_meta(const match_data_schedule_item_t *match,
                                               char *english, size_t english_size)
{
    const char *venue = match_data_localized_venue_name(match != NULL ? match->venue : "");
    const char *group = match != NULL ? match->group : "";
    char stage_part[48];

    if (english == NULL || english_size == 0) {
        return;
    }

    if (match == NULL) {
        snprintf(english, english_size, "MATCH");
        return;
    }

    if (match->stage == MATCH_DATA_STAGE_GROUP) {
        if (group != NULL && group[0] != '\0') {
            snprintf(stage_part, sizeof(stage_part), "GROUP %s", group);
        } else {
            snprintf(stage_part, sizeof(stage_part), "GROUP");
        }
    } else {
        snprintf(stage_part, sizeof(stage_part), "%s",
                 match_watch_app_stage_english_label(match->stage));
    }

    if (venue != NULL && venue[0] != '\0') {
        snprintf(english, english_size, "##0xFFFFFF%s · ##0x787878%s", stage_part, venue);
    } else {
        snprintf(english, english_size, "##0xFFFFFF%s", stage_part);
    }
}

void match_watch_app_set_detail_info_visible(bool visible)
{
    if (s_app->ui.detail_time != NULL) {
        (void)gfx_obj_set_visible(s_app->ui.detail_time, visible);
    }
    if (s_app->ui.detail_meta != NULL) {
        (void)gfx_obj_set_visible(s_app->ui.detail_meta, visible);
    }
}

void match_watch_app_set_card_visible(bool visible)
{
    /* main/sub visibility is owned by set_card_texts to avoid stale score/code flash */
    gfx_obj_t *items[] = {
        s_app->ui.card_top,
        s_app->ui.card_center,
    };

    for (size_t i = 0; i < MATCH_WATCH_ARRAY_SIZE(items); i++) {
        if (items[i] != NULL) {
            (void)gfx_obj_set_visible(items[i], visible);
        }
    }
}

void match_watch_app_set_pet_visible(bool visible)
{
    match_watch_pet_set_visible(&s_app->pet.handle, visible);
}

void match_watch_app_place_pet_for_page(match_watch_page_t page)
{
    (void)page;

    match_watch_pet_place(&s_app->pet.handle, MATCH_WATCH_PET_PAGE_HOME);
}

static void match_watch_app_format_beijing_time(char *buf, size_t buf_size)
{
    time_t now;
    struct tm tm_info;

    if (buf == NULL || buf_size == 0) {
        return;
    }
    now = time(NULL);
    if (now <= 0 || localtime_r(&now, &tm_info) == NULL) {
        snprintf(buf, buf_size, "--:--");
        return;
    }
    snprintf(buf, buf_size, "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
}

void match_watch_app_apply_time_home_layout(void)
{
    uint32_t screen_w = gfx_disp_get_hor_res(disp_default);

    if (match_watch_platform_lock() != ESP_OK) {
        return;
    }

    if (s_app->ui.card_top != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_top, (uint16_t)screen_w, 42);
        (void)gfx_obj_set_pos(s_app->ui.card_top, 0, 12);
        (void)gfx_label_set_font(s_app->ui.card_top, (gfx_font_t)&font_match_bogle_32_4);
        (void)gfx_label_set_color(s_app->ui.card_top, match_watch_platform_color(MATCH_WATCH_COLOR_WHITE));
        (void)gfx_label_set_text_align(s_app->ui.card_top, GFX_TEXT_ALIGN_CENTER);
    }

    match_watch_platform_unlock();
}

void match_watch_app_apply_detail_page_layout(void)
{
    uint32_t screen_w = gfx_disp_get_hor_res(disp_default);

    if (match_watch_platform_lock() != ESP_OK) {
        return;
    }

    if (s_app->ui.card_top != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_top, (uint16_t)screen_w, 24);
        (void)gfx_obj_set_pos(s_app->ui.card_top, 0, 5);
        (void)gfx_label_set_text_align(s_app->ui.card_top, GFX_TEXT_ALIGN_CENTER);
        (void)gfx_label_set_font(s_app->ui.card_top, (gfx_font_t)&font_match_bogle_20_4);
        (void)gfx_label_set_color(s_app->ui.card_top, match_watch_platform_color(MATCH_WATCH_COLOR_TOP_MUTED));
    }
    if (s_app->ui.card_left_main != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_left_main, 128, 82);
        (void)gfx_obj_set_pos(s_app->ui.card_left_main, 0, 28);
    }
    if (s_app->ui.card_left_sub != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_left_sub, 128, 30);
        (void)gfx_obj_set_pos(s_app->ui.card_left_sub, 0, 96);
        (void)gfx_label_set_long_mode(s_app->ui.card_left_sub, GFX_LABEL_LONG_CLIP);
    }
    if (s_app->ui.card_center != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_center, 60, 40);
        (void)gfx_obj_set_pos(s_app->ui.card_center, (gfx_coord_t)((screen_w - 60U) / 2U), 51);
        (void)gfx_label_set_font(s_app->ui.card_center, (gfx_font_t)&font_match_bogle_32_4);
        (void)gfx_label_set_color(s_app->ui.card_center, match_watch_platform_color(MATCH_WATCH_COLOR_META_MUTED));
        (void)gfx_label_set_text_align(s_app->ui.card_center, GFX_TEXT_ALIGN_CENTER);
    }
    if (s_app->ui.card_right_main != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_right_main, 128, 82);
        (void)gfx_obj_set_pos(s_app->ui.card_right_main, (gfx_coord_t)(screen_w - 128U), 28);
        (void)gfx_label_set_color(s_app->ui.card_right_main, match_watch_platform_color(MATCH_WATCH_COLOR_WHITE));
    }
    if (s_app->ui.card_right_sub != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_right_sub, 128, 30);
        (void)gfx_obj_set_pos(s_app->ui.card_right_sub, (gfx_coord_t)(screen_w - 128U), 96);
        (void)gfx_label_set_color(s_app->ui.card_right_sub, match_watch_platform_color(MATCH_WATCH_COLOR_WHITE));
        (void)gfx_label_set_long_mode(s_app->ui.card_right_sub, GFX_LABEL_LONG_CLIP);
    }
    if (s_app->ui.detail_time != NULL) {
        (void)gfx_obj_set_size(s_app->ui.detail_time, 280, 62);
        (void)gfx_obj_set_pos(s_app->ui.detail_time, (gfx_coord_t)((screen_w - 280U) / 2U), 124);
        (void)gfx_label_set_font(s_app->ui.detail_time, (gfx_font_t)&font_match_bogle_55_4);
        (void)gfx_label_set_color(s_app->ui.detail_time, match_watch_platform_color(MATCH_WATCH_COLOR_WHITE));
    }
    if (s_app->ui.detail_meta != NULL) {
        (void)gfx_obj_set_size(s_app->ui.detail_meta, (uint16_t)screen_w, MATCH_WATCH_DETAIL_META_ROW_H);
        (void)gfx_obj_set_pos(s_app->ui.detail_meta, 0, MATCH_WATCH_DETAIL_META_ROW_Y);
        (void)gfx_label_set_font(s_app->ui.detail_meta, (gfx_font_t)&font_match_bogle_24_4);
        (void)gfx_label_set_color(s_app->ui.detail_meta, match_watch_platform_color(MATCH_WATCH_COLOR_WHITE));
        (void)gfx_label_set_text_align(s_app->ui.detail_meta, GFX_TEXT_ALIGN_CENTER);
        (void)gfx_label_set_line_spacing(s_app->ui.detail_meta, 0);
    }

    match_watch_platform_unlock();
}

void match_watch_app_apply_home_card_layout(void)
{
    uint32_t screen_w = gfx_disp_get_hor_res(disp_default);

    if (match_watch_platform_lock() != ESP_OK) {
        return;
    }

    if (s_app->ui.card_top != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_top, 220, 24);
        (void)gfx_obj_set_pos(s_app->ui.card_top, (gfx_coord_t)((screen_w - 220U) / 2U), 5);
    }
    if (s_app->ui.card_left_main != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_left_main, 128, 82);
        (void)gfx_obj_set_pos(s_app->ui.card_left_main, 0, 28);
    }
    if (s_app->ui.card_left_sub != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_left_sub, 128, 30);
        (void)gfx_obj_set_pos(s_app->ui.card_left_sub, 0, 96);
        (void)gfx_label_set_long_mode(s_app->ui.card_left_sub, GFX_LABEL_LONG_CLIP);
    }
    if (s_app->ui.card_center != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_center, 60, 26);
        (void)gfx_obj_set_pos(s_app->ui.card_center, (gfx_coord_t)((screen_w - 60U) / 2U), 58);
        (void)gfx_label_set_font(s_app->ui.card_center, (gfx_font_t)&font_match_bogle_20_4);
        (void)gfx_label_set_color(s_app->ui.card_center, match_watch_platform_color(MATCH_WATCH_COLOR_META_MUTED));
    }
    if (s_app->ui.card_right_main != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_right_main, 128, 82);
        (void)gfx_obj_set_pos(s_app->ui.card_right_main, (gfx_coord_t)(screen_w - 128U), 28);
        (void)gfx_label_set_color(s_app->ui.card_right_main, match_watch_platform_color(MATCH_WATCH_COLOR_WHITE));
    }
    if (s_app->ui.card_right_sub != NULL) {
        (void)gfx_obj_set_size(s_app->ui.card_right_sub, 128, 30);
        (void)gfx_obj_set_pos(s_app->ui.card_right_sub, (gfx_coord_t)(screen_w - 128U), 96);
        (void)gfx_label_set_color(s_app->ui.card_right_sub, match_watch_platform_color(MATCH_WATCH_COLOR_WHITE));
        (void)gfx_label_set_long_mode(s_app->ui.card_right_sub, GFX_LABEL_LONG_CLIP);
    }

    match_watch_platform_unlock();
}

static void match_watch_app_copy_date_label(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    strlcpy(dst, src != NULL ? src : "", dst_size);
    for (char *p = dst; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '.';
        }
    }
}

static void match_watch_app_color_to_inline_hex(gfx_color_t color, char *hex, size_t hex_size)
{
    uint16_t c = color.full;

    if (hex == NULL || hex_size < 7) {
        return;
    }

    snprintf(hex, hex_size, "%02X%02X%02X",
             (unsigned)((c & 0xF800U) >> 8),
             (unsigned)((c & 0x07E0U) >> 3),
             (unsigned)((c & 0x001FU) << 3));
}

void match_watch_app_format_card_kickoff(const match_data_schedule_item_t *match, char *buf, size_t buf_size)
{
    struct tm tm_info;
    time_t kickoff;
    char date_label[24];
    char beijing_label[32];

    if (buf == NULL || buf_size == 0) {
        return;
    }
    if (match == NULL) {
        snprintf(buf, buf_size, "##0x767676 UPCOMING ##0xFFFFFF --.-- --:--");
        return;
    }
    if (match->beijing_label[0] != '\0') {
        match_watch_app_copy_date_label(beijing_label, sizeof(beijing_label), match->beijing_label);
        snprintf(buf, buf_size, "##0x767676 UPCOMING ##0xFFFFFF %s", beijing_label);
        return;
    }
    if (match->date_label[0] != '\0' || match->time_label[0] != '\0') {
        match_watch_app_copy_date_label(date_label, sizeof(date_label),
                                        match->date_label[0] != '\0' ? match->date_label : "--.--");
        snprintf(buf, buf_size, "##0x767676 UPCOMING ##0xFFFFFF %s %s",
                 date_label,
                 match->time_label[0] != '\0' ? match->time_label : "--:--");
        return;
    }
    if (match->kickoff_ts == 0U) {
        snprintf(buf, buf_size, "##0x767676 UPCOMING ##0xFFFFFF --.-- --:--");
        return;
    }

    kickoff = (time_t)match->kickoff_ts;
    localtime_r(&kickoff, &tm_info);
    snprintf(buf, buf_size, "##0x767676 UPCOMING ##0xFFFFFF %02d.%02d %02d:%02d",
             tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min);
}

void match_watch_app_format_detail_time(const match_data_schedule_item_t *match, char *buf, size_t buf_size)
{
    struct tm tm_info;
    time_t kickoff;
    char date_label[24];
    char beijing_label[32];

    if (buf == NULL || buf_size == 0) {
        return;
    }
    if (match == NULL || match->kickoff_ts == 0U) {
        if (match != NULL && match->beijing_label[0] != '\0') {
            match_watch_app_copy_date_label(beijing_label, sizeof(beijing_label), match->beijing_label);
            snprintf(buf, buf_size, "%s", beijing_label);
        } else if (match != NULL && (match->date_label[0] != '\0' || match->time_label[0] != '\0')) {
            match_watch_app_copy_date_label(date_label, sizeof(date_label),
                                            match->date_label[0] != '\0' ? match->date_label : "--.--");
            snprintf(buf, buf_size, "%s %s",
                     date_label,
                     match->time_label[0] != '\0' ? match->time_label : "--:--");
        } else {
            snprintf(buf, buf_size, "--.-- --:--");
        }
        return;
    }
    if (match->state == MATCH_DATA_MATCH_PENALTY_WIN) {
        snprintf(buf, buf_size, "PENALTY");
        return;
    }
    if (match_watch_app_match_is_finished(match)) {
        snprintf(buf, buf_size, "FULL TIME");
        return;
    }

    kickoff = (time_t)match->kickoff_ts;
    localtime_r(&kickoff, &tm_info);
    snprintf(buf, buf_size, "%d.%d %02d:%02d",
             tm_info.tm_mon + 1, tm_info.tm_mday, tm_info.tm_hour, tm_info.tm_min);
}

bool match_watch_app_parse_score(const char *score, int *home_score, int *away_score)
{
    int home;
    int away;
    float home_float;
    float away_float;

    if (score == NULL) {
        return false;
    }
    if (sscanf(score, "%d-%d", &home, &away) != 2) {
        if (sscanf(score, "%f-%f", &home_float, &away_float) != 2) {
            return false;
        }
        home = (int)home_float;
        away = (int)away_float;
    }

    if (home_score != NULL) {
        *home_score = home;
    }
    if (away_score != NULL) {
        *away_score = away;
    }
    return true;
}

bool match_watch_app_parse_result_score(const char *score, int *home_score, int *away_score)
{
    int home;
    int away;
    int penalty_home;
    int penalty_away;

    if (score == NULL || sscanf(score, "%d-%d 点%d-%d", &home, &away, &penalty_home, &penalty_away) != 4) {
        return match_watch_app_parse_score(score, home_score, away_score);
    }

    if (home_score != NULL) {
        *home_score = penalty_home;
    }
    if (away_score != NULL) {
        *away_score = penalty_away;
    }
    return true;
}

bool match_watch_app_parse_penalty_score(const char *score, int *home_score, int *away_score,
                                                int *home_penalty, int *away_penalty)
{
    int home;
    int away;
    int penalty_home;
    int penalty_away;

    if (score == NULL || sscanf(score, "%d-%d 点%d-%d", &home, &away, &penalty_home, &penalty_away) != 4) {
        return false;
    }

    if (home_score != NULL) {
        *home_score = home;
    }
    if (away_score != NULL) {
        *away_score = away;
    }
    if (home_penalty != NULL) {
        *home_penalty = penalty_home;
    }
    if (away_penalty != NULL) {
        *away_penalty = penalty_away;
    }
    return true;
}

const char *match_watch_app_detail_stage_label(const match_data_schedule_item_t *match)
{
    if (match == NULL) {
        return "";
    }
    if (match->stage == MATCH_DATA_STAGE_GROUP && match->group[0] != '\0') {
        return match->group;
    }
    return match_data_stage_name(match->stage);
}

uint32_t match_watch_app_live_display_minute(const match_data_schedule_item_t *match)
{
    if (match == NULL || match->state == MATCH_DATA_MATCH_HALF_TIME) {
        return 0;
    }
    if (match->live_minute > 0U) {
        return match->live_minute;
    }
    return 0;
}

match_watch_home_phase_t match_watch_app_home_raw_phase(const match_data_schedule_item_t *match,
                                                               int64_t now, int64_t *remain_s,
                                                               int64_t *elapsed_s)
{
    int64_t remain = 0;
    int64_t elapsed = 0;

    if (match == NULL) {
        return MATCH_WATCH_HOME_PHASE_FAR;
    }

    if (match_watch_app_match_is_finished(match)) {
        return MATCH_WATCH_HOME_PHASE_FINISHED;
    }

    if (match->kickoff_ts != 0U && now > 0) {
        if (now < (int64_t)match->kickoff_ts) {
            remain = (int64_t)match->kickoff_ts - now;
        } else {
            elapsed = now - (int64_t)match->kickoff_ts;
        }
    }

    if (remain_s != NULL) {
        *remain_s = remain;
    }
    if (elapsed_s != NULL) {
        *elapsed_s = elapsed;
    }

    switch (match->state) {
    case MATCH_DATA_MATCH_LIVE:
    case MATCH_DATA_MATCH_GOAL:
    case MATCH_DATA_MATCH_LOST:
        return MATCH_WATCH_HOME_PHASE_LIVE;
    case MATCH_DATA_MATCH_HALF_TIME:
        if (elapsed_s != NULL) {
            *elapsed_s = 45 * 60;
        }
        return MATCH_WATCH_HOME_PHASE_LIVE;
    default:
        break;
    }

    if (remain > 0 && remain <= 60 * 60) {
        return MATCH_WATCH_HOME_PHASE_COUNTDOWN;
    }
    return MATCH_WATCH_HOME_PHASE_FAR;
}

void match_watch_app_get_home_timing(const match_data_schedule_item_t *match,
                                            match_watch_home_timing_t *timing)
{
    int64_t raw_remain = 0;
    int64_t raw_elapsed = 0;
    int64_t value;
    int64_t scaled_delta;
    time_t now = time(NULL);
    int64_t now_ms = esp_timer_get_time() / 1000;
    match_watch_home_phase_t phase;

    if (timing == NULL) {
        return;
    }

    memset(timing, 0, sizeof(*timing));
    timing->phase = MATCH_WATCH_HOME_PHASE_FAR;
    if (match == NULL || now <= 0) {
        return;
    }

    if (match->state == MATCH_DATA_MATCH_HALF_TIME) {
        timing->phase = MATCH_WATCH_HOME_PHASE_LIVE;
        timing->elapsed_s = 45 * 60;
        return;
    }

    phase = match_watch_app_home_raw_phase(match, now, &raw_remain, &raw_elapsed);
    if (phase == MATCH_WATCH_HOME_PHASE_LIVE && match->live_minute > 0U) {
        raw_elapsed = ((int64_t)match->live_minute - 1) * 60;
    } else if (phase == MATCH_WATCH_HOME_PHASE_LIVE) {
        raw_elapsed = 0;
    }
    value = (phase == MATCH_WATCH_HOME_PHASE_COUNTDOWN) ? raw_remain :
            (phase == MATCH_WATCH_HOME_PHASE_LIVE) ? raw_elapsed : 0;

    if (s_app->timing.match_no != match->match_no || s_app->timing.phase != phase ||
            (phase == MATCH_WATCH_HOME_PHASE_LIVE && s_app->timing.live_minute != match->live_minute) ||
            s_app->timing.real_base_ms <= 0) {
        s_app->timing.match_no = match->match_no;
        s_app->timing.phase = phase;
        s_app->timing.real_base_ms = now_ms;
        s_app->timing.value_base = value;
        s_app->timing.live_minute = match->live_minute;
    }

    timing->phase = phase;
    if (phase != MATCH_WATCH_HOME_PHASE_COUNTDOWN && phase != MATCH_WATCH_HOME_PHASE_LIVE) {
        return;
    }

    scaled_delta = (now_ms - s_app->timing.real_base_ms) / 1000;
    if (phase == MATCH_WATCH_HOME_PHASE_COUNTDOWN) {
        timing->remain_s = s_app->timing.value_base - scaled_delta;
        if (timing->remain_s <= 0) {
            timing->remain_s = 0;
            timing->elapsed_s = 0;
            timing->phase = MATCH_WATCH_HOME_PHASE_LIVE;
        }
    } else {
        timing->elapsed_s = s_app->timing.value_base + scaled_delta;
        if (match->live_minute > 0U) {
            int64_t max_elapsed_s = ((int64_t)match->live_minute * 60) - 1;
            if (timing->elapsed_s > max_elapsed_s) {
                timing->elapsed_s = max_elapsed_s;
            }
        } else {
            timing->elapsed_s = 0;
        }
    }
}

static void match_watch_app_format_match_session_top(const match_data_schedule_item_t *match,
                                                     char *buf, size_t buf_size)
{
    const char *stage_label;

    if (buf == NULL || buf_size == 0) {
        return;
    }
    if (match == NULL) {
        snprintf(buf, buf_size, "MATCH");
        return;
    }

    if (match->stage == MATCH_DATA_STAGE_GROUP) {
        if (match->group[0] != '\0' && match->round > 0U) {
            snprintf(buf, buf_size, "GROUP %s-M%u", match->group, (unsigned)match->round);
        } else if (match->group[0] != '\0') {
            snprintf(buf, buf_size, "GROUP %s", match->group);
        } else if (match->round > 0U) {
            snprintf(buf, buf_size, "GROUP-M%u", (unsigned)match->round);
        } else {
            snprintf(buf, buf_size, "GROUP");
        }
        return;
    }

    stage_label = match_watch_app_stage_english_label(match->stage);
    if (match->round > 0U) {
        snprintf(buf, buf_size, "%s-M%u", stage_label, (unsigned)match->round);
    } else {
        snprintf(buf, buf_size, "%s", stage_label);
    }
}

void match_watch_app_format_live_top(const match_data_schedule_item_t *match,
                                            match_watch_home_timing_t timing,
                                            bool detail_page, char *buf, size_t buf_size)
{
    (void)timing;

    if (buf == NULL || buf_size == 0) {
        return;
    }
    if (detail_page) {
        if (match != NULL && match->state == MATCH_DATA_MATCH_HALF_TIME) {
            snprintf(buf, buf_size, "HALF TIME");
        } else {
            snprintf(buf, buf_size, "LIVE");
        }
        return;
    }
    match_watch_app_format_match_session_top(match, buf, buf_size);
}

void match_watch_app_format_score_fields(const match_data_schedule_item_t *match,
                                                char *left_main, size_t left_main_size,
                                                char *right_main, size_t right_main_size,
                                                int *home_score, int *away_score)
{
    int home = 0;
    int away = 0;
    int home_penalty = 0;
    int away_penalty = 0;

    if (match_watch_app_parse_penalty_score(match != NULL ? match->score_label : NULL,
                                            &home, &away, &home_penalty, &away_penalty)) {
        snprintf(left_main, left_main_size, "%d(%d)", home, home_penalty);
        snprintf(right_main, right_main_size, "%d(%d)", away, away_penalty);
    } else if (!match_watch_app_parse_score(match != NULL ? match->score_label : NULL, &home, &away)) {
        snprintf(left_main, left_main_size, "-");
        snprintf(right_main, right_main_size, "-");
    } else {
        snprintf(left_main, left_main_size, "%d", home);
        snprintf(right_main, right_main_size, "%d", away);
    }
    if (home_score != NULL) {
        *home_score = home;
    }
    if (away_score != NULL) {
        *away_score = away;
    }
}

static void match_watch_app_format_center_score(const match_data_schedule_item_t *match,
                                                char *buf, size_t buf_size)
{
    int home = 0;
    int away = 0;

    if (buf == NULL || buf_size == 0) {
        return;
    }
    if (!match_watch_app_parse_score(match != NULL ? match->score_label : NULL, &home, &away)) {
        snprintf(buf, buf_size, "0-0");
        return;
    }
    snprintf(buf, buf_size, "%d-%d", home, away);
}

void match_watch_app_format_card(const match_data_schedule_item_t *match, bool has_match,
                                      bool waiting_for_data, bool detail_page,
                                      char *top, size_t top_size,
                                      char *left_main, size_t left_main_size,
                                      char *left_sub, size_t left_sub_size,
                                      char *right_main, size_t right_main_size,
                                      char *right_sub, size_t right_sub_size)
{
    int home_score = 0;
    int away_score = 0;
    match_watch_home_timing_t timing;
    char home_code[8] = "---";
    char away_code[8] = "---";

    if (top == NULL || left_main == NULL || left_sub == NULL || right_main == NULL ||
            right_sub == NULL) {
        return;
    }

    if (waiting_for_data || !has_match || match == NULL) {
        snprintf(top, top_size, "%s", waiting_for_data ? "NO NETWORK" : "NO MATCH");
        snprintf(left_main, left_main_size, "--");
        snprintf(left_sub, left_sub_size, "--");
        snprintf(right_main, right_main_size, "--");
        snprintf(right_sub, right_sub_size, "--");
        return;
    }

    match_watch_app_make_team_badge_code(match, true, home_code, sizeof(home_code));
    match_watch_app_make_team_badge_code(match, false, away_code, sizeof(away_code));
    match_watch_app_get_home_timing(match, &timing);

    if (timing.phase == MATCH_WATCH_HOME_PHASE_COUNTDOWN) {
        int64_t minutes = timing.remain_s / 60;
        int64_t seconds = timing.remain_s % 60;
        gfx_color_t host_color = match_watch_app_host_team_color(match, has_match);
        bool host_is_home = match_watch_app_host_is_home_side(match);
        gfx_color_t home_color = host_is_home ? host_color : MATCH_WATCH_COLOR_WHITE;
        gfx_color_t away_color = host_is_home ? MATCH_WATCH_COLOR_WHITE : host_color;
        char home_hex[8];
        char away_hex[8];

        match_watch_app_color_to_inline_hex(home_color, home_hex, sizeof(home_hex));
        match_watch_app_color_to_inline_hex(away_color, away_hex, sizeof(away_hex));
        snprintf(top, top_size, "##0x%s %s ##0xFFFFFF VS ##0x%s %s ##0x767676 COUNTDOWN",
                 home_hex, home_code, away_hex, away_code);
        snprintf(left_main, left_main_size, "%02lld", (long long)minutes);
        snprintf(left_sub, left_sub_size, "MINUTE");
        snprintf(right_main, right_main_size, "%02lld", (long long)seconds);
        snprintf(right_sub, right_sub_size, "SECOND");
        return;
    }

    if (timing.phase == MATCH_WATCH_HOME_PHASE_FAR) {
        if (detail_page) {
            snprintf(top, top_size, "UPCOMING");
        } else {
            match_watch_app_format_card_kickoff(match, top, top_size);
        }
        snprintf(left_main, left_main_size, "%s", home_code);
        snprintf(right_main, right_main_size, "%s", away_code);
        left_sub[0] = '\0';
        right_sub[0] = '\0';
        return;
    }

    if (timing.phase == MATCH_WATCH_HOME_PHASE_FINISHED) {
        match_watch_app_parse_result_score(match->score_label, &home_score, &away_score);
        match_watch_app_format_score_fields(match, left_main, left_main_size, right_main, right_main_size,
                                            &home_score, &away_score);
        if (detail_page) {
            snprintf(top, top_size, "REVIEW");
            snprintf(left_sub, left_sub_size, "%s", home_code);
            snprintf(right_sub, right_sub_size, "%s", away_code);
        } else {
            match_watch_app_format_finished_top(match, home_score, away_score, top, top_size);
            snprintf(left_sub, left_sub_size, "%s", home_code);
            snprintf(right_sub, right_sub_size, "%s", away_code);
        }
        return;
    }

    match_watch_app_format_live_top(match, timing, detail_page, top, top_size);
    snprintf(left_main, left_main_size, "%s", home_code);
    snprintf(right_main, right_main_size, "%s", away_code);
    left_sub[0] = '\0';
    right_sub[0] = '\0';
}

static bool match_watch_app_text_has_inline_colors(const char *text)
{
    return text != NULL && text[0] == '#' && text[1] == '#' && text[2] == '0' &&
           (text[3] == 'x' || text[3] == 'X');
}

void match_watch_app_set_card_texts(const char *top, const char *left_main,
                                        const char *left_sub, const char *right_main,
                                        const char *right_sub,
                                        const match_data_schedule_item_t *match, bool has_match)
{
    bool countdown_subs = match_watch_app_is_countdown_subs(left_sub, right_sub);
    bool finished_result_top = match_watch_app_is_finished_result_top(top);
    bool left_main_visible = left_main != NULL && left_main[0] != '\0';
    bool right_main_visible = right_main != NULL && right_main[0] != '\0';
    bool left_sub_visible = left_sub != NULL && left_sub[0] != '\0';
    bool right_sub_visible = right_sub != NULL && right_sub[0] != '\0';
    gfx_color_t host_team_color = match_watch_app_host_team_color(match, has_match);
    bool host_is_home = match_watch_app_host_is_home_side(match);
    gfx_color_t host_fg = match_watch_platform_color(host_team_color);
    gfx_color_t rival_fg = match_watch_platform_color(MATCH_WATCH_COLOR_WHITE);
    gfx_color_t left_fg = host_is_home ? host_fg : rival_fg;
    gfx_color_t right_fg = host_is_home ? rival_fg : host_fg;

    if (match_watch_platform_lock() == ESP_OK) {
        (void)gfx_label_set_color(s_app->ui.card_left_main, left_fg);
        (void)gfx_label_set_color(s_app->ui.card_left_sub, left_fg);
        (void)gfx_label_set_color(s_app->ui.card_right_main, right_fg);
        (void)gfx_label_set_color(s_app->ui.card_right_sub, right_fg);
        if (s_app->ui.card_top != NULL) {
            uint32_t screen_w = gfx_disp_get_hor_res(disp_default);

            match_watch_app_apply_card_score_layout(finished_result_top, screen_w);
            if (finished_result_top) {
                (void)gfx_obj_set_size(s_app->ui.card_top, (uint16_t)screen_w, MATCH_WATCH_CARD_TOP_FINISHED_H);
                (void)gfx_obj_set_pos(s_app->ui.card_top, 0, MATCH_WATCH_CARD_TOP_FINISHED_Y);
                (void)gfx_label_set_font(s_app->ui.card_top, (gfx_font_t)&font_match_bogle_32_4);
                (void)gfx_label_set_color(s_app->ui.card_top, host_fg);
            } else if (countdown_subs || match_watch_app_text_has_inline_colors(top)) {
                (void)gfx_obj_set_size(s_app->ui.card_top, (uint16_t)screen_w, 24);
                (void)gfx_obj_set_pos(s_app->ui.card_top, 0, 5);
                (void)gfx_label_set_font(s_app->ui.card_top, (gfx_font_t)&font_match_bogle_20_4);
                (void)gfx_label_set_color(s_app->ui.card_top,
                                          match_watch_platform_color(MATCH_WATCH_COLOR_TOP_MUTED));
                (void)gfx_label_set_text_align(s_app->ui.card_top, GFX_TEXT_ALIGN_CENTER);
            } else {
                (void)gfx_obj_set_size(s_app->ui.card_top, 220, 24);
                (void)gfx_obj_set_pos(s_app->ui.card_top, (gfx_coord_t)((screen_w - 220U) / 2U), 5);
                (void)gfx_label_set_font(s_app->ui.card_top, (gfx_font_t)&font_match_bogle_20_4);
                (void)gfx_label_set_color(s_app->ui.card_top,
                                          match_watch_platform_color(MATCH_WATCH_COLOR_TOP_MUTED));
            }
        }
        if (strcmp(s_app->ui.card_top_text, top) != 0) {
            (void)gfx_label_set_text(s_app->ui.card_top, top);
            strlcpy(s_app->ui.card_top_text, top, sizeof(s_app->ui.card_top_text));
        }
        match_watch_app_set_main_font_for_text(s_app->ui.card_left_main, left_main, !countdown_subs);
        if (strcmp(s_app->ui.card_left_main_text, left_main) != 0) {
            (void)gfx_label_set_text(s_app->ui.card_left_main, left_main);
            strlcpy(s_app->ui.card_left_main_text, left_main, sizeof(s_app->ui.card_left_main_text));
        }
        match_watch_app_set_sub_font_for_text(s_app->ui.card_left_sub, left_sub, countdown_subs);
        if (strcmp(s_app->ui.card_left_sub_text, left_sub) != 0) {
            (void)gfx_label_set_text(s_app->ui.card_left_sub, left_sub);
            strlcpy(s_app->ui.card_left_sub_text, left_sub, sizeof(s_app->ui.card_left_sub_text));
        }
        if (s_app->ui.card_center_text[0] != '\0') {
            (void)gfx_label_set_text(s_app->ui.card_center, "");
            s_app->ui.card_center_text[0] = '\0';
        }
        match_watch_app_set_main_font_for_text(s_app->ui.card_right_main, right_main, !countdown_subs);
        if (strcmp(s_app->ui.card_right_main_text, right_main) != 0) {
            (void)gfx_label_set_text(s_app->ui.card_right_main, right_main);
            strlcpy(s_app->ui.card_right_main_text, right_main, sizeof(s_app->ui.card_right_main_text));
        }
        match_watch_app_set_sub_font_for_text(s_app->ui.card_right_sub, right_sub, countdown_subs);
        if (strcmp(s_app->ui.card_right_sub_text, right_sub) != 0) {
            (void)gfx_label_set_text(s_app->ui.card_right_sub, right_sub);
            strlcpy(s_app->ui.card_right_sub_text, right_sub, sizeof(s_app->ui.card_right_sub_text));
        }
        if (s_app->ui.card_left_main != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_left_main, left_main_visible);
        }
        if (s_app->ui.card_left_sub != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_left_sub, left_sub_visible);
        }
        if (s_app->ui.card_right_main != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_right_main, right_main_visible);
        }
        if (s_app->ui.card_right_sub != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_right_sub, right_sub_visible);
        }
        match_watch_platform_unlock();
    }
}

void match_watch_app_set_card_center_text(const char *text)
{
    bool visible = text != NULL && text[0] != '\0';

    if (match_watch_platform_lock() == ESP_OK) {
        if (s_app->ui.card_center != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_center, visible);
            if (visible) {
                (void)gfx_label_set_font(s_app->ui.card_center, (gfx_font_t)&font_match_bogle_32_4);
            }
        }
        if (strcmp(s_app->ui.card_center_text, text != NULL ? text : "") != 0) {
            (void)gfx_label_set_text(s_app->ui.card_center, text != NULL ? text : "");
            strlcpy(s_app->ui.card_center_text, text != NULL ? text : "",
                    sizeof(s_app->ui.card_center_text));
        }
        match_watch_platform_unlock();
    }
}

bool match_watch_app_active_match_is_far(void)
{
    match_data_schedule_item_t match;
    bool waiting_for_data;
    match_watch_home_timing_t timing;
    bool has_match = match_watch_app_get_home_match_state(&match, &waiting_for_data);

    if (waiting_for_data || !has_match) {
        return false;
    }
    match_watch_app_get_home_timing(&match, &timing);
    return timing.phase == MATCH_WATCH_HOME_PHASE_FAR;
}

bool match_watch_app_should_show_time_home(void)
{
    if (match_watch_app_initial_match_hold_active()) {
        return false;
    }
    return match_watch_app_active_match_is_far() &&
           !match_watch_app_user_browse_hold_active() &&
           !match_watch_app_full_time_review_hold_active();
}

bool match_watch_app_initial_match_hold_active(void)
{
    uint32_t until_ms = s_app->runtime.initial_match_until_ms;
    uint32_t now_ms;

    if (until_ms == 0U) {
        return false;
    }
    now_ms = esp_log_timestamp();
    if ((int32_t)(until_ms - now_ms) > 0) {
        return true;
    }
    s_app->runtime.initial_match_until_ms = 0;
    return false;
}

void match_watch_app_refresh_home_card_with_match(const match_data_schedule_item_t *match,
                                                         bool has_match, bool waiting_for_data)
{
    char card_top[96];
    char card_left_main[16];
    char card_left_sub[32];
    char card_right_main[16];
    char card_right_sub[32];
    char card_center[16];
    match_watch_home_timing_t timing;

    match_watch_app_format_card(has_match ? match : NULL, has_match, waiting_for_data, false,
                                card_top, sizeof(card_top),
                                card_left_main, sizeof(card_left_main),
                                card_left_sub, sizeof(card_left_sub),
                                card_right_main, sizeof(card_right_main),
                                card_right_sub, sizeof(card_right_sub));
    match_watch_app_set_card_texts(card_top, card_left_main, card_left_sub,
                                   card_right_main, card_right_sub,
                                   has_match ? match : NULL, has_match && !waiting_for_data);
    if (has_match && !waiting_for_data && match != NULL) {
        match_watch_app_get_home_timing(match, &timing);
        if (timing.phase == MATCH_WATCH_HOME_PHASE_LIVE) {
            match_watch_app_format_center_score(match, card_center, sizeof(card_center));
            match_watch_app_set_card_center_text(card_center);
        } else {
            match_watch_app_set_card_center_text("");
        }
    } else {
        match_watch_app_set_card_center_text("");
    }
}

void match_watch_app_refresh_home_card(void)
{
    match_data_schedule_item_t match;
    bool waiting_for_data;
    bool has_match = match_watch_app_get_home_match_state(&match, &waiting_for_data);

    match_watch_app_refresh_home_card_with_match(&match, has_match, waiting_for_data);
}

void match_watch_app_set_detail_info_texts(const char *time_text, const char *meta_text, bool show_time)
{
    if (match_watch_platform_lock() == ESP_OK) {
        (void)gfx_obj_set_visible(s_app->ui.detail_time, show_time);
        if (strcmp(s_app->ui.detail_time_text, time_text) != 0) {
            (void)gfx_label_set_text(s_app->ui.detail_time, time_text);
            strlcpy(s_app->ui.detail_time_text, time_text, sizeof(s_app->ui.detail_time_text));
        }
        if (strcmp(s_app->ui.detail_meta_text, meta_text) != 0) {
            (void)gfx_label_set_text(s_app->ui.detail_meta, meta_text);
            strlcpy(s_app->ui.detail_meta_text, meta_text, sizeof(s_app->ui.detail_meta_text));
        }
        match_watch_platform_unlock();
    }
}

void match_watch_app_refresh_detail_card(const match_data_schedule_item_t *match,
                                                bool has_match, bool waiting_for_data)
{
    char card_top[96];
    char card_left_main[16];
    char card_left_sub[32];
    char card_right_main[16];
    char card_right_sub[32];
    char kickoff[24];
    char meta_en[96];
    bool show_kickoff;

    match_watch_app_format_card(has_match ? match : NULL, has_match, waiting_for_data, true,
                                card_top, sizeof(card_top),
                                card_left_main, sizeof(card_left_main),
                                card_left_sub, sizeof(card_left_sub),
                                card_right_main, sizeof(card_right_main),
                                card_right_sub, sizeof(card_right_sub));

    if (waiting_for_data || !has_match || match == NULL) {
        match_watch_app_set_card_texts(card_top, card_left_main, card_left_sub,
                                       card_right_main, card_right_sub,
                                       NULL, false);
        match_watch_app_set_card_center_text("VS");
        match_watch_app_set_detail_info_texts("--.-- --:--", "WAITING", true);
        return;
    }

    show_kickoff = match->state == MATCH_DATA_MATCH_UPCOMING;
    if (show_kickoff) {
        match_watch_app_format_detail_time(match, kickoff, sizeof(kickoff));
    } else {
        kickoff[0] = '\0';
    }
    match_watch_app_format_detail_meta(match, meta_en, sizeof(meta_en));
    match_watch_app_set_card_texts(card_top, card_left_main, card_left_sub,
                                   card_right_main, card_right_sub,
                                   match, true);
    match_watch_app_set_card_center_text("VS");
    match_watch_app_set_detail_info_texts(kickoff, meta_en, show_kickoff);
}

void match_watch_app_refresh_home_live_state(void)
{
    match_data_schedule_item_t match;
    bool waiting_for_data;
    bool has_match = match_watch_app_get_home_match_state(&match, &waiting_for_data);
    const char *action;
    match_watch_home_timing_t timing;

    match_watch_app_get_home_timing(has_match ? &match : NULL, &timing);
    action = match_watch_pet_update_action(&s_app->pet.logic, &match_watch_app_pet_policy,
                                           has_match ? &match : NULL, has_match, waiting_for_data,
                                           timing, s_app->selection.host_team);

    if (s_app->runtime.active_page == MATCH_WATCH_PAGE_TIME_HOME) {
        char time_text[32];

        match_watch_app_format_beijing_time(time_text, sizeof(time_text));
        if (match_watch_platform_lock() == ESP_OK) {
            if (strcmp(s_app->ui.card_top_text, time_text) != 0) {
                (void)gfx_label_set_text(s_app->ui.card_top, time_text);
                strlcpy(s_app->ui.card_top_text, time_text, sizeof(s_app->ui.card_top_text));
            }
            match_watch_platform_unlock();
        }
        (void)action;
        return;
    } else {
        match_watch_app_refresh_home_card_with_match(&match, has_match, waiting_for_data);
    }
    (void)match_watch_pet_set_action(&s_app->pet.handle, action, true);
}

void match_watch_app_render_page(match_watch_page_t page)
{
    bool locked;

    if (page == MATCH_WATCH_PAGE_TIME_HOME &&
            match_data_has_live_data() &&
            !match_watch_app_initial_match_hold_active() &&
            !match_watch_app_active_match_is_far()) {
        page = MATCH_WATCH_PAGE_TEAM;
    }

    ESP_LOGV(TAG, "render page start: page=%d live=%d sync_done=%d host=%s",
             page, match_data_has_live_data(), s_app->runtime.net_sync_done,
             s_app->selection.host_team[0] != '\0' ? s_app->selection.host_team : "<none>");

    locked = match_watch_platform_lock() == ESP_OK;
    if (!locked) {
        ESP_LOGW(TAG, "render page skipped: display lock failed");
        return;
    }
    s_app->runtime.active_page = page;
    switch (page) {
    case MATCH_WATCH_PAGE_TIME_HOME: {
        match_data_schedule_item_t match;
        bool waiting_for_data;
        bool has_match = match_watch_app_get_home_match_state(&match, &waiting_for_data);
        match_watch_home_timing_t timing;
        char time_text[32];

        match_watch_app_apply_time_home_layout();
        match_watch_app_place_pet_for_page(MATCH_WATCH_PAGE_TIME_HOME);
        match_watch_app_set_pet_visible(true);
        match_watch_app_set_detail_info_visible(false);
        if (s_app->ui.card_left_main != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_left_main, false);
        }
        if (s_app->ui.card_left_sub != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_left_sub, false);
        }
        if (s_app->ui.card_center != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_center, false);
        }
        if (s_app->ui.card_right_main != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_right_main, false);
        }
        if (s_app->ui.card_right_sub != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_right_sub, false);
        }
        if (s_app->ui.card_top != NULL) {
            (void)gfx_obj_set_visible(s_app->ui.card_top, true);
        }
        match_watch_app_get_home_timing(has_match ? &match : NULL, &timing);
        const char *action = match_watch_pet_update_action(&s_app->pet.logic, &match_watch_app_pet_policy,
                                                           has_match ? &match : NULL, has_match,
                                                           waiting_for_data, timing, s_app->selection.host_team);
        match_watch_app_format_beijing_time(time_text, sizeof(time_text));
        if (strcmp(s_app->ui.card_top_text, time_text) != 0) {
            (void)gfx_label_set_text(s_app->ui.card_top, time_text);
            strlcpy(s_app->ui.card_top_text, time_text, sizeof(s_app->ui.card_top_text));
        }
        match_watch_platform_unlock();
        (void)match_watch_pet_set_action(&s_app->pet.handle, action, true);
        return;
    }
    case MATCH_WATCH_PAGE_TEAM: {
        match_data_schedule_item_t match;
        bool waiting_for_data;
        bool has_match = match_watch_app_get_home_match_state(&match, &waiting_for_data);
        match_watch_home_timing_t timing;

        ESP_LOGV(TAG, "render team: host=%s has_match=%d",
                 s_app->selection.host_team[0] != '\0' ? s_app->selection.host_team : "<none>", has_match);
        match_watch_app_apply_home_card_layout();
        match_watch_app_place_pet_for_page(page);
        match_watch_app_set_pet_visible(true);
        match_watch_app_set_detail_info_visible(false);
        match_watch_app_get_home_timing(has_match ? &match : NULL, &timing);
        const char *action = match_watch_pet_update_action(&s_app->pet.logic, &match_watch_app_pet_policy,
                                                           has_match ? &match : NULL, has_match,
                                                           waiting_for_data, timing, s_app->selection.host_team);
        ESP_LOGV(TAG, "render team text/action ready: action=%s", action);
        match_watch_app_refresh_home_card();
        match_watch_app_set_card_visible(true);
        ESP_LOGV(TAG, "render team set action: %s", action);
        ESP_LOGV(TAG, "render page done: team");
        match_watch_platform_unlock();
        (void)match_watch_pet_set_action(&s_app->pet.handle, action, true);
        return;
    }
    case MATCH_WATCH_PAGE_DETAIL: {
        match_data_schedule_item_t match;
        bool waiting_for_data;
        bool has_match = match_watch_app_get_home_match_state(&match, &waiting_for_data);

        match_watch_app_apply_detail_page_layout();
        match_watch_app_set_pet_visible(false);
        match_watch_app_set_detail_info_visible(true);
        match_watch_app_refresh_detail_card(&match, has_match, waiting_for_data);
        match_watch_app_set_card_visible(true);
        match_watch_platform_unlock();
        return;
    }
    default:
        match_watch_platform_unlock();
        match_watch_app_render_page(MATCH_WATCH_PAGE_TEAM);
        return;
    }
}
