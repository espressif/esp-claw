/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "claw_task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "match_watch_app.h"
#include "match_watch_internal.h"
#include "match_watch_module.h"
#include "scene_runtime.h"

static const char *const TAG = "match_watch";

#define MATCH_WATCH_RUNTIME_SCENE_NAME    "match_watch"
#define MATCH_WATCH_RUNTIME_CLOSE_WAIT_MS 4000

typedef struct {
    scene_runtime_t scene;
    SemaphoreHandle_t lock;
    match_watch_runtime_push_success_cb_t push_success_cb;
    void *push_success_user_data;
} match_watch_runtime_state_t;

static match_watch_runtime_state_t s_match_watch_runtime;

static esp_err_t match_watch_app_post_external_event(const match_watch_app_external_event_t *event);

static esp_err_t match_watch_runtime_ensure_lock(void)
{
    if (s_match_watch_runtime.lock == NULL) {
        s_match_watch_runtime.lock = xSemaphoreCreateMutex();
    }
    return s_match_watch_runtime.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t match_watch_runtime_apply_open_config(const match_watch_runtime_open_config_t *config)
{
    esp_err_t ret;

    if (config == NULL) {
        return ESP_OK;
    }
    if (config->team != NULL && config->team[0] != '\0') {
        ret = match_watch_runtime_set_favorite_with_source(config->team,
                                                           config->competition,
                                                           config->favorite_source);
        if (ret != ESP_OK) {
            return ret;
        }
    } else if (config->competition != NULL && config->competition[0] != '\0') {
        ret = match_watch_runtime_set_competition(config->competition);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (config->reminders_set) {
        ret = match_watch_runtime_set_reminders(config->reminders_enabled);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}

static esp_err_t match_watch_runtime_task_create(const scene_runtime_task_config_t *config,
                                                 TaskFunction_t task_fn,
                                                 void *task_arg,
                                                 TaskHandle_t *out_task)
{
    BaseType_t ok;

    ESP_RETURN_ON_FALSE(config != NULL && task_fn != NULL && out_task != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid task args");
    ok = claw_task_create(&(claw_task_config_t) {
                              .name = config->name,
                              .stack_size = config->stack_size,
                              .priority = config->priority,
                              .core_id = tskNO_AFFINITY,
                              .stack_policy = CLAW_TASK_STACK_PREFER_PSRAM,
                          },
                          task_fn,
                          task_arg,
                          out_task);
    return ok == pdPASS && *out_task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void match_watch_runtime_task_delete(void)
{
    claw_task_delete(NULL);
}

static esp_err_t match_watch_runtime_scene_request_close(void *user_data)
{
    (void)user_data;
    return match_watch_app_request_close();
}

static esp_err_t match_watch_runtime_scene_run(void *arg)
{
    esp_err_t ret;

    (void)arg;

    ESP_LOGI(TAG, "starting match watch runtime");
    ret = match_watch_app_main();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "match watch runtime stopped with error: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t match_watch_runtime_open(const match_watch_runtime_open_config_t *config,
                                   bool *already_running)
{
    const scene_runtime_config_t scene_config = {
        .scene_name = MATCH_WATCH_RUNTIME_SCENE_NAME,
        .task_name = "match_watch",
        .task_stack_size = 1024 * 6,
        .task_priority = 5,
        .close_wait_ms = MATCH_WATCH_RUNTIME_CLOSE_WAIT_MS,
        .repeat_close_request = false,
        .prepare = NULL,
        .cleanup = NULL,
        .run = match_watch_runtime_scene_run,
        .request_close = match_watch_runtime_scene_request_close,
        .task_create = match_watch_runtime_task_create,
        .task_delete = match_watch_runtime_task_delete,
        .user_data = NULL,
    };
    bool scene_already_running = false;
    esp_err_t ret;

    if (already_running != NULL) {
        *already_running = false;
    }

    ESP_RETURN_ON_ERROR(match_watch_runtime_apply_open_config(config), TAG, "apply open config failed");
    ret = scene_runtime_open(&s_match_watch_runtime.scene, &scene_config, &scene_already_running);
    ESP_RETURN_ON_ERROR(ret, TAG, "open scene failed");
    if (scene_already_running && already_running != NULL) {
        *already_running = true;
    }
    return ESP_OK;
}

esp_err_t match_watch_runtime_close(void)
{
    return scene_runtime_close(&s_match_watch_runtime.scene);
}

bool match_watch_runtime_is_running(void)
{
    return scene_runtime_is_running(&s_match_watch_runtime.scene);
}

static match_watch_host_team_source_t match_watch_runtime_parse_favorite_source(const char *source)
{
    if (source == NULL || source[0] == '\0') {
        return MATCH_WATCH_HOST_TEAM_SOURCE_USER;
    }
    if (strcmp(source, "pet") == 0) {
        return MATCH_WATCH_HOST_TEAM_SOURCE_PET;
    }
    if (strcmp(source, "provider") == 0) {
        return MATCH_WATCH_HOST_TEAM_SOURCE_PROVIDER;
    }
    if (strcmp(source, "default") == 0) {
        return MATCH_WATCH_HOST_TEAM_SOURCE_DEFAULT;
    }
    return MATCH_WATCH_HOST_TEAM_SOURCE_USER;
}

esp_err_t match_watch_runtime_set_favorite_with_source(const char *team,
                                                       const char *competition,
                                                       const char *source)
{
    char canonical[MATCH_DATA_TEAM_NAME_LEN] = {0};
    match_watch_app_external_event_t event = {
        .type = MATCH_WATCH_EXTERNAL_EVENT_FAVORITE_TEAM,
        .host_team_source = match_watch_runtime_parse_favorite_source(source),
    };
    esp_err_t ret;

    if (team != NULL && team[0] != '\0') {
        if (strlen(team) >= MATCH_DATA_TEAM_NAME_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(canonical, team, sizeof(canonical));
    }

    ESP_RETURN_ON_ERROR(match_watch_app_ensure_external_queue(), TAG, "create external queue failed");

    if (!s_app->runtime.running) {
        if (canonical[0] != '\0') {
            match_watch_app_set_host_team_with_source(canonical, true, event.host_team_source);
        } else {
            match_watch_app_clear_host_team(true);
        }
        ret = ESP_OK;
    } else {
        strlcpy(event.text, canonical, sizeof(event.text));
        ret = match_watch_app_post_external_event(&event);
    }

    if (ret == ESP_OK && competition != NULL && competition[0] != '\0') {
        ret = match_watch_runtime_set_competition(competition);
    }
    return ret;
}

esp_err_t match_watch_runtime_set_favorite(const char *team, const char *competition)
{
    return match_watch_runtime_set_favorite_with_source(team, competition, "user");
}

esp_err_t match_watch_runtime_set_competition(const char *competition)
{
    ESP_RETURN_ON_ERROR(match_watch_app_ensure_context(), TAG, "create app context failed");
    if (competition == NULL || competition[0] == '\0') {
        s_app->selection.competition[0] = '\0';
        return ESP_OK;
    }
    if (strlen(competition) >= sizeof(s_app->selection.competition)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_app->selection.competition, competition, sizeof(s_app->selection.competition));
    return ESP_OK;
}

esp_err_t match_watch_runtime_set_reminders(bool enabled)
{
    match_watch_app_external_event_t event = {
        .type = MATCH_WATCH_EXTERNAL_EVENT_REMINDERS,
        .enabled = enabled,
    };

    ESP_RETURN_ON_ERROR(match_watch_app_ensure_external_queue(), TAG, "create external queue failed");

    if (!s_app->runtime.running) {
        s_app->runtime.reminders_enabled = enabled;
        return ESP_OK;
    }

    return match_watch_app_post_external_event(&event);
}

esp_err_t match_watch_runtime_set_notify_target(const char *channel, const char *chat_id)
{
    ESP_RETURN_ON_ERROR(match_watch_app_ensure_context(), TAG, "create app context failed");

    if (chat_id == NULL || chat_id[0] == '\0') {
        s_app->runtime.notify_channel[0] = '\0';
        s_app->runtime.notify_chat_id[0] = '\0';
        return ESP_OK;
    }
    if (strlen(chat_id) >= sizeof(s_app->runtime.notify_chat_id) ||
            (channel != NULL && strlen(channel) >= sizeof(s_app->runtime.notify_channel))) {
        return ESP_ERR_INVALID_ARG;
    }

    if (channel != NULL && channel[0] != '\0') {
        strlcpy(s_app->runtime.notify_channel, channel, sizeof(s_app->runtime.notify_channel));
    } else if (s_app->runtime.notify_channel[0] == '\0') {
        strlcpy(s_app->runtime.notify_channel, "wechat", sizeof(s_app->runtime.notify_channel));
    }
    strlcpy(s_app->runtime.notify_chat_id, chat_id, sizeof(s_app->runtime.notify_chat_id));
    return ESP_OK;
}

esp_err_t match_watch_runtime_get_status(match_watch_runtime_status_t *out_status)
{
    if (out_status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(match_watch_app_ensure_context(), TAG, "create app context failed");
    match_watch_app_load_host_team();

    memset(out_status, 0, sizeof(*out_status));
    out_status->running = match_watch_runtime_is_running();
    out_status->reminders_enabled = s_app != NULL && s_app->runtime.reminders_enabled;
    out_status->data_source = s_app != NULL ? s_app->runtime.data_source : MATCH_WATCH_DATA_SOURCE_LIVE;
    if (s_app != NULL) {
        if (s_app->selection.host_team[0] != '\0') {
            strlcpy(out_status->favorite_team,
                    s_app->selection.host_team,
                    sizeof(out_status->favorite_team));
        }
        strlcpy(out_status->favorite_team_source,
                match_watch_app_host_team_source_name(s_app->selection.host_team_source),
                sizeof(out_status->favorite_team_source));
        if (s_app->selection.competition[0] != '\0') {
            strlcpy(out_status->active_competition,
                    s_app->selection.competition,
                    sizeof(out_status->active_competition));
        }
        strlcpy(out_status->notify_channel,
                s_app->runtime.notify_channel,
                sizeof(out_status->notify_channel));
        strlcpy(out_status->notify_chat_id,
                s_app->runtime.notify_chat_id,
                sizeof(out_status->notify_chat_id));
    }
    return ESP_OK;
}

esp_err_t match_watch_runtime_set_notification_callback(match_watch_runtime_notification_cb_t cb,
                                                        void *user_data)
{
    return match_watch_ctx_set_notification_callback(cb, user_data);
}

esp_err_t match_watch_runtime_set_push_success_callback(match_watch_runtime_push_success_cb_t cb,
                                                        void *user_data)
{
    ESP_RETURN_ON_ERROR(match_watch_runtime_ensure_lock(), TAG, "create lock failed");

    xSemaphoreTake(s_match_watch_runtime.lock, portMAX_DELAY);
    s_match_watch_runtime.push_success_cb = cb;
    s_match_watch_runtime.push_success_user_data = user_data;
    xSemaphoreGive(s_match_watch_runtime.lock);
    return ESP_OK;
}

void match_watch_runtime_notify_push_success(void)
{
    match_watch_runtime_push_success_cb_t cb = NULL;
    void *user_data = NULL;

    if (match_watch_runtime_ensure_lock() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_match_watch_runtime.lock, portMAX_DELAY);
    cb = s_match_watch_runtime.push_success_cb;
    user_data = s_match_watch_runtime.push_success_user_data;
    xSemaphoreGive(s_match_watch_runtime.lock);

    if (cb != NULL) {
        cb(user_data);
    }
}

static esp_err_t match_watch_app_post_external_event(const match_watch_app_external_event_t *event)
{
    if (event == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(match_watch_app_ensure_external_queue(), TAG, "create external queue failed");
    if (xQueueSend(s_app->external.queue, event, 0) != pdTRUE) {
        if (event->type == MATCH_WATCH_EXTERNAL_EVENT_CLOSE) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "external event queue full: type=%d", (int)event->type);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t match_watch_app_request_close(void)
{
    const match_watch_app_external_event_t event = {
        .type = MATCH_WATCH_EXTERNAL_EVENT_CLOSE,
    };
    esp_err_t ret;

    if (s_app == NULL || !s_app->runtime.running) {
        return ESP_OK;
    }

    if (s_app->runtime.should_stop) {
        return ESP_OK;
    }
    if (s_app->runtime.close_requested) {
        return ESP_OK;
    }

    s_app->runtime.close_requested = true;
    ret = match_watch_app_post_external_event(&event);
    if (ret != ESP_OK) {
        s_app->runtime.close_requested = false;
    }
    return ret;
}

esp_err_t match_watch_app_request_data_changed(void)
{
    const match_watch_app_external_event_t event = {
        .type = MATCH_WATCH_EXTERNAL_EVENT_DATA_CHANGED,
    };

    if (s_app == NULL || !s_app->runtime.running) {
        return ESP_OK;
    }

    return match_watch_app_post_external_event(&event);
}

esp_err_t match_watch_app_request_pet_reload(void)
{
    const match_watch_app_external_event_t event = {
        .type = MATCH_WATCH_EXTERNAL_EVENT_PET_RELOAD,
    };

    if (s_app == NULL || !s_app->runtime.running) {
        return ESP_OK;
    }

    return match_watch_app_post_external_event(&event);
}

esp_err_t match_watch_app_set_data_source(match_watch_data_source_t source)
{
    match_watch_app_external_event_t event = {
        .type = MATCH_WATCH_EXTERNAL_EVENT_DATA_SOURCE,
        .data_source = source,
    };

    if (source != MATCH_WATCH_DATA_SOURCE_LIVE &&
            source != MATCH_WATCH_DATA_SOURCE_EXTERNAL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(match_watch_app_ensure_external_queue(), TAG, "create external queue failed");

    if (!s_app->runtime.running) {
        s_app->runtime.data_source = source;
        return ESP_OK;
    }

    return match_watch_app_post_external_event(&event);
}

bool match_watch_app_is_running(void)
{
    return s_app != NULL && s_app->runtime.running;
}
