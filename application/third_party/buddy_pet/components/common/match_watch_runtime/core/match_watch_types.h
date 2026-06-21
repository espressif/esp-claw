/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "freertos/queue.h"
#include "gfx.h"
#include "match_watch_home_state.h"
#include "match_watch_pet.h"
#include "match_data.h"
#include "match_watch_runtime.h"
#include "match_watch_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MATCH_WATCH_COLOR_BG                 GFX_COLOR_HEX(0x000000)
#define MATCH_WATCH_COLOR_ACCENT             GFX_COLOR_HEX(0xFCDB02)
#define MATCH_WATCH_COLOR_WHITE              GFX_COLOR_HEX(0xFFFFFF)
#define MATCH_WATCH_COLOR_TEXT               GFX_COLOR_HEX(0xE7EDF2)
#define MATCH_WATCH_COLOR_TEXT_MUTED         GFX_COLOR_HEX(0xD0D9E0)
#define MATCH_WATCH_COLOR_TOP_MUTED          GFX_COLOR_HEX(0x767676)
#define MATCH_WATCH_COLOR_META_MUTED         GFX_COLOR_HEX(0x787878)
#define MATCH_WATCH_COLOR_TEAM_BLUE          GFX_COLOR_HEX(0x326FFF)
#define MATCH_WATCH_COLOR_TEAM_CYAN          GFX_COLOR_HEX(0x78DAEC)
#define MATCH_WATCH_COLOR_TEAM_GREEN         GFX_COLOR_HEX(0x1D9E75)
#define MATCH_WATCH_COLOR_TEAM_RED           GFX_COLOR_HEX(0xF04D4D)
#define MATCH_WATCH_TEAM_OPTION_MAX          48
#define MATCH_WATCH_HOST_NVS_NAMESPACE       "match_watch"
#define MATCH_WATCH_HOST_NVS_KEY             "host_team"
#define MATCH_WATCH_HOST_SOURCE_NVS_KEY      "host_source"
#define MATCH_WATCH_LONG_PRESS_MS            1200U
#define MATCH_WATCH_LONG_PRESS_MOVE_PX       12
#define MATCH_WATCH_PET_LIMITED_MS           5000U
#define MATCH_WATCH_PET_RANDOM_MIN_MS        3000U
#define MATCH_WATCH_PET_RANDOM_SPAN_MS       5000U
#define MATCH_WATCH_PET_RANDOM_HOLD_MS       3000U
#define MATCH_WATCH_PET_WAIT_WAVE_MIN_MS     4000U
#define MATCH_WATCH_PET_WAIT_WAVE_SPAN_MS    6000U
#define MATCH_WATCH_PET_WAIT_WAVE_HOLD_MS    2500U
#define MATCH_WATCH_PET_LIVE_RUN_MIN_MS      2500U
#define MATCH_WATCH_PET_LIVE_RUN_SPAN_MS     3500U
#define MATCH_WATCH_TEAM_COL_W               128U
#define MATCH_WATCH_FULL_TIME_REVIEW_MS      30000U
#define MATCH_WATCH_LOOP_DELAY_MS            100U
#define MATCH_WATCH_USER_BROWSE_HOLD_MS      20000U
#define MATCH_WATCH_EXTERNAL_EVENT_QUEUE_LEN 16U
#define MATCH_WATCH_EXTERNAL_TEXT_LEN        64U
typedef enum {
    MATCH_WATCH_PAGE_TIME_HOME = 0,
    MATCH_WATCH_PAGE_TEAM,
    MATCH_WATCH_PAGE_DETAIL,
} match_watch_page_t;

typedef enum {
    MATCH_WATCH_HOST_TEAM_SOURCE_NONE = 0,
    MATCH_WATCH_HOST_TEAM_SOURCE_DEFAULT,
    MATCH_WATCH_HOST_TEAM_SOURCE_PET,
    MATCH_WATCH_HOST_TEAM_SOURCE_USER,
    MATCH_WATCH_HOST_TEAM_SOURCE_PROVIDER,
} match_watch_host_team_source_t;

