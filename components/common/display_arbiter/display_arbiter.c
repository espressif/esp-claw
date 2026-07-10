/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "display_arbiter.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display_arbiter";

typedef struct {
    SemaphoreHandle_t lock;
    display_arbiter_owner_t owner;
    uint32_t lua_depth;
    display_arbiter_owner_changed_cb_t callback;
    void *callback_user_ctx;
} display_arbiter_state_t;

static display_arbiter_state_t s_state = {
    .owner = DISPLAY_ARBITER_OWNER_EMOTE,
};

static esp_err_t display_arbiter_lock(void)
{
    if (!s_state.lock) {
        s_state.lock = xSemaphoreCreateMutex();
    }
    ESP_RETURN_ON_FALSE(s_state.lock != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_state.lock, pdMS_TO_TICKS(1000)) == pdTRUE, ESP_ERR_TIMEOUT, TAG, "mutex timeout");
    return ESP_OK;
}

static void display_arbiter_unlock(void)
{
    if (s_state.lock) {
        xSemaphoreGive(s_state.lock);
    }
}

static const char *display_arbiter_owner_to_str(display_arbiter_owner_t owner)
{
    switch (owner) {
    case DISPLAY_ARBITER_OWNER_LUA:
        return "lua";
    case DISPLAY_ARBITER_OWNER_EMOTE:
        return "emote";
    default:
        return "unknown";
    }
    return "unknown";
}

static esp_err_t display_arbiter_change_owner_locked(display_arbiter_owner_t owner)
{
    s_state.owner = owner;
    ESP_LOGI(TAG, "display owner changed to %s", display_arbiter_owner_to_str(owner));
    return ESP_OK;
}

static void display_arbiter_notify_owner_changed(display_arbiter_owner_t owner)
{
    if (s_state.callback) {
        s_state.callback(owner, s_state.callback_user_ctx);
    }
}

esp_err_t display_arbiter_acquire(display_arbiter_owner_t owner)
{
    esp_err_t ret = display_arbiter_lock();
    display_arbiter_owner_t notify_owner = DISPLAY_ARBITER_OWNER_NONE;
    bool owner_changed = false;

    if (ret != ESP_OK) {
        return ret;
    }

    ESP_GOTO_ON_FALSE(owner == DISPLAY_ARBITER_OWNER_LUA || owner == DISPLAY_ARBITER_OWNER_EMOTE, ESP_ERR_INVALID_ARG,
                      fail, TAG, "invalid owner");

    if (owner == DISPLAY_ARBITER_OWNER_LUA) {
        s_state.lua_depth++;
        if (s_state.owner != DISPLAY_ARBITER_OWNER_LUA) {
            ESP_GOTO_ON_ERROR(display_arbiter_change_owner_locked(DISPLAY_ARBITER_OWNER_LUA), fail, TAG,
                              "switch to lua owner failed");
            notify_owner = DISPLAY_ARBITER_OWNER_LUA;
            owner_changed = true;
        }
    } else if (s_state.owner != DISPLAY_ARBITER_OWNER_EMOTE) {
        ESP_GOTO_ON_ERROR(display_arbiter_change_owner_locked(DISPLAY_ARBITER_OWNER_EMOTE), fail, TAG,
                          "switch to emote owner failed");
        notify_owner = DISPLAY_ARBITER_OWNER_EMOTE;
        owner_changed = true;
    }

fail:
    display_arbiter_unlock();
    if (ret == ESP_OK && owner_changed) {
        display_arbiter_notify_owner_changed(notify_owner);
    }
    return ret;
}

esp_err_t display_arbiter_release(display_arbiter_owner_t owner)
{
    esp_err_t ret = display_arbiter_lock();
    display_arbiter_owner_t notify_owner = DISPLAY_ARBITER_OWNER_NONE;
    bool owner_changed = false;

    if (ret != ESP_OK) {
        return ret;
    }

    ESP_GOTO_ON_FALSE(owner == DISPLAY_ARBITER_OWNER_LUA || owner == DISPLAY_ARBITER_OWNER_EMOTE, ESP_ERR_INVALID_ARG,
                      fail, TAG, "invalid owner");

    if (owner == DISPLAY_ARBITER_OWNER_LUA) {
        ESP_GOTO_ON_FALSE(s_state.lua_depth > 0, ESP_ERR_INVALID_STATE, fail, TAG, "lua owner is not active");
        s_state.lua_depth--;
        if (s_state.lua_depth == 0 && s_state.owner == DISPLAY_ARBITER_OWNER_LUA) {
            ESP_GOTO_ON_ERROR(display_arbiter_change_owner_locked(DISPLAY_ARBITER_OWNER_EMOTE), fail, TAG,
                              "restore emote owner failed");
            notify_owner = DISPLAY_ARBITER_OWNER_EMOTE;
            owner_changed = true;
        }
    } else if (s_state.owner == DISPLAY_ARBITER_OWNER_EMOTE) {
        ESP_GOTO_ON_ERROR(display_arbiter_change_owner_locked(DISPLAY_ARBITER_OWNER_NONE), fail, TAG,
                          "clear emote owner failed");
        notify_owner = DISPLAY_ARBITER_OWNER_NONE;
        owner_changed = true;
    }

fail:
    display_arbiter_unlock();
    if (ret == ESP_OK && owner_changed) {
        display_arbiter_notify_owner_changed(notify_owner);
    }
    return ret;
}

display_arbiter_owner_t display_arbiter_get_owner(void)
{
    return s_state.owner;
}

bool display_arbiter_is_owner(display_arbiter_owner_t owner)
{
    return display_arbiter_get_owner() == owner;
}

esp_err_t display_arbiter_set_owner_changed_callback(display_arbiter_owner_changed_cb_t callback, void *user_ctx)
{
    esp_err_t ret = display_arbiter_lock();

    if (ret != ESP_OK) {
        return ret;
    }

    s_state.callback = callback;
    s_state.callback_user_ctx = user_ctx;
    display_arbiter_unlock();
    return ESP_OK;
}
