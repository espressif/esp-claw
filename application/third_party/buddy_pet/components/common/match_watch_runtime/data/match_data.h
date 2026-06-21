/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

/**
 * @file match_data.h
 * @brief Match schedule storage, localization helpers, and live data accessors.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum length of team name fields. */
#define MATCH_DATA_TEAM_NAME_LEN      32
/** @brief Maximum length of group name field. */
#define MATCH_DATA_GROUP_NAME_LEN     8
/** @brief Maximum length of date label field. */
#define MATCH_DATA_DATE_LABEL_LEN     16
/** @brief Maximum length of time label field. */
#define MATCH_DATA_TIME_LABEL_LEN     16
/** @brief Maximum length of venue name field. */
#define MATCH_DATA_VENUE_NAME_LEN     48
/** @brief Maximum length of score label field. */
#define MATCH_DATA_SCORE_LABEL_LEN    16
/** @brief Maximum number of group-stage matches in live storage. */
#define MATCH_DATA_LIVE_GROUP_MAX     80
/** @brief Maximum number of knockout matches in live storage. */
#define MATCH_DATA_LIVE_KNOCKOUT_MAX  40

/** @brief Tournament stage for a schedule item. */
typedef enum {
    MATCH_DATA_STAGE_GROUP = 0,          /**< Group stage. */
    MATCH_DATA_STAGE_ROUND_OF_32,        /**< Round of 32. */
    MATCH_DATA_STAGE_ROUND_OF_16,        /**< Round of 16. */
    MATCH_DATA_STAGE_QUARTER_FINAL,      /**< Quarter final. */
    MATCH_DATA_STAGE_SEMI_FINAL,         /**< Semi final. */
    MATCH_DATA_STAGE_THIRD_PLACE,        /**< Third place playoff. */
    MATCH_DATA_STAGE_FINAL,              /**< Final. */
} match_data_stage_t;

/** @brief Runtime match state used by UI and pet logic. */
typedef enum {
    MATCH_DATA_MATCH_UPCOMING = 0,   /**< Not started yet. */
    MATCH_DATA_MATCH_LIVE,           /**< In progress. */
    MATCH_DATA_MATCH_GOAL,           /**< Goal just scored. */
    MATCH_DATA_MATCH_HALF_TIME,      /**< Half time break. */
    MATCH_DATA_MATCH_FULL_TIME,      /**< Full time finished. */
    MATCH_DATA_MATCH_PENALTY_WIN,    /**< Won on penalties. */
    MATCH_DATA_MATCH_LOST,           /**< Favorite team lost. */
} match_data_match_state_t;

/** @brief Single match entry in group or knockout schedule. */
typedef struct {
    uint16_t match_no;                                      /**< Match number. */
    match_data_stage_t stage;                               /**< Tournament stage. */
    uint8_t round;                                          /**< Round index within stage. */
    char group[MATCH_DATA_GROUP_NAME_LEN];                  /**< Group name, for example A. */
    char home[MATCH_DATA_TEAM_NAME_LEN];                    /**< Home team canonical name. */
    char away[MATCH_DATA_TEAM_NAME_LEN];                    /**< Away team canonical name. */
    char home_code[8];                                      /**< Home team short code. */
    char away_code[8];                                      /**< Away team short code. */
    char home_display[MATCH_DATA_TEAM_NAME_LEN];            /**< Home team display name. */
    char away_display[MATCH_DATA_TEAM_NAME_LEN];            /**< Away team display name. */
    char date_label[MATCH_DATA_DATE_LABEL_LEN];             /**< Date label for UI. */
    char time_label[MATCH_DATA_TIME_LABEL_LEN];             /**< Time label for UI. */
    char beijing_label[MATCH_DATA_DATE_LABEL_LEN + MATCH_DATA_TIME_LABEL_LEN]; /**< Beijing time label. */
    char venue[MATCH_DATA_VENUE_NAME_LEN];                  /**< Venue name. */
    char score_label[MATCH_DATA_SCORE_LABEL_LEN];           /**< Score text for UI. */
    uint32_t kickoff_ts;                                    /**< Kickoff Unix timestamp. */
    uint16_t live_minute;                                   /**< Live minute, 0 if unknown. */
    match_data_match_state_t state;                         /**< Current match state. */
} match_data_schedule_item_t;

/**
 * @brief Get localized display name for a tournament stage.
 *
 * @param stage Tournament stage.
 * @return Localized stage name string.
 */
const char *match_data_stage_name(match_data_stage_t stage);

/**
 * @brief Get short team code for UI badges.
 *
 * @param team Team name or placeholder token.
 * @return Team code, TBD for placeholders, or "---" if empty.
 */
const char *match_data_team_code(const char *team);

/**
 * @brief Get localized display name for a team.
 *
 * @param team Team name or placeholder token.
 * @return Localized team name, or empty string if unset.
 */
const char *match_data_localized_team_name(const char *team);

/**
 * @brief Get localized display name for a venue.
 *
 * @param venue Venue name.
 * @return Localized venue name, or empty string if unset.
 */
const char *match_data_localized_venue_name(const char *venue);

/**
 * @brief Get live group-stage schedule items.
 *
 * @param count Optional output item count.
 * @return Pointer to schedule array, or NULL if no live group data.
 */
const match_data_schedule_item_t *match_data_get_group_schedule(size_t *count);

/**
 * @brief Get live knockout schedule items.
 *
 * @param count Optional output item count.
 * @return Pointer to schedule array, or NULL if no live knockout data.
 */
const match_data_schedule_item_t *match_data_get_knockout_schedule(size_t *count);

/**
 * @brief Replace in-memory live schedule data.
 *
 * Copies provider-pushed items, sorts by kickoff time, and marks live data ready.
 *
 * @param group_items Group-stage items, may be NULL.
 * @param group_count Number of group items.
 * @param knockout_items Knockout items, may be NULL.
 * @param knockout_count Number of knockout items.
 */
void match_data_set_live_data(const match_data_schedule_item_t *group_items, size_t group_count,
                              const match_data_schedule_item_t *knockout_items, size_t knockout_count);

/**
 * @brief Check whether live schedule data has been loaded.
 *
 * @return true if at least one group or knockout item is stored.
 */
bool match_data_has_live_data(void);

/**
 * @brief Find the first schedule item that includes a team.
 *
 * Searches group stage first, then knockout.
 *
 * @param team Team name or code to match.
 * @param out_match Optional output match copy.
 * @return true if a matching item was found.
 */
bool match_data_find_team_match(const char *team, match_data_schedule_item_t *out_match);

/**
 * @brief Check whether a schedule item includes a team.
 *
 * Matches canonical name, code, or display name (case-insensitive).
 *
 * @param match Schedule item to inspect.
 * @param team Team name or code to match.
 * @return true if the team participates in the match.
 */
bool match_data_match_has_team(const match_data_schedule_item_t *match, const char *team);

#ifdef __cplusplus
}
#endif