typedef struct {
    match_watch_page_t active_page;
    bool net_sync_done;
    bool initial_data_hold_done;
    uint32_t initial_match_until_ms;
    bool running;
    bool should_stop;
    bool close_requested;
    match_watch_data_source_t data_source;
    bool reminders_enabled;
    char notify_channel[24];
    char notify_chat_id[96];
} match_watch_app_runtime_state_t;

typedef struct {
    match_watch_pet_t handle;
    match_watch_pet_logic_t logic;
} match_watch_app_pet_state_t;

typedef struct {
    gfx_obj_t *card_top;
    gfx_obj_t *card_left_main;
    gfx_obj_t *card_left_sub;
    gfx_obj_t *card_center;
    gfx_obj_t *card_right_main;
    gfx_obj_t *card_right_sub;
    gfx_obj_t *detail_time;
    gfx_obj_t *detail_meta;
    char card_top_text[96];
    char card_left_main_text[16];
    char card_left_sub_text[32];
    char card_right_main_text[16];
    char card_right_sub_text[32];
    char card_center_text[32];
    char detail_time_text[32];
    char detail_meta_text[96];
    uint32_t left_team_color_key;
    gfx_color_t left_team_color;
    bool left_team_color_valid;
} match_watch_app_ui_state_t;

typedef struct {
    char host_team[MATCH_DATA_TEAM_NAME_LEN];
    char competition[48];
    bool host_team_selected;
    bool host_team_loaded;
    match_watch_host_team_source_t host_team_source;
    size_t host_team_index;
    match_data_stage_t selected_stage;
    uint16_t selected_match_no;
    uint32_t user_browse_until_ms;
} match_watch_app_selection_state_t;

typedef struct {
    uint16_t match_no;
    match_watch_home_phase_t phase;
    int64_t real_base_ms;
    int64_t value_base;
    uint16_t live_minute;
} match_watch_app_timing_state_t;

typedef struct {
    uint16_t match_no;
    match_data_match_state_t state;
    int home_score;
    int away_score;
    uint32_t reminder_mask;
    uint32_t last_goal_total;
    bool kickoff_sent;
    bool half_time_sent;
    bool full_time_sent;
    uint32_t full_time_review_until_ms;
    bool match_switched_sent;
} match_watch_app_notify_state_t;

typedef struct {
    char values[MATCH_WATCH_TEAM_OPTION_MAX][MATCH_DATA_TEAM_NAME_LEN];
    size_t count;
} match_watch_app_team_options_t;

typedef struct {
    bool pressed;
    bool pet_touch_active;
    bool pet_touch_suppress;
    bool swipe_seen;
    int32_t press_x;
    int32_t press_y;
    int32_t max_move;
    uint32_t press_time_ms;
} match_watch_app_touch_state_t;

typedef enum {
    MATCH_WATCH_EXTERNAL_EVENT_CLOSE = 0,
    MATCH_WATCH_EXTERNAL_EVENT_DATA_CHANGED,
    MATCH_WATCH_EXTERNAL_EVENT_PET_RELOAD,
    MATCH_WATCH_EXTERNAL_EVENT_FAVORITE_TEAM,
    MATCH_WATCH_EXTERNAL_EVENT_DATA_SOURCE,
    MATCH_WATCH_EXTERNAL_EVENT_REMINDERS,
} match_watch_app_external_event_type_t;

typedef struct {
    match_watch_app_external_event_type_t type;
    char text[MATCH_WATCH_EXTERNAL_TEXT_LEN];
    match_watch_data_source_t data_source;
    match_watch_host_team_source_t host_team_source;
    bool enabled;
} match_watch_app_external_event_t;

typedef struct {
    QueueHandle_t queue;
} match_watch_app_external_state_t;

typedef struct {
    match_watch_app_runtime_state_t runtime;
    match_watch_app_pet_state_t pet;
    match_watch_app_ui_state_t ui;
    match_watch_app_selection_state_t selection;
    match_watch_app_timing_state_t timing;
    match_watch_app_team_options_t team_options;
    match_watch_app_touch_state_t touch;
    match_watch_app_notify_state_t notify;
    match_watch_app_external_state_t external;
} match_watch_app_ctx_t;
