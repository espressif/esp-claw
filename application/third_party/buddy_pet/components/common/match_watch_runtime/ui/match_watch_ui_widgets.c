/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "gfx.h"
#include "match_watch_internal.h"
#include "match_watch_platform.h"

void match_watch_app_style_label(gfx_obj_t *obj, gfx_color_t fg, gfx_color_t bg, bool bg_enable,
                                 gfx_text_align_t align, gfx_label_long_mode_t long_mode)
{
    (void)gfx_label_set_font(obj, (gfx_font_t)&font_match_bogle_20_4);
    (void)gfx_label_set_color(obj, fg);
    (void)gfx_label_set_bg_color(obj, bg);
    (void)gfx_label_set_bg_enable(obj, bg_enable);
    (void)gfx_label_set_text_align(obj, align);
    (void)gfx_label_set_long_mode(obj, long_mode);
    (void)gfx_label_set_line_spacing(obj, 2);
}

void match_watch_app_make_team_badge_code(const match_data_schedule_item_t *match, bool home_side,
                                          char *buf, size_t buf_size)
{
    const char *team = home_side ? (match != NULL ? match->home : "") : (match != NULL ? match->away : "");
    const char *provided = home_side ? (match != NULL ? match->home_code : "") : (match != NULL ? match->away_code : "");
    const char *code = provided != NULL && provided[0] != '\0' ? provided : match_data_team_code(team);
    size_t count = 0;

    if (buf == NULL || buf_size == 0) {
        return;
    }
    buf[0] = '\0';
    if (code != NULL && strlen(code) <= 3) {
        strlcpy(buf, code, buf_size);
        return;
    }

    team = team != NULL ? team : "";
    for (const char *p = team; *p != '\0' && count < 3 && count + 1 < buf_size; p++) {
        bool word_start = (p == team || p[-1] == ' ' || p[-1] == '-' || p[-1] == '_');
        if (word_start && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) {
            char ch = *p;
            if (ch >= 'a' && ch <= 'z') {
                ch = (char)(ch - 'a' + 'A');
            }
            buf[count++] = ch;
        }
    }
    for (const char *p = team; *p != '\0' && count < 3 && count + 1 < buf_size; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
            char ch = *p;
            if (ch >= 'a' && ch <= 'z') {
                ch = (char)(ch - 'a' + 'A');
            }
            buf[count++] = ch;
        }
    }
    buf[count] = '\0';
    if (buf[0] == '\0') {
        strlcpy(buf, "---", buf_size);
    }
}

static void match_watch_app_create_label(gfx_obj_t **obj, gfx_disp_t *disp, uint16_t w, uint16_t h,
                                         gfx_coord_t x, gfx_coord_t y, gfx_color_t fg, gfx_color_t bg,
                                         bool bg_enable, gfx_text_align_t align)
{
    *obj = gfx_label_create(disp);
    assert(*obj != NULL);
    (void)gfx_obj_set_size(*obj, w, h);
    (void)gfx_obj_set_pos(*obj, x, y);
    match_watch_app_style_label(*obj, fg, bg, bg_enable, align, GFX_LABEL_LONG_WRAP);
}

static void match_watch_app_delete_obj(gfx_obj_t **obj)
{
    if (obj == NULL || *obj == NULL) {
        return;
    }
    (void)gfx_obj_delete(*obj);
    *obj = NULL;
}

static void match_watch_app_set_initial_fonts(void)
{
    (void)gfx_label_set_font(s_app->ui.card_top, (gfx_font_t)&font_match_bogle_20_4);
    (void)gfx_label_set_font(s_app->ui.card_left_main, (gfx_font_t)&font_match_bogle_70_4);
    (void)gfx_label_set_font(s_app->ui.card_right_main, (gfx_font_t)&font_match_bogle_70_4);
    (void)gfx_label_set_font(s_app->ui.card_left_sub, (gfx_font_t)&font_match_bogle_24_4);
    (void)gfx_label_set_font(s_app->ui.card_right_sub, (gfx_font_t)&font_match_bogle_24_4);
    (void)gfx_label_set_font(s_app->ui.card_center, (gfx_font_t)&font_match_bogle_20_4);
    (void)gfx_label_set_font(s_app->ui.detail_time, (gfx_font_t)&font_match_bogle_55_4);
    (void)gfx_label_set_font(s_app->ui.detail_meta, (gfx_font_t)&font_match_bogle_24_4);
}

void match_watch_app_destroy_ui(void)
{
    if (match_watch_platform_lock() != ESP_OK) {
        return;
    }

    match_watch_app_delete_obj(&s_app->ui.card_top);
    match_watch_app_delete_obj(&s_app->ui.card_left_main);
    match_watch_app_delete_obj(&s_app->ui.card_left_sub);
    match_watch_app_delete_obj(&s_app->ui.card_center);
    match_watch_app_delete_obj(&s_app->ui.card_right_main);
    match_watch_app_delete_obj(&s_app->ui.card_right_sub);
    match_watch_app_delete_obj(&s_app->ui.detail_time);
    match_watch_app_delete_obj(&s_app->ui.detail_meta);

    memset(s_app->ui.card_top_text, 0, sizeof(s_app->ui.card_top_text));
    memset(s_app->ui.card_left_main_text, 0, sizeof(s_app->ui.card_left_main_text));
    memset(s_app->ui.card_left_sub_text, 0, sizeof(s_app->ui.card_left_sub_text));
    memset(s_app->ui.card_right_main_text, 0, sizeof(s_app->ui.card_right_main_text));
    memset(s_app->ui.card_right_sub_text, 0, sizeof(s_app->ui.card_right_sub_text));
    memset(s_app->ui.card_center_text, 0, sizeof(s_app->ui.card_center_text));
    memset(s_app->ui.detail_time_text, 0, sizeof(s_app->ui.detail_time_text));
    memset(s_app->ui.detail_meta_text, 0, sizeof(s_app->ui.detail_meta_text));
    s_app->ui.left_team_color_valid = false;
    s_app->ui.left_team_color_key = 0;

    match_watch_platform_unlock();
}

