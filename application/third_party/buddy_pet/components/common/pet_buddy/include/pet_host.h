/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "gfx.h"

/**
 * @file pet_host.h
 * @brief Host-side adapter for embedding a Pet Buddy renderer in a scene.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Scene page layout used when placing a pet host object.
 */
typedef enum {
    /** Compact placement for a home-style scene. */
    PET_HOST_PAGE_HOME = 0,
    /** Larger placement for a detail-style scene. */
    PET_HOST_PAGE_DETAIL,
} pet_host_page_t;

/**
 * @brief Pet host instance owned by a scene.
 */
typedef struct {
    /** Private implementation pointer. Do not access directly. */
    void *impl;
    /** true after pet_host_open() succeeds. */
    bool opened;
} pet_host_t;

/**
 * @brief Pet host creation options.
 */
typedef struct {
    /** Target GFX display for the pet object. */
    gfx_disp_t *display;
    /** Maximum bytes allowed for a single action asset. Zero uses renderer default. */
    size_t max_action_asset_bytes;
} pet_host_config_t;

/**
 * @brief Open a pet host and create its renderer.
 *
 * @param[out] host Host instance to initialize.
 * @param config Host creation options.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_host_open(pet_host_t *host, const pet_host_config_t *config);

/**
 * @brief Close a pet host and release renderer resources.
 *
 * Safe to call on a zero-initialized or already-closed host.
 *
 * @param host Host instance.
 */
void pet_host_close(pet_host_t *host);

/**
 * @brief Set an action on a specific pet host.
 *
 * @param host Host instance.
 * @param action Pet action name.
 * @param keep_pos true to keep the current pet position.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_host_set_action(pet_host_t *host, const char *action, bool keep_pos);

/**
 * @brief Show or hide a host's pet object.
 *
 * @param host Host instance.
 * @param visible true to show, false to hide.
 */
void pet_host_set_visible(pet_host_t *host, bool visible);

/**
 * @brief Place a host's pet object for a scene page layout.
 *
 * @param host Host instance.
 * @param page Target page layout.
 */
void pet_host_place(pet_host_t *host, pet_host_page_t page);

/**
 * @brief Feed a touch event to a host's pet object.
 *
 * @param host Host instance.
 * @param event GFX touch event.
 */
void pet_host_handle_touch(pet_host_t *host, const gfx_touch_event_t *event);

/**
 * @brief Get the GFX object owned by a host.
 *
 * @param host Host instance.
 * @return GFX object pointer, or NULL when unavailable.
 */
gfx_obj_t *pet_host_object(pet_host_t *host);

#ifdef __cplusplus
}
#endif
