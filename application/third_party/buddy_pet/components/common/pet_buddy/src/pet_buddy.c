/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pet_buddy.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pet_registry_internal.h"
#include "pet_runtime.h"

typedef struct {
    SemaphoreHandle_t lock;
    pet_host_t *active_host;
    pet_buddy_scene_hooks_t hooks;
} pet_buddy_state_t;

static const char *const TAG = "pet_buddy";
static pet_buddy_state_t s_buddy;

static esp_err_t pet_buddy_ensure_lock(void)
{
    if (s_buddy.lock == NULL) {
        s_buddy.lock = xSemaphoreCreateMutex();
    }
    return s_buddy.lock != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static const pet_registry_entry_t *pet_buddy_current_pet_entry(pet_registry_entry_t *entry)
{
    if (entry != NULL && pet_registry_get_selected_entry(entry) == ESP_OK) {
        return entry;
    }
    return NULL;
}

static pet_host_t *pet_buddy_get_active_host(void)
{
    pet_host_t *host = NULL;

    if (s_buddy.lock == NULL) {
        return NULL;
    }

    xSemaphoreTake(s_buddy.lock, portMAX_DELAY);
    host = s_buddy.active_host;
    xSemaphoreGive(s_buddy.lock);
    return host;
}

static void pet_buddy_notify_pet_changed(void)
{
    pet_buddy_scene_hooks_t hooks = {0};
    pet_registry_entry_t entry = {0};
    const pet_registry_entry_t *entry_ptr = NULL;

    if (pet_buddy_ensure_lock() != ESP_OK) {
        return;
    }

    xSemaphoreTake(s_buddy.lock, portMAX_DELAY);
    hooks = s_buddy.hooks;
    xSemaphoreGive(s_buddy.lock);

    if (hooks.on_pet_changed == NULL) {
        return;
    }
    entry_ptr = pet_buddy_current_pet_entry(&entry);
    hooks.on_pet_changed(entry_ptr, hooks.user_data);
}

esp_err_t pet_buddy_start(void)
{
    return pet_runtime_start();
}

esp_err_t pet_buddy_stop(void)
{
    return pet_runtime_stop();
}

bool pet_buddy_has_active(void)
{
    bool active = false;

    if (s_buddy.lock == NULL) {
        return false;
    }
    xSemaphoreTake(s_buddy.lock, portMAX_DELAY);
    active = s_buddy.active_host != NULL && s_buddy.active_host->opened;
    xSemaphoreGive(s_buddy.lock);
    return active;
}

esp_err_t pet_buddy_action(const char *action, bool keep_pos)
{
    pet_host_t *host = NULL;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(action != NULL && action[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "invalid action");

    host = pet_buddy_get_active_host();
    ret = (host != NULL && host->opened) ? pet_host_set_action(host, action, keep_pos) :
          ESP_ERR_INVALID_STATE;
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = pet_buddy_start();
        if (ret == ESP_OK) {
            host = pet_buddy_get_active_host();
            ret = (host != NULL && host->opened) ? pet_host_set_action(host, action, keep_pos) :
                  ESP_ERR_INVALID_STATE;
        }
    }
    return ret;
}

esp_err_t pet_buddy_attach_host(pet_host_t *host, const char *owner)
{
    return pet_buddy_attach_scene(host, owner, NULL);
}

esp_err_t pet_buddy_attach_scene(pet_host_t *host,
                                 const char *owner,
                                 const pet_buddy_scene_hooks_t *hooks)
{
    pet_buddy_scene_hooks_t new_hooks = {0};
    pet_buddy_scene_hooks_t old_hooks = {0};
    bool replaced = false;
    pet_registry_entry_t entry = {0};
    const pet_registry_entry_t *entry_ptr = NULL;

    ESP_RETURN_ON_FALSE(host != NULL && host->opened, ESP_ERR_INVALID_ARG, TAG, "invalid host");
    if (hooks != NULL) {
        new_hooks = *hooks;
    }

    ESP_RETURN_ON_ERROR(pet_buddy_ensure_lock(), TAG, "ensure lock failed");

    xSemaphoreTake(s_buddy.lock, portMAX_DELAY);
    if (s_buddy.active_host != NULL && s_buddy.active_host != host) {
        old_hooks = s_buddy.hooks;
        replaced = true;
    }
    s_buddy.active_host = host;
    s_buddy.hooks = new_hooks;
    xSemaphoreGive(s_buddy.lock);

    ESP_LOGI(TAG, "active host: %s", owner != NULL && owner[0] != '\0' ? owner : "(unnamed)");
    if (replaced && old_hooks.on_unmount != NULL) {
        old_hooks.on_unmount(old_hooks.user_data);
    }
    if (new_hooks.on_mount != NULL) {
        entry_ptr = pet_buddy_current_pet_entry(&entry);
        new_hooks.on_mount(entry_ptr, new_hooks.user_data);
    }
    return ESP_OK;
}

esp_err_t pet_buddy_select(const char *pet_id)
{
    esp_err_t ret;

    ret = pet_registry_select(pet_id);
    if (ret == ESP_OK) {
        pet_buddy_notify_pet_changed();
    }
    return ret;
}

esp_err_t pet_buddy_clear_selected(void)
{
    esp_err_t ret = pet_registry_clear_selected();

    if (ret == ESP_OK) {
        pet_buddy_notify_pet_changed();
    }
    return ret;
}

esp_err_t pet_buddy_attach_scene_pet(pet_host_t *host,
                                     const pet_buddy_scene_host_config_t *config)
{
    pet_host_config_t host_config = {0};
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(host != NULL && config != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid scene host");
    host_config.display = config->display;
    host_config.max_action_asset_bytes = config->max_action_asset_bytes;
    ESP_RETURN_ON_ERROR(pet_host_open(host, &host_config), TAG, "open pet host failed");
    ret = pet_buddy_attach_scene(host, config->owner, config->hooks);
    ESP_GOTO_ON_ERROR(ret, fail_close_host, TAG, "attach pet scene failed");
    return ESP_OK;

fail_close_host:
    pet_host_close(host);
    return ret;
}

void pet_buddy_detach_host(pet_host_t *host)
{
    pet_buddy_scene_hooks_t hooks = {0};
    bool detached = false;

    if (pet_buddy_ensure_lock() == ESP_OK) {
        xSemaphoreTake(s_buddy.lock, portMAX_DELAY);
        if (s_buddy.active_host == host) {
            hooks = s_buddy.hooks;
            detached = true;
            s_buddy.active_host = NULL;
            memset(&s_buddy.hooks, 0, sizeof(s_buddy.hooks));
        }
        xSemaphoreGive(s_buddy.lock);
    }
    if (detached && hooks.on_unmount != NULL) {
        hooks.on_unmount(hooks.user_data);
    }
}

void pet_buddy_detach_scene_pet(pet_host_t *host)
{
    pet_buddy_detach_host(host);
    pet_host_close(host);
}
