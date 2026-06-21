/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "scene_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "scene_manager";

#define SCENE_MANAGER_NAME_MAX 32

typedef struct {
    SemaphoreHandle_t lock;
    char active[SCENE_MANAGER_NAME_MAX];
    scene_manager_close_cb_t close_cb;
    void *user_data;
} scene_manager_state_t;

static scene_manager_state_t s_scene_manager;

static esp_err_t scene_manager_ensure_lock(void)
{
    if (s_scene_manager.lock == NULL) {
        s_scene_manager.lock = xSemaphoreCreateRecursiveMutex();
    }
    ESP_RETURN_ON_FALSE(s_scene_manager.lock != NULL, ESP_ERR_NO_MEM, TAG, "create lock failed");
    return ESP_OK;
}

static esp_err_t scene_manager_lock(void)
{
    ESP_RETURN_ON_ERROR(scene_manager_ensure_lock(), TAG, "ensure lock failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTakeRecursive(s_scene_manager.lock, pdMS_TO_TICKS(3000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "lock timeout");
    return ESP_OK;
}

static void scene_manager_unlock(void)
{
    if (s_scene_manager.lock != NULL) {
        xSemaphoreGiveRecursive(s_scene_manager.lock);
    }
}

esp_err_t scene_manager_acquire(const scene_manager_scene_t *scene)
{
    char old_name[SCENE_MANAGER_NAME_MAX] = {0};
    scene_manager_close_cb_t old_close = NULL;
    void *old_user_data = NULL;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(scene != NULL && scene->name != NULL && scene->name[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "invalid scene");
    ESP_RETURN_ON_ERROR(scene_manager_lock(), TAG, "lock failed");

    if (strcmp(s_scene_manager.active, scene->name) == 0) {
        s_scene_manager.close_cb = scene->close_cb;
        s_scene_manager.user_data = scene->user_data;
        scene_manager_unlock();
        return ESP_OK;
    }

    if (s_scene_manager.active[0] != '\0') {
        strlcpy(old_name, s_scene_manager.active, sizeof(old_name));
        old_close = s_scene_manager.close_cb;
        old_user_data = s_scene_manager.user_data;
    }
    scene_manager_unlock();

    if (old_close != NULL) {
        ESP_LOGI(TAG, "closing scene %s before opening %s", old_name, scene->name);
        ret = old_close(old_user_data);
        ESP_RETURN_ON_ERROR(ret, TAG, "close old scene failed");
    }

    ESP_RETURN_ON_ERROR(scene_manager_lock(), TAG, "lock failed");
    if (old_name[0] != '\0' && strcmp(s_scene_manager.active, old_name) == 0) {
        s_scene_manager.active[0] = '\0';
        s_scene_manager.close_cb = NULL;
        s_scene_manager.user_data = NULL;
    }
    strlcpy(s_scene_manager.active, scene->name, sizeof(s_scene_manager.active));
    s_scene_manager.close_cb = scene->close_cb;
    s_scene_manager.user_data = scene->user_data;
    ESP_LOGI(TAG, "active scene: %s", s_scene_manager.active);
    scene_manager_unlock();
    return ESP_OK;
}

void scene_manager_release(const char *name)
{
    if (name == NULL || scene_manager_lock() != ESP_OK) {
        return;
    }
    if (strcmp(s_scene_manager.active, name) == 0) {
        ESP_LOGI(TAG, "release scene: %s", name);
        s_scene_manager.active[0] = '\0';
        s_scene_manager.close_cb = NULL;
        s_scene_manager.user_data = NULL;
    }
    scene_manager_unlock();
}

const char *scene_manager_active_scene(void)
{
    return s_scene_manager.active[0] != '\0' ? s_scene_manager.active : NULL;
}
