/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "emote.h"

#include <stdio.h>
#include <string.h>

#include "display_session.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mmap_assets.h"
#include "gfx.h"
#include "lvgl.h"

static const char *TAG = "app_emote";

#define EMOTE_STATUS_MAX 96
#define EMOTE_ASSETS_PARTITION "emote"
#define EMOTE_ANIM_ONLINE "swim.eaf"
#define EMOTE_ANIM_OFFLINE "offline.eaf"
#define EMOTE_ANIM_Y_OFFSET 20
#define EMOTE_TITLE_Y 4
#define EMOTE_TITLE_H 28

static display_session_t *s_display_session;
static mmap_assets_handle_t s_assets_handle;
static gfx_obj_t *s_anim_obj;
static gfx_obj_t *s_title_label;
static char s_status_text[EMOTE_STATUS_MAX] = "Wi-Fi offline";
static bool s_sta_connected;
static bool s_started;

static gfx_color_t emote_color(gfx_color_t color)
{
    if (display_session_should_swap_color(s_display_session)) {
        color.full = (uint16_t)((color.full << 8) | (color.full >> 8));
    }
    return color;
}

static int emote_find_asset_id_by_name(const char *filename)
{
    if (s_assets_handle == NULL || filename == NULL) {
        return -1;
    }

    int files = mmap_assets_get_stored_files(s_assets_handle);
    for (int i = 0; i < files; i++) {
        const char *name = mmap_assets_get_name(s_assets_handle, i);
        if (name != NULL && strcmp(name, filename) == 0) {
            return i;
        }
    }

    return -1;
}

static esp_err_t emote_mount_assets(void)
{
    if (s_assets_handle != NULL) {
        return ESP_OK;
    }

    const mmap_assets_config_t asset_config = {
        .partition_label = EMOTE_ASSETS_PARTITION,
        .flags = {
            .mmap_enable = true,
            .use_fs = false,
            .app_bin_check = false,
        },
    };

    return mmap_assets_new(&asset_config, &s_assets_handle);
}

static esp_err_t emote_set_anim_locked(const char *filename)
{
    ESP_RETURN_ON_FALSE(s_anim_obj != NULL && s_assets_handle != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "animation is not ready");

    int asset_id = emote_find_asset_id_by_name(filename);
    ESP_RETURN_ON_FALSE(asset_id >= 0, ESP_ERR_NOT_FOUND, TAG, "animation asset not found: %s", filename);

    const void *anim_data = mmap_assets_get_mem(s_assets_handle, asset_id);
    int anim_size = mmap_assets_get_size(s_assets_handle, asset_id);
    ESP_RETURN_ON_FALSE(anim_data != NULL && anim_size > 0,
                        ESP_ERR_INVALID_SIZE, TAG, "invalid animation asset: %s", filename);

    const gfx_anim_src_t anim_src = {
        .type = GFX_ANIM_SRC_TYPE_MEMORY,
        .data = anim_data,
        .data_len = (size_t)anim_size,
    };

    (void)gfx_anim_stop(s_anim_obj);
    ESP_RETURN_ON_ERROR(gfx_anim_set_src_desc(s_anim_obj, &anim_src), TAG, "set animation source failed");
    ESP_RETURN_ON_ERROR(gfx_obj_align(s_anim_obj, GFX_ALIGN_CENTER, 0, EMOTE_ANIM_Y_OFFSET),
                        TAG, "align animation failed");
    ESP_RETURN_ON_ERROR(gfx_anim_set_segment(s_anim_obj, 0, 0xFFFFFFFF, 20, true),
                        TAG, "set animation segment failed");
    return gfx_anim_start(s_anim_obj);
}

static void emote_format_network_status(bool sta_connected, const char *ap_ssid)
{
    const bool ap_present = (ap_ssid != NULL && ap_ssid[0] != '\0');

    if (sta_connected && ap_present) {
        snprintf(s_status_text, sizeof(s_status_text), "Online * AP: %s", ap_ssid);
    } else if (sta_connected) {
        strlcpy(s_status_text, "Wi-Fi connected", sizeof(s_status_text));
    } else if (ap_present) {
        snprintf(s_status_text, sizeof(s_status_text), "Setup WiFi: %s", ap_ssid);
    } else {
        strlcpy(s_status_text, "Wi-Fi offline", sizeof(s_status_text));
    }
}

