/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "gfx.h"
#include "match_watch_types.h"
#include "match_data.h"
#include "match_watch_home_state.h"
#include "pet_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- ui_widgets --- */
void match_watch_app_style_label(gfx_obj_t *obj, gfx_color_t fg, gfx_color_t bg, bool bg_enable,
                                 gfx_text_align_t align, gfx_label_long_mode_t long_mode);
void match_watch_app_make_team_badge_code(const match_data_schedule_item_t *match, bool home_side,
                                          char *buf, size_t buf_size);
void match_watch_app_create_ui(gfx_disp_t *disp);
void match_watch_app_destroy_ui(void);

/* --- schedule --- */
void match_watch_app_reset_home_timing(void);
void match_watch_app_clear_host_team(bool persist);
void match_watch_app_set_host_team(const char *team, bool persist);
void match_watch_app_set_host_team_with_source(const char *team,
                                               bool persist,
                                               match_watch_host_team_source_t source);
const char *match_watch_app_host_team_source_name(match_watch_host_team_source_t source);
match_watch_host_team_source_t match_watch_app_host_team_source(void);
void match_watch_app_refresh_team_options(bool keep_current_host);
bool match_watch_app_pick_best_team_match(const char *team, match_data_schedule_item_t *out_match);
bool match_watch_app_should_switch_from_current(const match_data_schedule_item_t *current,
                                                const match_data_schedule_item_t *best);
bool match_watch_app_get_current_match(match_data_schedule_item_t *out_match);
bool match_watch_app_get_home_match_state(match_data_schedule_item_t *out_match, bool *waiting_for_data);
void match_watch_app_select_match(const match_data_schedule_item_t *match, bool persist);
bool match_watch_app_select_match_by_offset(int offset);
bool match_watch_app_select_stage_by_offset(int offset);
bool match_watch_app_select_next_host_match(match_data_schedule_item_t *out_match);
bool match_watch_app_can_select_stage_by_offset(int offset);
void match_watch_app_select_default_host_team(void);
void match_watch_app_note_user_browse(void);
bool match_watch_app_user_browse_hold_active(void);
bool match_watch_app_select_best_match_if_user_idle(void);
bool match_watch_app_match_is_finished(const match_data_schedule_item_t *match);
bool match_watch_app_full_time_review_hold_active(void);
void match_watch_app_cancel_full_time_review_hold(void);
void match_watch_app_sync_full_time_review(const match_data_schedule_item_t *match);
bool match_watch_app_apply_pet_profile(const pet_registry_entry_t *entry, bool persist);
bool match_watch_app_apply_selected_pet_theme(bool persist);
void match_watch_app_load_host_team(void);

/* --- ui --- */
void match_watch_app_render_page(match_watch_page_t page);
void match_watch_app_refresh_home_live_state(void);
bool match_watch_app_should_show_time_home(void);
bool match_watch_app_active_match_is_far(void);
bool match_watch_app_initial_match_hold_active(void);
void match_watch_app_set_detail_info_visible(bool visible);
void match_watch_app_set_card_visible(bool visible);
bool match_watch_app_parse_score(const char *score, int *home_score, int *away_score);
uint32_t match_watch_app_live_display_minute(const match_data_schedule_item_t *match);
void match_watch_app_get_home_timing(const match_data_schedule_item_t *match,
                                     match_watch_home_timing_t *timing);

/* --- notify --- */
void match_watch_app_check_notifications(void);

/* --- input --- */
void match_watch_touch_event_cb(gfx_touch_t *touch, const gfx_touch_event_t *event, void *user_data);

/* --- loop --- */
void match_watch_app_process_external_requests(void);
void match_watch_app_apply_data_source(match_watch_data_source_t source);
esp_err_t match_watch_app_reload_pet(void);

#ifdef __cplusplus
}
#endif