void match_watch_app_create_ui(gfx_disp_t *disp)
{
    uint32_t screen_w = gfx_disp_get_hor_res(disp);

    (void)gfx_disp_set_bg_color(disp, match_watch_platform_color(MATCH_WATCH_COLOR_BG));

    match_watch_app_create_label(&s_app->ui.card_top, disp, 220, 24, (gfx_coord_t)((screen_w - 220U) / 2U), 5,
                                 match_watch_platform_color(MATCH_WATCH_COLOR_TOP_MUTED), match_watch_platform_color(MATCH_WATCH_COLOR_BG), false, GFX_TEXT_ALIGN_CENTER);
    match_watch_app_create_label(&s_app->ui.card_left_main, disp, 128, 82, 0, 28,
                                 match_watch_platform_color(MATCH_WATCH_COLOR_ACCENT), match_watch_platform_color(MATCH_WATCH_COLOR_BG), true, GFX_TEXT_ALIGN_CENTER);
    match_watch_app_create_label(&s_app->ui.card_left_sub, disp, 128, 30, 0, 96,
                                 match_watch_platform_color(MATCH_WATCH_COLOR_ACCENT), match_watch_platform_color(MATCH_WATCH_COLOR_BG), true, GFX_TEXT_ALIGN_CENTER);
    match_watch_app_create_label(&s_app->ui.card_center, disp, 60, 26, (gfx_coord_t)((screen_w - 60U) / 2U), 58,
                                 match_watch_platform_color(MATCH_WATCH_COLOR_META_MUTED), match_watch_platform_color(MATCH_WATCH_COLOR_BG), false, GFX_TEXT_ALIGN_CENTER);
    match_watch_app_create_label(&s_app->ui.card_right_main, disp, 128, 82, (gfx_coord_t)(screen_w - 128U), 28,
                                 match_watch_platform_color(MATCH_WATCH_COLOR_WHITE), match_watch_platform_color(MATCH_WATCH_COLOR_BG), false, GFX_TEXT_ALIGN_CENTER);
    match_watch_app_create_label(&s_app->ui.card_right_sub, disp, 128, 30, (gfx_coord_t)(screen_w - 128U), 96,
                                 match_watch_platform_color(MATCH_WATCH_COLOR_WHITE), match_watch_platform_color(MATCH_WATCH_COLOR_BG), false, GFX_TEXT_ALIGN_CENTER);
    match_watch_app_create_label(&s_app->ui.detail_time, disp, 280, 62, (gfx_coord_t)((screen_w - 280U) / 2U), 124,
                                 match_watch_platform_color(MATCH_WATCH_COLOR_WHITE), match_watch_platform_color(MATCH_WATCH_COLOR_BG), false, GFX_TEXT_ALIGN_CENTER);
    match_watch_app_create_label(&s_app->ui.detail_meta, disp, (uint16_t)screen_w, 30, 0, 184,
                                 match_watch_platform_color(MATCH_WATCH_COLOR_META_MUTED), match_watch_platform_color(MATCH_WATCH_COLOR_BG), false, GFX_TEXT_ALIGN_CENTER);
    match_watch_app_set_initial_fonts();
    (void)gfx_label_set_line_spacing(s_app->ui.card_left_sub, 0);
    (void)gfx_label_set_line_spacing(s_app->ui.card_right_sub, 0);
    (void)gfx_label_set_long_mode(s_app->ui.card_top, GFX_LABEL_LONG_CLIP);
    (void)gfx_label_set_long_mode(s_app->ui.card_left_main, GFX_LABEL_LONG_CLIP);
    (void)gfx_label_set_long_mode(s_app->ui.card_left_sub, GFX_LABEL_LONG_CLIP);
    (void)gfx_label_set_long_mode(s_app->ui.card_center, GFX_LABEL_LONG_CLIP);
    (void)gfx_label_set_long_mode(s_app->ui.card_right_main, GFX_LABEL_LONG_CLIP);
    (void)gfx_label_set_long_mode(s_app->ui.card_right_sub, GFX_LABEL_LONG_CLIP);
    (void)gfx_label_set_long_mode(s_app->ui.detail_time, GFX_LABEL_LONG_CLIP);
    (void)gfx_label_set_long_mode(s_app->ui.detail_meta, GFX_LABEL_LONG_CLIP);
    match_watch_app_set_detail_info_visible(false);
    match_watch_app_set_card_visible(true);
    match_watch_app_refresh_team_options(false);
}
