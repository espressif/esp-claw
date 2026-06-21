/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene_runtime.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "scene_manager.h"

static const char *TAG = "scene_runtime";

#define SCENE_RUNTIME_OPEN_POLL_MS  20
#define SCENE_RUNTIME_CLOSE_POLL_MS 20
#define SCENE_RUNTIME_DEFAULT_CLOSE_WAIT_MS 3000

static esp_err_t scene_runtime_ensure_lock(scene_runtime_t *runtime)
{
    ESP_RETURN_ON_FALSE(runtime != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid runtime");
    if (runtime->lock == NULL) {
        runtime->lock = xSemaphoreCreateMutex();
    }
    ESP_RETURN_ON_FALSE(runtime->lock != NULL, ESP_ERR_NO_MEM, TAG, "create lock failed");
    return ESP_OK;
}

static uint32_t scene_runtime_close_wait_ms(const scene_runtime_t *runtime)
{
    return runtime->config.close_wait_ms > 0 ? runtime->config.close_wait_ms :
           SCENE_RUNTIME_DEFAULT_CLOSE_WAIT_MS;
}

static bool scene_runtime_is_running_locked(const scene_runtime_t *runtime)
{
    return runtime->running || runtime->task != NULL;
}

static void scene_runtime_clear_opening_locked(scene_runtime_t *runtime)
{
    runtime->opening = false;
    runtime->close_requested = false;
}

static esp_err_t scene_runtime_wait_opening_done(scene_runtime_t *runtime)
{
    TickType_t start = xTaskGetTickCount();
    bool opening;

    do {
        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        opening = runtime->opening;
        xSemaphoreGive(runtime->lock);
        if (!opening) {
            return ESP_OK;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(scene_runtime_close_wait_ms(runtime))) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(SCENE_RUNTIME_OPEN_POLL_MS));
    } while (true);
}

static esp_err_t scene_runtime_scene_close_cb(void *user_data)
{
    return scene_runtime_close((scene_runtime_t *)user_data);
}

static esp_err_t scene_runtime_default_task_create(const scene_runtime_task_config_t *config,
                                                   TaskFunction_t task_fn,
                                                   void *task_arg,
                                                   TaskHandle_t *out_task)
{
    BaseType_t created;

    ESP_RETURN_ON_FALSE(config != NULL && task_fn != NULL && out_task != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid task args");
    created = xTaskCreate(task_fn, config->name, config->stack_size, task_arg,
                          config->priority, out_task);
    return created == pdPASS && *out_task != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static void scene_runtime_default_task_delete(void)
{
    vTaskDelete(NULL);
}

static void scene_runtime_task(void *arg)
{
    scene_runtime_t *runtime = (scene_runtime_t *)arg;
    scene_runtime_config_t config;
    esp_err_t ret = ESP_OK;

    if (runtime == NULL || scene_runtime_ensure_lock(runtime) != ESP_OK) {
        scene_runtime_default_task_delete();
        return;
    }

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    config = runtime->config;
    xSemaphoreGive(runtime->lock);

    if (config.run != NULL) {
        ret = config.run(config.user_data);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "scene %s stopped with error: %s",
                     config.scene_name != NULL ? config.scene_name : "(unnamed)",
                     esp_err_to_name(ret));
        }
    }

    if (config.scene_name != NULL) {
        scene_manager_release(config.scene_name);
    }

    if (config.cleanup != NULL) {
        config.cleanup(config.user_data);
    }

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    runtime->running = false;
    runtime->task = NULL;
    scene_runtime_clear_opening_locked(runtime);
    xSemaphoreGive(runtime->lock);

    if (config.task_delete != NULL) {
        config.task_delete();
        return;
    }
    scene_runtime_default_task_delete();
}

