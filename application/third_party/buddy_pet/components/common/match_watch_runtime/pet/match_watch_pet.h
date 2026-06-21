/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file match_watch_pet.h
 * @brief Match Watch pet action selection policy.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "gfx.h"
#include "pet_buddy.h"
#include "pet_host.h"
#include "match_watch_home_state.h"
#include "match_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Pet action aliases mapped to Pet Buddy action names. */
#define MATCH_WATCH_PET_ACTION_IDLE          PET_BUDDY_ACTION_IDLE
#define MATCH_WATCH_PET_ACTION_RUNNING       PET_BUDDY_ACTION_RUNNING
#define MATCH_WATCH_PET_ACTION_RUNNING_LEFT  PET_BUDDY_ACTION_RUNNING_LEFT
#define MATCH_WATCH_PET_ACTION_RUNNING_RIGHT PET_BUDDY_ACTION_RUNNING_RIGHT
#define MATCH_WATCH_PET_ACTION_WAVE          PET_BUDDY_ACTION_WAVE
#define MATCH_WATCH_PET_ACTION_JUMP          PET_BUDDY_ACTION_JUMP
#define MATCH_WATCH_PET_ACTION_LOSE          PET_BUDDY_ACTION_LOSE
#define MATCH_WATCH_PET_ACTION_SLEEP         PET_BUDDY_ACTION_SLEEP
#define MATCH_WATCH_PET_ACTION_REVIEW        PET_BUDDY_ACTION_REVIEW

#define MATCH_WATCH_PET_PAGE_HOME   PET_HOST_PAGE_HOME
#define MATCH_WATCH_PET_PAGE_DETAIL PET_HOST_PAGE_DETAIL

typedef pet_host_page_t match_watch_pet_page_t;
typedef pet_host_t match_watch_pet_t;

/** @brief Timing policy for temporary pet actions. */
typedef struct {
    uint32_t limited_ms;      /**< Duration for goal/lose limited actions. */
    uint32_t random_min_ms;     /**< Minimum delay before next random action. */
    uint32_t random_span_ms;    /**< Random delay range for idle random actions. */
    uint32_t random_hold_ms;    /**< How long a random action is held. */
    uint32_t wait_wave_min_ms;  /**< Minimum delay before next wait wave. */
    uint32_t wait_wave_span_ms; /**< Random delay range for wait wave. */
    uint32_t wait_wave_hold_ms; /**< How long wait wave is held. */
    uint32_t live_run_min_ms;   /**< Minimum delay before next live run action. */
    uint32_t live_run_span_ms;  /**< Random delay range for live run action. */
} match_watch_pet_policy_t;

/** @brief Runtime state used by pet action selection logic. */
typedef struct {
    const char *limited_action;   /**< Current temporary action, or NULL. */
    uint32_t limited_until_ms;  /**< Expiration time for limited_action. */
    uint16_t limited_match_no;  /**< Match number associated with limited_action. */
    int limited_home_score;     /**< Home score when limited_action started. */
    int limited_away_score;     /**< Away score when limited_action started. */
    uint16_t last_host_match_no; /**< Last seen match number for score tracking. */
    int last_host_score;        /**< Last seen host team score. */
    const char *random_action;  /**< Current random action, or NULL. */
    uint32_t random_next_ms;    /**< Next scheduled time for random action. */
    uint32_t random_until_ms;   /**< Expiration time for random_action. */
    uint32_t wait_wave_next_ms;  /**< Next scheduled time for wait wave. */
    uint32_t wait_wave_until_ms; /**< Expiration time for wait wave. */
    const char *live_run_action;  /**< Current live run action, or NULL. */
    uint32_t live_run_next_ms;    /**< Next scheduled time for live run action. */
} match_watch_pet_logic_t;

/**
 * @brief Open Match Watch pet renderer.
 *
 * @param pet Pet instance to initialize.
 * @param disp Target display.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t match_watch_pet_open(match_watch_pet_t *pet, gfx_disp_t *disp);

/**
 * @brief Close Match Watch pet renderer.
 *
 * @param pet Pet instance to close.
 */
