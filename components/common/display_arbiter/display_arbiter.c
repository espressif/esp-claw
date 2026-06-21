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

#define DISPLAY_ARBITER_OWNER_STACK_MAX 4

typedef struct {
    SemaphoreHandle_t lock;
    display_arbiter_owner_t owner;
    uint32_t lua_depth;
    display_arbiter_owner_t owner_stack[DISPLAY_ARBITER_OWNER_STACK_MAX];
    uint32_t owner_stack_depth;
    display_arbiter_owner_changed_cb_t owner_changed_cb;
    void *owner_changed_user_ctx;
} display_arbiter_state_t;

static display_arbiter_state_t s_state = {
    .owner = DISPLAY_ARBITER_OWNER_EMOTE_GFX,
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
    case DISPLAY_ARBITER_OWNER_NONE:
        return "none";
    case DISPLAY_ARBITER_OWNER_LUA:
        return "lua";
    case DISPLAY_ARBITER_OWNER_EMOTE_GFX:
        return "emote_gfx";
    default:
        return "unknown";
    }
    return "unknown";
}

bool display_arbiter_owner_is_valid(display_arbiter_owner_t owner)
{
    return owner == DISPLAY_ARBITER_OWNER_LUA ||
           owner == DISPLAY_ARBITER_OWNER_EMOTE_GFX;
}

bool display_arbiter_owner_uses_emote_gfx(display_arbiter_owner_t owner)
{
    return owner == DISPLAY_ARBITER_OWNER_EMOTE_GFX;
}

static esp_err_t display_arbiter_change_owner_locked(display_arbiter_owner_t owner)
{
    s_state.owner = owner;
    ESP_LOGI(TAG, "display owner changed to %s", display_arbiter_owner_to_str(owner));
    return ESP_OK;
}

static esp_err_t display_arbiter_push_owner_locked(display_arbiter_owner_t owner)
{
    if (owner == DISPLAY_ARBITER_OWNER_NONE) {
        return ESP_OK;
    }
    if (s_state.owner_stack_depth > 0 &&
            s_state.owner_stack[s_state.owner_stack_depth - 1] == owner) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(s_state.owner_stack_depth < DISPLAY_ARBITER_OWNER_STACK_MAX,
                        ESP_ERR_NO_MEM, TAG, "owner stack full");
    s_state.owner_stack[s_state.owner_stack_depth++] = owner;
    return ESP_OK;
}

static display_arbiter_owner_t display_arbiter_pop_owner_locked(void)
{
    if (s_state.owner_stack_depth == 0) {
        return DISPLAY_ARBITER_OWNER_NONE;
    }
    return s_state.owner_stack[--s_state.owner_stack_depth];
}

static void display_arbiter_remove_stacked_owner_locked(display_arbiter_owner_t owner)
{
    uint32_t out = 0;

    for (uint32_t i = 0; i < s_state.owner_stack_depth; i++) {
        if (s_state.owner_stack[i] != owner) {
            s_state.owner_stack[out++] = s_state.owner_stack[i];
        }
    }
    for (uint32_t i = out; i < s_state.owner_stack_depth; i++) {
        s_state.owner_stack[i] = DISPLAY_ARBITER_OWNER_NONE;
    }
    s_state.owner_stack_depth = out;
}

static display_arbiter_owner_t display_arbiter_restore_owner_locked(display_arbiter_owner_t fallback)
{
    display_arbiter_owner_t owner;

    while ((owner = display_arbiter_pop_owner_locked()) != DISPLAY_ARBITER_OWNER_NONE) {
        if (owner == DISPLAY_ARBITER_OWNER_LUA && s_state.lua_depth == 0) {
            continue;
        }
        return owner;
    }
    return fallback;
}

static display_arbiter_owner_t display_arbiter_release_fallback_owner(display_arbiter_owner_t owner)
{
    (void)owner;
    return s_state.lua_depth > 0 ? DISPLAY_ARBITER_OWNER_LUA : DISPLAY_ARBITER_OWNER_EMOTE_GFX;
}

static void display_arbiter_notify_owner_changed(display_arbiter_owner_t owner)
{
    display_arbiter_owner_changed_cb_t callback = NULL;
    void *user_ctx = NULL;

    if (display_arbiter_lock() != ESP_OK) {
        return;
    }
    callback = s_state.owner_changed_cb;
    user_ctx = s_state.owner_changed_user_ctx;
    display_arbiter_unlock();

    if (callback != NULL) {
        callback(owner, user_ctx);
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

    ESP_GOTO_ON_FALSE(display_arbiter_owner_is_valid(owner), ESP_ERR_INVALID_ARG, fail, TAG, "invalid owner");

    if (owner == DISPLAY_ARBITER_OWNER_LUA) {
        if (s_state.owner != DISPLAY_ARBITER_OWNER_LUA) {
            ESP_GOTO_ON_ERROR(display_arbiter_push_owner_locked(s_state.owner), fail, TAG,
                              "save previous owner failed");
            ESP_GOTO_ON_ERROR(display_arbiter_change_owner_locked(DISPLAY_ARBITER_OWNER_LUA), fail, TAG,
                              "switch to lua owner failed");
            notify_owner = DISPLAY_ARBITER_OWNER_LUA;
            owner_changed = true;
        }
        s_state.lua_depth++;
    } else if (s_state.owner != owner) {
        ESP_GOTO_ON_ERROR(display_arbiter_push_owner_locked(s_state.owner), fail, TAG,
                          "save previous display owner failed");
        ESP_GOTO_ON_ERROR(display_arbiter_change_owner_locked(owner), fail, TAG,
                          "switch display owner failed");
        notify_owner = owner;
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

    ESP_GOTO_ON_FALSE(display_arbiter_owner_is_valid(owner), ESP_ERR_INVALID_ARG, fail, TAG, "invalid owner");

    if (owner == DISPLAY_ARBITER_OWNER_LUA) {
        ESP_GOTO_ON_FALSE(s_state.lua_depth > 0, ESP_ERR_INVALID_STATE, fail, TAG, "lua owner is not active");
        s_state.lua_depth--;
        if (s_state.lua_depth == 0 && s_state.owner == DISPLAY_ARBITER_OWNER_LUA) {
            display_arbiter_owner_t restore_owner =
                display_arbiter_restore_owner_locked(DISPLAY_ARBITER_OWNER_EMOTE_GFX);
            ESP_GOTO_ON_ERROR(display_arbiter_change_owner_locked(restore_owner), fail, TAG,
                              "restore emote owner failed");
            notify_owner = restore_owner;
            owner_changed = true;
        } else if (s_state.lua_depth == 0) {
            display_arbiter_remove_stacked_owner_locked(DISPLAY_ARBITER_OWNER_LUA);
        }
    } else if (s_state.owner == owner) {
        display_arbiter_owner_t restore_owner =
            display_arbiter_restore_owner_locked(display_arbiter_release_fallback_owner(owner));
        ESP_GOTO_ON_ERROR(display_arbiter_change_owner_locked(restore_owner), fail, TAG,
                          "restore display owner failed");
        notify_owner = restore_owner;
        owner_changed = true;
    } else {
        display_arbiter_remove_stacked_owner_locked(owner);
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

    s_state.owner_changed_cb = callback;
    s_state.owner_changed_user_ctx = user_ctx;
    display_arbiter_unlock();
    return ESP_OK;
}