esp_err_t scene_runtime_open(scene_runtime_t *runtime,
                             const scene_runtime_config_t *config,
                             bool *already_running)
{
    scene_runtime_task_config_t task_config;
    const scene_manager_scene_t scene = {
        .name = config != NULL ? config->scene_name : NULL,
        .close_cb = scene_runtime_scene_close_cb,
        .user_data = runtime,
    };
    scene_runtime_task_create_cb_t task_create;
    bool should_open;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(runtime != NULL && config != NULL &&
                        config->scene_name != NULL && config->scene_name[0] != '\0' &&
                        config->task_name != NULL && config->task_name[0] != '\0' &&
                        config->run != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid config");
    if (already_running != NULL) {
        *already_running = false;
    }
    ESP_RETURN_ON_ERROR(scene_runtime_ensure_lock(runtime), TAG, "ensure lock failed");

    do {
        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        if (scene_runtime_is_running_locked(runtime)) {
            if (already_running != NULL) {
                *already_running = true;
            }
            xSemaphoreGive(runtime->lock);
            return ESP_OK;
        }
        should_open = !runtime->opening;
        if (should_open) {
            runtime->opening = true;
            runtime->close_requested = false;
            runtime->config = *config;
        }
        xSemaphoreGive(runtime->lock);

        if (!should_open) {
            ESP_RETURN_ON_ERROR(scene_runtime_wait_opening_done(runtime), TAG, "wait opening failed");
        }
    } while (!should_open);

    ret = scene_manager_acquire(&scene);
    if (ret != ESP_OK) {
        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        scene_runtime_clear_opening_locked(runtime);
        xSemaphoreGive(runtime->lock);
        if (config->cleanup != NULL) {
            config->cleanup(config->user_data);
        }
        ESP_RETURN_ON_ERROR(ret, TAG, "acquire scene failed");
    }

    if (config->prepare != NULL) {
        ret = config->prepare(config->user_data);
        if (ret != ESP_OK) {
            xSemaphoreTake(runtime->lock, portMAX_DELAY);
            scene_runtime_clear_opening_locked(runtime);
            xSemaphoreGive(runtime->lock);
            if (config->cleanup != NULL) {
                config->cleanup(config->user_data);
            }
            scene_manager_release(config->scene_name);
            return ret;
        }
    }

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (runtime->close_requested) {
        scene_runtime_clear_opening_locked(runtime);
        xSemaphoreGive(runtime->lock);
        if (config->cleanup != NULL) {
            config->cleanup(config->user_data);
        }
        scene_manager_release(config->scene_name);
        return ESP_ERR_INVALID_STATE;
    }

    task_config = (scene_runtime_task_config_t) {
        .name = config->task_name,
        .stack_size = config->task_stack_size,
        .priority = config->task_priority,
    };
    task_create = config->task_create != NULL ? config->task_create :
                  scene_runtime_default_task_create;
    runtime->running = true;
    runtime->opening = false;
    runtime->close_requested = false;
    ret = task_create(&task_config, scene_runtime_task, runtime, &runtime->task);
    if (ret != ESP_OK || runtime->task == NULL) {
        runtime->running = false;
        runtime->task = NULL;
        scene_runtime_clear_opening_locked(runtime);
        xSemaphoreGive(runtime->lock);
        if (config->cleanup != NULL) {
            config->cleanup(config->user_data);
        }
        scene_manager_release(config->scene_name);
        return ret != ESP_OK ? ret : ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(runtime->lock);
    return ESP_OK;
}

esp_err_t scene_runtime_close(scene_runtime_t *runtime)
{
    TickType_t start;
    bool requested = false;
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_ERROR(scene_runtime_ensure_lock(runtime), TAG, "ensure lock failed");

    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    if (runtime->opening && !scene_runtime_is_running_locked(runtime)) {
        runtime->close_requested = true;
        xSemaphoreGive(runtime->lock);
        return ESP_OK;
    }
    if (!scene_runtime_is_running_locked(runtime)) {
        xSemaphoreGive(runtime->lock);
        return ESP_OK;
    }
    xSemaphoreGive(runtime->lock);

    start = xTaskGetTickCount();
    while (scene_runtime_is_running(runtime)) {
        scene_runtime_request_close_cb_t request_close = NULL;
        void *user_data = NULL;
        bool repeat_close_request = false;

        xSemaphoreTake(runtime->lock, portMAX_DELAY);
        request_close = runtime->config.request_close;
        user_data = runtime->config.user_data;
        repeat_close_request = runtime->config.repeat_close_request;
        xSemaphoreGive(runtime->lock);

        if (request_close != NULL && (!requested || repeat_close_request)) {
            ret = request_close(user_data);
            if (ret == ESP_ERR_INVALID_STATE) {
                return ESP_OK;
            }
            if (ret != ESP_OK && !(repeat_close_request && ret == ESP_ERR_TIMEOUT)) {
                return ret;
            }
            requested = true;
        }
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(scene_runtime_close_wait_ms(runtime))) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(SCENE_RUNTIME_CLOSE_POLL_MS));
    }
    return ESP_OK;
}

bool scene_runtime_is_running(scene_runtime_t *runtime)
{
    bool running;

    if (runtime == NULL || scene_runtime_ensure_lock(runtime) != ESP_OK) {
        return false;
    }
    xSemaphoreTake(runtime->lock, portMAX_DELAY);
    running = scene_runtime_is_running_locked(runtime);
    xSemaphoreGive(runtime->lock);
    return running;
}
