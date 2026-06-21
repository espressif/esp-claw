/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_mmap_assets.h"
#include "gfx.h"
#include "pet_buddy.h"

/**
 * @file pet_renderer.h
 * @brief Pet package renderer API used by Pet Buddy hosts.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Standard pet action names understood by the renderer.
 */
#define PET_RENDERER_ACTION_IDLE          PET_BUDDY_ACTION_IDLE
#define PET_RENDERER_ACTION_RUNNING       PET_BUDDY_ACTION_RUNNING
#define PET_RENDERER_ACTION_RUNNING_LEFT  PET_BUDDY_ACTION_RUNNING_LEFT
#define PET_RENDERER_ACTION_RUNNING_RIGHT PET_BUDDY_ACTION_RUNNING_RIGHT
#define PET_RENDERER_ACTION_WAVE          PET_BUDDY_ACTION_WAVE
#define PET_RENDERER_ACTION_JUMP          PET_BUDDY_ACTION_JUMP
#define PET_RENDERER_ACTION_LOSE          PET_BUDDY_ACTION_LOSE
#define PET_RENDERER_ACTION_SLEEP         PET_BUDDY_ACTION_SLEEP
#define PET_RENDERER_ACTION_REVIEW        PET_BUDDY_ACTION_REVIEW

/**
 * @brief Default upper bound for a single action asset copied into heap.
 */
#define PET_RENDERER_DEFAULT_ACTION_ASSET_MAX_BYTES (512U * 1024U)

/**
 * @brief Host layout page used when placing the pet object.
 */
typedef enum {
    /** Compact home-page placement. */
    PET_RENDERER_PAGE_HOME = 0,
    /** Larger detail-page placement. */
    PET_RENDERER_PAGE_DETAIL,
} pet_renderer_page_t;

/**
 * @brief Opaque pet renderer instance.
 */
typedef struct {
    /** Private implementation pointer. Do not access directly. */
    void *impl;
} pet_renderer_t;

/**
 * @brief Pet renderer creation options.
 */
typedef struct {
    /** Target GFX display used to create and draw the animated object. */
    gfx_disp_t *display;
    /** Maximum bytes allowed for a single action asset. Zero uses the default limit. */
    size_t max_action_asset_bytes;
} pet_renderer_config_t;

/**
 * @brief Create a pet renderer and load the selected or fallback pet package.
 *
 * @param[out] renderer Renderer instance to initialize.
 * @param config Creation options.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_renderer_create(pet_renderer_t *renderer, const pet_renderer_config_t *config);

/**
 * @brief Destroy a pet renderer and release owned assets.
 *
 * Safe to call on a zero-initialized or already-destroyed renderer.
 *
 * @param renderer Renderer instance.
 */
void pet_renderer_destroy(pet_renderer_t *renderer);

/**
 * @brief Load a pet package from an mmap asset handle.
 *
 * The package should contain index.json or action image entries that can be
 * matched to standard action names.
 *
 * @param renderer Renderer instance.
 * @param assets_handle mmap asset handle to use.
 * @param take_ownership true if the renderer should destroy assets_handle.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_renderer_load_package(pet_renderer_t *renderer,
                                    mmap_assets_handle_t assets_handle,
                                    bool take_ownership);

/**
 * @brief Switch the current pet action.
 *
 * @param renderer Renderer instance.
 * @param action Action name, for example PET_RENDERER_ACTION_IDLE.
 * @param keep_pos true to keep the current object position.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_renderer_set_action(pet_renderer_t *renderer, const char *action, bool keep_pos);

/**
 * @brief Show or hide the pet object.
 *
 * @param renderer Renderer instance.
 * @param visible true to show, false to hide.
 */
void pet_renderer_set_visible(pet_renderer_t *renderer, bool visible);

/**
 * @brief Place the pet object for a host page layout.
 *
 * @param renderer Renderer instance.
 * @param page Host page layout.
 */
void pet_renderer_place(pet_renderer_t *renderer, pet_renderer_page_t page);

/**
 * @brief Feed a touch event to the renderer for drag handling.
 *
 * @param renderer Renderer instance.
 * @param event GFX touch event.
 */
void pet_renderer_handle_touch(pet_renderer_t *renderer, const gfx_touch_event_t *event);

/**
 * @brief Get the underlying GFX object.
 *
 * @param renderer Renderer instance.
 * @return GFX object pointer, or NULL when unavailable.
 */
gfx_obj_t *pet_renderer_object(pet_renderer_t *renderer);

/**
 * @brief Advance renderer animation state.
 *
 * Hosts should call this from their display/session tick.
 *
 * @param renderer Renderer instance.
 * @param now_ms Monotonic timestamp in milliseconds.
 */
void pet_renderer_tick(pet_renderer_t *renderer, uint32_t now_ms);

/**
 * @brief Draw the current animation frame.
 *
 * @param renderer Renderer instance.
 * @return ESP_OK on success, or an ESP error code.
 */
esp_err_t pet_renderer_draw_frame(pet_renderer_t *renderer);

/**
 * @brief Get the preferred pet object size.
 *
 * Either output pointer may be NULL.
 *
 * @param renderer Renderer instance.
 * @param[out] width Preferred width in pixels.
 * @param[out] height Preferred height in pixels.
 */
void pet_renderer_get_preferred_size(pet_renderer_t *renderer, uint16_t *width, uint16_t *height);

#ifdef __cplusplus
}
#endif
