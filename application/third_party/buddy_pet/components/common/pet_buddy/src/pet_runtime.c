/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "pet_runtime.h"

#include <string.h>

#include "display_session.h"
#include "esp_check.h"
#include "esp_log.h"
#include "pet_buddy.h"
#include "pet_host.h"

static const char *const TAG = "pet_runtime";

typedef struct {
    bool running;
    display_session_t *display_session;
    pet_host_t pet;
} pet_runtime_state_t;

static pet_runtime_state_t s_pet_runtime;

static esp_err_t pet_runtime_open_pet(bool active);
static void pet_runtime_close_pet(void);

static void pet_runtime_on_pet_changed(const pet_registry_entry_t *entry, void *user_data)
{
    bool active;

    (void)entry;
    (void)user_data;
    if (!s_pet_runtime.running || s_pet_runtime.display_session == NULL) {
        return;
    }

    active = display_session_is_active(s_pet_runtime.display_session);
    pet_runtime_close_pet();
    if (pet_runtime_open_pet(active) != ESP_OK) {
        ESP_LOGW(TAG, "reload default pet failed");
    }
}

static const pet_buddy_scene_hooks_t s_pet_runtime_hooks = {
    .on_pet_changed = pet_runtime_on_pet_changed,
};

static void pet_runtime_touch_cb(gfx_touch_t *touch, const gfx_touch_event_t *event, void *user_data)
{
    (void)touch;
    (void)user_data;

    if (s_pet_runtime.running) {
        pet_host_handle_touch(&s_pet_runtime.pet, event);
    }
}

static void pet_runtime_active_cb(bool active, void *user_data)
{
    (void)user_data;

    if (!s_pet_runtime.running || !s_pet_runtime.pet.opened) {
        return;
    }
    if (active) {
        esp_err_t ret = pet_buddy_attach_scene(&s_pet_runtime.pet, "empty_pet", &s_pet_runtime_hooks);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "reactivate default pet failed: %s", esp_err_to_name(ret));
            return;
        }
        pet_host_set_visible(&s_pet_runtime.pet, true);
    } else {
        pet_buddy_detach_host(&s_pet_runtime.pet);
        pet_host_set_visible(&s_pet_runtime.pet, false);
    }
}

static esp_err_t pet_runtime_open_pet(bool active)
{
    esp_err_t ret;
    const pet_buddy_scene_host_config_t config = {
        .display = display_session_display(s_pet_runtime.display_session),
        .owner = "empty_pet",
        .hooks = active ? &s_pet_runtime_hooks : NULL,
    };

    ret = active ? pet_buddy_attach_scene_pet(&s_pet_runtime.pet, &config) :
          pet_host_open(&s_pet_runtime.pet, &(const pet_host_config_t) {
              .display = config.display,
          });
    ESP_RETURN_ON_ERROR(ret, TAG, "create empty pet module failed");
    pet_host_place(&s_pet_runtime.pet, PET_HOST_PAGE_HOME);
    pet_host_set_visible(&s_pet_runtime.pet, active);
    return ESP_OK;
}

static void pet_runtime_close_pet(void)
{
    pet_buddy_detach_scene_pet(&s_pet_runtime.pet);
    memset(&s_pet_runtime.pet, 0, sizeof(s_pet_runtime.pet));
}

esp_err_t pet_runtime_start(void)
{
    esp_err_t ret;
    bool active;

    if (s_pet_runtime.running) {
        return ESP_OK;
    }
    if (pet_buddy_has_active()) {
        return ESP_OK;
    }

    const display_session_config_t session_config = {
        .touch_cb = pet_runtime_touch_cb,
        .active_cb = pet_runtime_active_cb,
    };
    ESP_RETURN_ON_ERROR(display_session_start(&s_pet_runtime.display_session, &session_config),
                        TAG, "start pet display session failed");

    if (display_session_lock(s_pet_runtime.display_session) == ESP_OK) {
        (void)gfx_disp_set_bg_color(display_session_display(s_pet_runtime.display_session),
                                    GFX_COLOR_HEX(0x000000));
        display_session_unlock(s_pet_runtime.display_session);
    }

    s_pet_runtime.running = true;
    active = display_session_is_active(s_pet_runtime.display_session);
    ret = pet_runtime_open_pet(active);
    ESP_GOTO_ON_ERROR(ret, fail_stop_session, TAG, "open default pet failed");
    return ESP_OK;

fail_stop_session:
    s_pet_runtime.running = false;
    display_session_stop(&s_pet_runtime.display_session);
    return ret;
}

esp_err_t pet_runtime_stop(void)
{
    if (!s_pet_runtime.running) {
        return ESP_OK;
    }

    s_pet_runtime.running = false;
    pet_runtime_close_pet();
    display_session_stop(&s_pet_runtime.display_session);
    return ESP_OK;
}

bool pet_runtime_is_running(void)
{
    return s_pet_runtime.running;
}