static esp_err_t emote_create_title_label_locked(gfx_disp_t *disp)
{
    uint32_t screen_w;

    ESP_RETURN_ON_FALSE(disp != NULL && s_display_session != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid title label args");

    screen_w = display_session_width(s_display_session);
    s_title_label = gfx_label_create(disp);
    ESP_RETURN_ON_FALSE(s_title_label != NULL, ESP_ERR_NO_MEM, TAG, "create title label failed");

    (void)gfx_obj_set_pos(s_title_label, 0, EMOTE_TITLE_Y);
    (void)gfx_obj_set_size(s_title_label, (gfx_coord_t)screen_w, EMOTE_TITLE_H);
    (void)gfx_label_set_font(s_title_label, (gfx_font_t)LV_FONT_DEFAULT);
    (void)gfx_label_set_color(s_title_label, emote_color(GFX_COLOR_HEX(0xFFFFFF)));
    (void)gfx_label_set_bg_enable(s_title_label, false);
    (void)gfx_label_set_text_align(s_title_label, GFX_TEXT_ALIGN_CENTER);
    (void)gfx_label_set_long_mode(s_title_label, GFX_LABEL_LONG_SCROLL);
    (void)gfx_label_set_text(s_title_label, s_status_text);
    return ESP_OK;
}

static void emote_delete_ui_locked(void)
{
    if (s_anim_obj != NULL) {
        (void)gfx_anim_stop(s_anim_obj);
        (void)gfx_obj_delete(s_anim_obj);
        s_anim_obj = NULL;
    }
    if (s_title_label != NULL) {
        (void)gfx_obj_delete(s_title_label);
        s_title_label = NULL;
    }
}

static esp_err_t emote_create_ui(void)
{
    gfx_disp_t *disp;
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(s_display_session != NULL, ESP_ERR_INVALID_STATE, TAG, "display session not started");
    ESP_RETURN_ON_ERROR(display_session_lock(s_display_session), TAG, "lock display failed");

    disp = display_session_display(s_display_session);

    emote_delete_ui_locked();
    (void)gfx_disp_set_bg_color(disp, GFX_COLOR_HEX(0x171617));

    s_anim_obj = gfx_anim_create(disp);
    ESP_GOTO_ON_FALSE(s_anim_obj != NULL, ESP_ERR_NO_MEM, err, TAG, "create animation failed");
    (void)gfx_anim_set_auto_mirror(s_anim_obj, false);
    ret = emote_set_anim_locked(s_sta_connected ? EMOTE_ANIM_ONLINE : EMOTE_ANIM_OFFLINE);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "start animation failed");
    ret = emote_create_title_label_locked(disp);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "create title label failed");

    gfx_disp_refresh_all(disp);
    display_session_unlock(s_display_session);
    return ESP_OK;

err:
    emote_delete_ui_locked();
    display_session_unlock(s_display_session);
    return ret;
}

esp_err_t emote_set_network_status(bool sta_connected, const char *ap_ssid)
{
    bool status_changed = s_sta_connected != sta_connected;

    s_sta_connected = sta_connected;
    emote_format_network_status(sta_connected, ap_ssid);

    if (!s_started || s_display_session == NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_session_lock(s_display_session), TAG, "lock display failed");
    if (status_changed && s_anim_obj != NULL && s_assets_handle != NULL) {
        esp_err_t ret = emote_set_anim_locked(s_sta_connected ? EMOTE_ANIM_ONLINE : EMOTE_ANIM_OFFLINE);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "switch animation failed: %s", esp_err_to_name(ret));
        }
    }
    if (s_title_label != NULL) {
        (void)gfx_label_set_text(s_title_label, s_status_text);
    }
    gfx_disp_refresh_all(display_session_display(s_display_session));
    display_session_unlock(s_display_session);
    return ESP_OK;
}

esp_err_t emote_start(void)
{
    esp_err_t ret;

    if (s_started) {
        return ESP_OK;
    }

    const display_session_config_t session_config = {0};
    ESP_RETURN_ON_ERROR(display_session_start(&s_display_session, &session_config),
                        TAG, "start emote display session failed");

    ret = emote_mount_assets();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount emote animation assets failed: %s", esp_err_to_name(ret));
        display_session_stop(&s_display_session);
        return ret;
    }

    s_started = true;
    ret = emote_create_ui();
    if (ret != ESP_OK) {
        s_started = false;
        if (s_assets_handle != NULL) {
            (void)mmap_assets_del(s_assets_handle);
            s_assets_handle = NULL;
        }
        display_session_stop(&s_display_session);
        return ret;
    }
    return ESP_OK;
}
