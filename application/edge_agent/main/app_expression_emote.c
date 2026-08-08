/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#if CONFIG_EDGE_AGENT_ENABLE_EMOTE

#include "app_expression_emote.h"

#include <stdio.h>
#include <string.h>

#include "display_service.h"
#include "esp_board_manager_includes.h"
#include "esp_check.h"
#include "esp_log.h"
#include "expression_emote.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gfx.h"
#include "system_ui.h"

#define EMOTE_ASSETS_PARTITION "emote"
#define EMOTE_DISPLAY_OWNER "esp_claw_emote"

static const char *TAG = "app_emote";

static int s_lcd_width;
static int s_lcd_height;
static emote_handle_t s_emote_handle;
static display_service_session_handle_t s_display_session;
static SemaphoreHandle_t s_session_mutex;

static bool app_emote_should_swap_color(const dev_display_lcd_config_t *lcd_cfg)
{
    if (lcd_cfg == NULL || lcd_cfg->sub_type == NULL) {
        return true;
    }

    return strcmp(lcd_cfg->sub_type, "dsi") != 0 &&
           strcmp(lcd_cfg->sub_type, "mipi_dsi") != 0 &&
           strcmp(lcd_cfg->sub_type, "rgb") != 0;
}

static void app_emote_flush_callback(int x_start, int y_start, int x_end, int y_end,
                                     const void *data, emote_handle_t handle)
{
    esp_err_t err = ESP_OK;

    if (s_session_mutex != NULL &&
            xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (s_display_session != NULL) {
            err = display_service_session_raw_blit(s_display_session,
                                                    &(display_service_raw_blit_t) {
                                                        .x_start = x_start,
                                                        .y_start = y_start,
                                                        .x_end = x_end,
                                                        .y_end = y_end,
                                                        .frame_buffer = data,
                                                        .wait = true,
                                                    });
        }
        xSemaphoreGive(s_session_mutex);
    }

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "emote frame flush failed: %s", esp_err_to_name(err));
    }
    if (handle != NULL) {
        emote_notify_flush_finished(handle);
    }
}

static void app_emote_update_callback(gfx_disp_event_t event, const void *obj,
                                      emote_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    gfx_obj_t *wait_obj = emote_get_obj_by_name(handle, EMT_DEF_ELEM_EMERG_DLG);
    if (wait_obj == obj && event == GFX_DISP_EVENT_ALL_FRAME_DONE) {
        ESP_LOGI(TAG, "Emergency dialog finished");
    }
}

static void app_emote_display_resume_callback(display_service_session_handle_t session,
                                              void *user_ctx)
{
    (void)user_ctx;

    if (session != s_display_session || s_emote_handle == NULL) {
        return;
    }
    esp_err_t err = emote_notify_all_refresh(s_emote_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "refresh resumed emote failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t app_emote_load_board_display(dev_display_lcd_config_t **ret_lcd_cfg)
{
    void *lcd_handle = NULL;
    void *lcd_config = NULL;

    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_handle(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                             &lcd_handle),
                        TAG, "get display handle failed");
    ESP_RETURN_ON_ERROR(esp_board_manager_get_device_config(ESP_BOARD_DEVICE_NAME_DISPLAY_LCD,
                                                             &lcd_config),
                        TAG, "get display config failed");
    ESP_RETURN_ON_FALSE(lcd_handle != NULL && lcd_config != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "display handle/config is NULL");

    dev_display_lcd_handles_t *lcd_handles = (dev_display_lcd_handles_t *)lcd_handle;
    dev_display_lcd_config_t *lcd_cfg = (dev_display_lcd_config_t *)lcd_config;
    ESP_RETURN_ON_FALSE(lcd_handles->panel_handle != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "display panel handle is NULL");

    s_lcd_width = lcd_cfg->lcd_width;
    s_lcd_height = lcd_cfg->lcd_height;
    *ret_lcd_cfg = lcd_cfg;
    return ESP_OK;
}

static emote_config_t app_emote_default_config(const dev_display_lcd_config_t *lcd_cfg)
{
    return (emote_config_t) {
        .flags = {
            .swap = app_emote_should_swap_color(lcd_cfg),
            .double_buffer = true,
            .buff_dma = true,
        },
        .gfx_emote = {
            .h_res = s_lcd_width,
            .v_res = s_lcd_height,
            .fps = 10,
        },
        .buffers = {
            .buf_pixels = (size_t)s_lcd_width * 16,
        },
        .task = {
            .task_priority = 3,
            .task_stack = 12 * 1024,
            .task_affinity = -1,
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
            .task_stack_in_ext = true,
#else
            .task_stack_in_ext = false,
#endif
        },
        .flush_cb = app_emote_flush_callback,
        .update_cb = app_emote_update_callback,
    };
}

static esp_err_t app_emote_show(void)
{
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(s_session_mutex != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "session mutex is NULL");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "session mutex timeout");
    if (s_display_session == NULL) {
        err = display_service_open(&(display_service_session_config_t) {
                                       .owner_name = EMOTE_DISPLAY_OWNER,
                                       .mode = DISPLAY_SERVICE_MODE_EXCLUSIVE_RAW,
                                       .flags = DISPLAY_SERVICE_SESSION_FLAG_ALLOW_SYSTEM_OVERLAY |
                                                DISPLAY_SERVICE_SESSION_FLAG_PREEMPTIBLE,
                                       .resume_cb = app_emote_display_resume_callback,
                                   },
                                   &s_display_session);
    }
    xSemaphoreGive(s_session_mutex);

    if (err == ESP_OK && s_emote_handle != NULL) {
        err = emote_notify_all_refresh(s_emote_handle);
    }
    return err;
}