void match_watch_pet_close(match_watch_pet_t *pet);

/**
 * @brief Set current pet action.
 *
 * @param pet Pet instance.
 * @param action Pet action name, for example idle, running, jumping.
 * @param keep_pos Keep current pet position when changing action.
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t match_watch_pet_set_action(match_watch_pet_t *pet, const char *action, bool keep_pos);

/**
 * @brief Show or hide pet object.
 *
 * @param pet Pet instance.
 * @param visible true to show, false to hide.
 */
void match_watch_pet_set_visible(match_watch_pet_t *pet, bool visible);

/**
 * @brief Place pet object for the current page layout.
 *
 * @param pet Pet instance.
 * @param page Target page layout.
 */
void match_watch_pet_place(match_watch_pet_t *pet, match_watch_pet_page_t page);

/**
 * @brief Forward touch events to pet renderer backend.
 *
 * @param pet Pet instance.
 * @param event GFX touch event.
 */
void match_watch_pet_handle_touch(match_watch_pet_t *pet, const gfx_touch_event_t *event);

/**
 * @brief Get underlying GFX animation object.
 *
 * @param pet Pet instance.
 * @return GFX object pointer, or NULL if unavailable.
 */
gfx_obj_t *match_watch_pet_object(match_watch_pet_t *pet);

/**
 * @brief Reset pet action selection runtime state.
 *
 * @param logic Pet logic state to reset.
 */
void match_watch_pet_logic_reset(match_watch_pet_logic_t *logic);

/**
 * @brief Pick base pet action from match state and timing.
 *
 * @param match Current match data, may be NULL.
 * @param host_team Favorite team name.
 * @param timing Current home timing phase.
 * @return Selected pet action name, or NULL if no action should be shown.
 */
const char *match_watch_pet_base_action(const match_data_schedule_item_t *match,
                                        const char *host_team,
                                        match_watch_home_timing_t timing);

/**
 * @brief Pick pet action for live score changes.
 *
 * @param logic Pet logic state.
 * @param match Current match data, may be NULL.
 * @param team Favorite team name.
 * @param finished Whether the match is finished.
 * @return Selected pet action name, or NULL if score action should not override base action.
 */
const char *match_watch_pet_live_score_action(match_watch_pet_logic_t *logic,
                                              const match_data_schedule_item_t *match,
                                              const char *team,
                                              bool finished);

/**
 * @brief Pick pet action from match state, policy, and timing.
 *
 * @param logic Pet logic state.
 * @param policy Pet timing policy.
 * @param match Current match data, may be NULL.
 * @param has_match Whether valid match data is available.
 * @param waiting_for_data Whether schedule data is not ready yet.
 * @param timing Current home timing phase.
 * @param base_action Base action selected before score override.
 * @param score_action Score-driven action override, may be NULL.
 * @return Final pet action name, or NULL if pet should stay hidden.
 */
const char *match_watch_pet_pick_action(match_watch_pet_logic_t *logic,
                                        const match_watch_pet_policy_t *policy,
                                        const match_data_schedule_item_t *match,
                                        bool has_match,
                                        bool waiting_for_data,
                                        match_watch_home_timing_t timing,
                                        const char *base_action,
                                        const char *score_action);

/**
 * @brief Update pet action after match or timing changes.
 *
 * @param logic Pet logic state.
 * @param policy Pet timing policy.
 * @param match Current match data, may be NULL.
 * @param has_match Whether valid match data is available.
 * @param waiting_for_data Whether schedule data is not ready yet.
 * @param timing Current home timing phase.
 * @param host_team Favorite team name.
 * @return Selected pet action name, or NULL if pet should stay hidden.
 */
const char *match_watch_pet_update_action(match_watch_pet_logic_t *logic,
                                          const match_watch_pet_policy_t *policy,
                                          const match_data_schedule_item_t *match,
                                          bool has_match,
                                          bool waiting_for_data,
                                          match_watch_home_timing_t timing,
                                          const char *host_team);

#ifdef __cplusplus
}
#endif
