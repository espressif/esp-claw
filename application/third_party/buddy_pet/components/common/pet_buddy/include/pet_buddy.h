/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "pet_host.h"
#include "pet_registry.h"

/**
 * @file pet_buddy.h
 * @brief Public Pet Buddy coordinator API.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Standard Pet Buddy action names.
 */
#define PET_BUDDY_ACTION_IDLE          "idle"
#define PET_BUDDY_ACTION_RUNNING       "running"
#define PET_BUDDY_ACTION_RUNNING_LEFT  "running-left"
#define PET_BUDDY_ACTION_RUNNING_RIGHT "running-right"
#define PET_BUDDY_ACTION_WAVE          "waving"
#define PET_BUDDY_ACTION_JUMP          "jumping"
#define PET_BUDDY_ACTION_LOSE          "failed"
#define PET_BUDDY_ACTION_SLEEP         "waiting"
#define PET_BUDDY_ACTION_REVIEW        "review"

/**
 * @brief Callback invoked after a scene pet host is attached.
 *
 * @param entry Currently selected pet entry, or NULL when selection is cleared/unavailable.
 * @param user_data Opaque pointer provided by the scene.
 */
typedef void (*pet_buddy_scene_mount_cb_t)(const pet_registry_entry_t *entry, void *user_data);

/**
 * @brief Callback invoked before a scene pet host is detached or replaced.
 *
 * @param user_data Opaque pointer provided by the scene.
 */
typedef void (*pet_buddy_scene_unmount_cb_t)(void *user_data);

/**
 * @brief Callback invoked when the selected pet changes.
 *
 * @param entry Selected pet entry, or NULL when selection is cleared/unavailable.
 * @param user_data Opaque pointer provided by the scene.
 */
typedef void (*pet_buddy_pet_changed_cb_t)(const pet_registry_entry_t *entry, void *user_data);

/**
 * @brief Scene hooks used by Pet Buddy to notify the attached scene.
 */
typedef struct {
    /** Called after this scene becomes the active pet host. */
    pet_buddy_scene_mount_cb_t on_mount;
    /** Called before this scene stops being the active pet host. */
    pet_buddy_scene_unmount_cb_t on_unmount;
    /** Called after the globally selected pet changes. */
    pet_buddy_pet_changed_cb_t on_pet_changed;
    /** Opaque pointer passed to hook callbacks. */
    void *user_data;
} pet_buddy_scene_hooks_t;

/**
 * @brief Options for opening and attaching a scene-owned pet host.
 */
typedef struct {
    /** Target GFX display for the pet object. */
    gfx_disp_t *display;
    /** Optional owner label used for diagnostics. */
    const char *owner;
    /** Optional scene hooks copied by Pet Buddy. */
    const pet_buddy_scene_hooks_t *hooks;
    /** Maximum bytes allowed for a single action asset. Zero uses renderer default. */
    size_t max_action_asset_bytes;
} pet_buddy_scene_host_config_t;

/**
 * @brief Start the default Pet Buddy scene.
 *
 * If a business scene has already attached an active pet host, this is a no-op.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_buddy_start(void);

/**
 * @brief Stop the default Pet Buddy scene.
 *
 * Attached business scene hosts are not closed by this API.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_buddy_stop(void);

/**
 * @brief Check whether Pet Buddy currently has an active pet host.
 *
 * The active host may be the default empty pet scene or a business scene such
 * as Match Watch or Buddy Home.
 *
 * @return true when an active pet host exists, false otherwise.
 */
bool pet_buddy_has_active(void);

/**
 * @brief Apply an action to the active Pet Buddy pet.
 *
 * If no pet host is active, the default Pet Buddy scene is started first.
 *
 * @param action Pet action name, for example "idle" or "jumping".
 * @param keep_pos true to keep the current pet position when switching action.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_buddy_action(const char *action, bool keep_pos);

/**
 * @brief Select a pet and notify the active scene.
 *
 * Passing NULL or "" clears selection.
 *
 * @param pet_id Pet id from pet_registry_entry_t::id.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND when the id is unavailable, or an ESP error code.
 */
esp_err_t pet_buddy_select(const char *pet_id);

/**
 * @brief Clear selected pet and notify the active scene.
 *
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_buddy_clear_selected(void);

/**
 * @brief Attach a business scene pet host to Pet Buddy.
 *
 * This stops the default empty pet scene if needed and makes @p host the active
 * target for pet actions.
 *
 * @param host Opened pet host owned by the business scene.
 * @param owner Optional owner label used for diagnostics.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_buddy_attach_host(pet_host_t *host, const char *owner);

/**
 * @brief Attach a scene pet host with scene notification hooks.
 *
 * @param host Opened pet host owned by the scene.
 * @param owner Optional owner label used for diagnostics.
 * @param hooks Optional scene hooks copied by Pet Buddy.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_buddy_attach_scene(pet_host_t *host,
                                 const char *owner,
                                 const pet_buddy_scene_hooks_t *hooks);

/**
 * @brief Create a scene-owned pet host and attach it to Pet Buddy.
 *
 * This is a convenience wrapper around pet_host_open() and
 * pet_buddy_attach_scene(). On failure, any partially opened host is closed
 * before returning.
 *
 * @param[out] host Host instance to initialize.
 * @param config Scene host creation and attach options.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_buddy_attach_scene_pet(pet_host_t *host,
                                     const pet_buddy_scene_host_config_t *config);

/**
 * @brief Detach a business scene pet host from Pet Buddy.
 *
 * This only unregisters the host from Pet Buddy; the caller still owns closing
 * and destroying the host.
 *
 * @param host Pet host previously attached with pet_buddy_attach_host().
 */
void pet_buddy_detach_host(pet_host_t *host);

/**
 * @brief Detach and destroy a scene-owned pet host.
 *
 * Safe to call on a zero-initialized or already-closed host.
 *
 * @param host Host instance.
 */
void pet_buddy_detach_scene_pet(pet_host_t *host);

#ifdef __cplusplus
}
#endif
