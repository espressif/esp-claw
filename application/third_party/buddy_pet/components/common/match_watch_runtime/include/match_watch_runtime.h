/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file match_watch_runtime.h
 * @brief Match Watch component public contract (start, status, notifications).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MATCH_WATCH_RUNTIME_TEAM_NAME_LEN 32

/** @brief Schedule data source for Match Watch UI. */
typedef enum {
    MATCH_WATCH_DATA_SOURCE_LIVE = 0,     /**< Provider-pushed live schedule. */
    MATCH_WATCH_DATA_SOURCE_EXTERNAL,     /**< Embedded or static schedule data. */
} match_watch_data_source_t;

/** @brief Read-only runtime status snapshot for capabilities and tools. */
typedef struct {
    bool running;                               /**< UI main loop is active. */
    bool reminders_enabled;                     /**< Match reminders are enabled. */
    match_watch_data_source_t data_source;      /**< Active schedule data source. */
    char favorite_team[MATCH_WATCH_RUNTIME_TEAM_NAME_LEN]; /**< Selected favorite team name. */
    char favorite_team_source[16];              /**< Selection source: default, pet, user, provider, or none. */
    char active_competition[48];                /**< Active competition label or slug. */
    char notify_channel[24];                    /**< Outbound notification channel. */
    char notify_chat_id[96];                    /**< Outbound notification chat id. */
} match_watch_runtime_status_t;

/** @brief Optional open-time settings applied before the UI task starts. */
typedef struct {
    const char *team;                           /**< Favorite team, or NULL to leave unchanged. */
    const char *competition;                    /**< Competition label, or NULL to leave unchanged. */
    const char *favorite_source;                /**< Optional source label: pet, user, provider, or default. */
    bool reminders_set;                         /**< Whether `reminders_enabled` should be applied. */
    bool reminders_enabled;                     /**< Reminder state used when `reminders_set` is true. */
} match_watch_runtime_open_config_t;

/** @brief Match reminder notification delivered to the capability layer. */
typedef struct {
    uint16_t match_no;                          /**< Related match number. */
    match_watch_data_source_t source;           /**< Data source when the event fired. */
    char kind[24];                              /**< Event kind, for example goal, kickoff. */
    char message[160];                          /**< Human-readable notification text. */
    char channel[24];                           /**< Delivery channel. */
    char chat_id[96];                           /**< Delivery chat id. */
} match_watch_runtime_notification_t;

/**
 * @brief Callback for match reminder notifications.
 *
 * @param notification Notification payload.
 * @param user_data Opaque pointer registered with the callback.
 */
typedef void (*match_watch_runtime_notification_cb_t)(const match_watch_runtime_notification_t *notification,
                                                      void *user_data);

/** @brief Callback invoked after external match data is pushed successfully. */
typedef void (*match_watch_runtime_push_success_cb_t)(void *user_data);
typedef esp_err_t (*match_watch_runtime_provider_run_cb_t)(const char *path,
                                                           const char *args_json,
                                                           uint32_t timeout_ms,
                                                           const char *name,
                                                           const char *exclusive,
                                                           bool replace_existing,
                                                           char *output,
                                                           size_t output_size);
typedef esp_err_t (*match_watch_runtime_provider_stop_cb_t)(const char *exclusive,
                                                            uint32_t wait_ms,
                                                            char *output,
                                                            size_t output_size);

/**
 * @brief Open Match Watch UI in its runtime task.
 *
 * The runtime owns scene arbitration, task lifecycle, and the blocking app main
 * loop. If Match Watch is already running, this returns ESP_OK and sets
 * `already_running` to true when the pointer is provided.
 *
 * @return ESP_OK on success, otherwise an error code.
 */
esp_err_t match_watch_runtime_open(const match_watch_runtime_open_config_t *config,
                                   bool *already_running);

/**
 * @brief Request Match Watch UI close and wait for the runtime task to exit.
 *
 * @return ESP_OK if closed or already idle, otherwise an error code.
 */
esp_err_t match_watch_runtime_close(void);

/**
 * @brief Return true while Match Watch is running or its runtime task exists.
 */
bool match_watch_runtime_is_running(void);

esp_err_t match_watch_runtime_set_favorite(const char *team, const char *competition);
esp_err_t match_watch_runtime_set_favorite_with_source(const char *team,
                                                       const char *competition,
                                                       const char *source);
esp_err_t match_watch_runtime_set_competition(const char *competition);
esp_err_t match_watch_runtime_set_reminders(bool enabled);
esp_err_t match_watch_runtime_set_notify_target(const char *channel, const char *chat_id);
esp_err_t match_watch_runtime_push_data_json(const char *input_json);
esp_err_t match_watch_runtime_get_status(match_watch_runtime_status_t *out_status);
esp_err_t match_watch_runtime_set_notification_callback(match_watch_runtime_notification_cb_t cb,
                                                        void *user_data);
esp_err_t match_watch_runtime_set_push_success_callback(match_watch_runtime_push_success_cb_t cb,
                                                        void *user_data);

#define MATCH_WATCH_RUNTIME_PROVIDER_STOP_WAIT_MS 4000

esp_err_t match_watch_runtime_provider_set_backend(match_watch_runtime_provider_run_cb_t run_cb,
                                                   match_watch_runtime_provider_stop_cb_t stop_cb);
esp_err_t match_watch_runtime_provider_start(const char *path,
                                             const char *args_json,
                                             const char *name,
                                             char *output,
                                             size_t output_size);
esp_err_t match_watch_runtime_provider_stop(uint32_t wait_ms, char *output, size_t output_size);
esp_err_t match_watch_runtime_provider_status(char *output, size_t output_size);
void match_watch_runtime_provider_record_error(const char *message);
void match_watch_runtime_provider_record_push_success(void *user_data);

#ifdef __cplusplus
}
#endif