static esp_err_t app_emote_hide(void)
{
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(s_session_mutex != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "session mutex is NULL");
    ESP_RETURN_ON_FALSE(xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(1000)) == pdTRUE,
                        ESP_ERR_TIMEOUT, TAG, "session mutex timeout");
    if (s_display_session != NULL) {
        display_service_session_handle_t session = s_display_session;
        s_display_session = NULL;
        err = display_service_close(session);
        if (err != ESP_OK) {
            s_display_session = session;
        }
    }
    xSemaphoreGive(s_session_mutex);
    return err;
}

static esp_err_t app_emote_page_swipe_cb(system_ui_page_swipe_direction_t direction,
                                         void *user_ctx)
{
    (void)user_ctx;

    if (direction == SYSTEM_UI_PAGE_SWIPE_PREVIOUS && s_display_session == NULL) {
        return app_emote_show();
    }
    if (direction == SYSTEM_UI_PAGE_SWIPE_NEXT && s_display_session != NULL) {
        return app_emote_hide();
    }
    return ESP_OK;
}

static esp_err_t app_emote_apply(const char *idle, const char *msg)
{
    ESP_RETURN_ON_FALSE(s_emote_handle != NULL, ESP_ERR_INVALID_STATE,
                        TAG, "emote handle is NULL");
    ESP_RETURN_ON_ERROR(emote_set_event_msg(s_emote_handle, EMOTE_MGR_EVT_SYS, msg),
                        TAG, "set emote message failed");
    ESP_RETURN_ON_ERROR(emote_set_anim_emoji(s_emote_handle, idle),
                        TAG, "set emote animation failed");

    gfx_obj_t *toast_obj = emote_get_obj_by_name(s_emote_handle, EMT_DEF_ELEM_TOAST_LABEL);
    if (toast_obj != NULL) {
        gfx_obj_align(toast_obj, GFX_ALIGN_TOP_MID, 0, 70);
    }
    gfx_obj_t *icon_obj = emote_get_obj_by_name(s_emote_handle, EMT_DEF_ELEM_STATUS_ICON);
    if (icon_obj != NULL) {
        gfx_obj_set_pos(icon_obj, 10, 78);
    }

    if (s_display_session != NULL) {
        return emote_notify_all_refresh(s_emote_handle);
    }
    return ESP_OK;
}

esp_err_t app_expression_emote_set_status(bool sta_connected,
                                          const char *ap_ssid,
                                          const char *sta_ip)
{
    const bool ap_present = ap_ssid != NULL && ap_ssid[0] != '\0';
    const char *idle = sta_connected ? "swim" : "offline";
    const char *ip = sta_ip != NULL && sta_ip[0] != '\0' ? sta_ip : "0.0.0.0";
    char msg[96];

    if (sta_connected && ap_present) {
        snprintf(msg, sizeof(msg), "Online * AP: %s  IP: %s", ap_ssid, ip);
    } else if (sta_connected) {
        snprintf(msg, sizeof(msg), "Wi-Fi connected  IP: %s", ip);
    } else if (ap_present) {
        snprintf(msg, sizeof(msg), "Setup WiFi: %s", ap_ssid);
    } else {
        snprintf(msg, sizeof(msg), "Wi-Fi offline");
    }

    ESP_LOGI(TAG, "Update network emote: idle=%s msg=\"%s\"", idle, msg);
    return app_emote_apply(idle, msg);
}

static void app_emote_cleanup(void)
{
    (void)app_emote_hide();
    (void)system_ui_set_page_swipe_callback(NULL, NULL);
    if (s_emote_handle != NULL) {
        emote_deinit(s_emote_handle);
        s_emote_handle = NULL;
    }
    if (s_session_mutex != NULL) {
        vSemaphoreDelete(s_session_mutex);
        s_session_mutex = NULL;
    }
}

esp_err_t app_expression_emote_start(void)
{
    dev_display_lcd_config_t *lcd_cfg = NULL;
    emote_data_t data = {
        .type = EMOTE_SOURCE_PARTITION,
        .source = {
            .partition_label = EMOTE_ASSETS_PARTITION,
        },
        .flags = {
#ifdef CONFIG_SPIRAM_XIP_FROM_PSRAM
            .mmap_enable = false,
#else
            .mmap_enable = true,
#endif
        },
    };

    if (s_emote_handle != NULL) {
        return ESP_OK;
    }

    s_session_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_session_mutex != NULL, ESP_ERR_NO_MEM,
                        TAG, "create session mutex failed");

    esp_err_t err = app_emote_load_board_display(&lcd_cfg);
    if (err != ESP_OK) {
        app_emote_cleanup();
        return err;
    }

    emote_config_t config = app_emote_default_config(lcd_cfg);
    s_emote_handle = emote_init(&config);
    if (s_emote_handle == NULL || !emote_is_initialized(s_emote_handle)) {
        app_emote_cleanup();
        return ESP_FAIL;
    }

    err = emote_mount_and_load_assets(s_emote_handle, &data);
    if (err == ESP_OK) {
        err = app_expression_emote_set_status(false, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = system_ui_set_page_swipe_callback(app_emote_page_swipe_cb, NULL);
    }
    if (err == ESP_OK) {
        err = app_emote_show();
    }
    if (err != ESP_OK) {
        app_emote_cleanup();
        return err;
    }

    ESP_LOGI(TAG, "Expression emote initialized as the default display page");
    return ESP_OK;
}

#endif
