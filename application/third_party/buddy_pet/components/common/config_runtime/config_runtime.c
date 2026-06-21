/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "config_runtime.h"

#include <string.h>

#include "display_session.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gfx.h"
#include "lvgl.h"
#include "widget/gfx_qrcode.h"

#define CONFIG_RUNTIME_TEXT_MAX 320
#define CONFIG_RUNTIME_TITLE_H  30
#define CONFIG_RUNTIME_HINT_H   28
#define CONFIG_RUNTIME_STATUS_H 24
#define CONFIG_RUNTIME_MARGIN   10

static const char *TAG = "config_runtime";

typedef struct {
    SemaphoreHandle_t lock;
    display_session_t *session;
    gfx_obj_t *qr_obj;
    gfx_obj_t *title_label;
    gfx_obj_t *hint_label;
    gfx_obj_t *status_label;
    char title[CONFIG_RUNTIME_TEXT_MAX];
    char qr_payload[CONFIG_RUNTIME_TEXT_MAX];
    char hint[CONFIG_RUNTIME_TEXT_MAX];
    char status[CONFIG_RUNTIME_TEXT_MAX];
} config_runtime_state_t;

static config_runtime_state_t s_config_runtime;

static esp_err_t config_runtime_ensure_lock(void)
{
    if (s_config_runtime.lock == NULL) {
        s_config_runtime.lock = xSemaphoreCreateMutex();
    }
    ESP_RETURN_ON_FALSE(s_config_runtime.lock != NULL, ESP_ERR_NO_MEM, TAG, "create lock failed");
    return ESP_OK;
}

static gfx_color_t config_runtime_color(gfx_color_t color)
{
    if (display_session_should_swap_color(s_config_runtime.session)) {
        color.full = (uint16_t)((color.full << 8) | (color.full >> 8));
    }
    return color;
}

static void config_runtime_delete_ui_locked(void)
{
    if (s_config_runtime.qr_obj != NULL) {
        (void)gfx_obj_delete(s_config_runtime.qr_obj);
        s_config_runtime.qr_obj = NULL;
    }
    if (s_config_runtime.title_label != NULL) {
        (void)gfx_obj_delete(s_config_runtime.title_label);
        s_config_runtime.title_label = NULL;
    }
    if (s_config_runtime.hint_label != NULL) {
        (void)gfx_obj_delete(s_config_runtime.hint_label);
        s_config_runtime.hint_label = NULL;
    }
    if (s_config_runtime.status_label != NULL) {
        (void)gfx_obj_delete(s_config_runtime.status_label);
        s_config_runtime.status_label = NULL;
    }
}

static esp_err_t config_runtime_create_label_locked(gfx_disp_t *disp,
                                                    gfx_obj_t **out_label,
                                                    const char *text,
                                                    gfx_coord_t y,
                                                    gfx_coord_t h,
                                                    gfx_color_t color)
{
    gfx_obj_t *label;
    uint32_t screen_w;

    ESP_RETURN_ON_FALSE(disp != NULL && out_label != NULL && s_config_runtime.session != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid label args");

    screen_w = display_session_width(s_config_runtime.session);
    label = gfx_label_create(disp);
    ESP_RETURN_ON_FALSE(label != NULL, ESP_ERR_NO_MEM, TAG, "create label failed");

    (void)gfx_obj_set_pos(label, 0, y);
    (void)gfx_obj_set_size(label, (gfx_coord_t)screen_w, h);
    (void)gfx_label_set_font(label, (gfx_font_t)LV_FONT_DEFAULT);
    (void)gfx_label_set_color(label, config_runtime_color(color));
    (void)gfx_label_set_bg_enable(label, false);
    (void)gfx_label_set_text_align(label, GFX_TEXT_ALIGN_CENTER);
    (void)gfx_label_set_long_mode(label, GFX_LABEL_LONG_SCROLL);
    (void)gfx_label_set_text(label, text ? text : "");
    *out_label = label;
    return ESP_OK;
}

static esp_err_t config_runtime_create_ui_locked(void)
{
    gfx_disp_t *disp;
    uint32_t screen_w;
    uint32_t screen_h;
    uint32_t qr_size;
    gfx_coord_t qr_x;
    gfx_coord_t qr_y;

    ESP_RETURN_ON_FALSE(s_config_runtime.session != NULL, ESP_ERR_INVALID_STATE, TAG, "display session not started");
    ESP_RETURN_ON_FALSE(s_config_runtime.qr_payload[0] != '\0', ESP_ERR_INVALID_ARG, TAG, "qr payload is empty");

    disp = display_session_display(s_config_runtime.session);
    screen_w = display_session_width(s_config_runtime.session);
    screen_h = display_session_height(s_config_runtime.session);
    qr_size = screen_w < screen_h ? screen_w : screen_h;
    if (qr_size > 168) {
        qr_size = 168;
    }
    if (qr_size + CONFIG_RUNTIME_TITLE_H + CONFIG_RUNTIME_HINT_H + CONFIG_RUNTIME_STATUS_H +
            (CONFIG_RUNTIME_MARGIN * 2) > screen_h) {
        qr_size = screen_h - CONFIG_RUNTIME_TITLE_H - CONFIG_RUNTIME_HINT_H -
                  CONFIG_RUNTIME_STATUS_H - (CONFIG_RUNTIME_MARGIN * 2);
    }
    if (qr_size < 96) {
        qr_size = 96;
    }
    qr_x = (gfx_coord_t)((screen_w > qr_size) ? ((screen_w - qr_size) / 2) : 0);
    qr_y = CONFIG_RUNTIME_TITLE_H + CONFIG_RUNTIME_MARGIN;

    config_runtime_delete_ui_locked();
    (void)gfx_disp_set_bg_color(disp, GFX_COLOR_HEX(0x101214));

    ESP_RETURN_ON_ERROR(config_runtime_create_label_locked(disp,
                                                           &s_config_runtime.title_label,
                                                           s_config_runtime.title,
                                                           2,
                                                           CONFIG_RUNTIME_TITLE_H,
                                                           GFX_COLOR_HEX(0xFFFFFF)),
                        TAG, "create title label failed");

    s_config_runtime.qr_obj = gfx_qrcode_create(disp);
    ESP_RETURN_ON_FALSE(s_config_runtime.qr_obj != NULL, ESP_ERR_NO_MEM, TAG, "create qr failed");
    ESP_RETURN_ON_ERROR(gfx_qrcode_set_size(s_config_runtime.qr_obj, (uint16_t)qr_size), TAG, "set qr size failed");
    ESP_RETURN_ON_ERROR(gfx_qrcode_set_color(s_config_runtime.qr_obj, GFX_COLOR_HEX(0x000000)),
                        TAG, "set qr color failed");
    ESP_RETURN_ON_ERROR(gfx_qrcode_set_bg_color(s_config_runtime.qr_obj, GFX_COLOR_HEX(0xFFFFFF)),
                        TAG, "set qr bg failed");
    /* ECC LOW (not MEDIUM): the gfx_qrcode encoder is hard-capped at QR version 5
     * (qrcode_wrapper max_qrcode_version=5). At ECC MEDIUM v5 holds only 84 bytes,
     * but the WeChat login URL is ~89 bytes and fails to encode
     * ("qrcode_lib: Failed to encode QR Code"). ECC LOW raises v5 capacity to
     * 106 bytes; on-device close-range scanning does not need stronger ECC. */
    ESP_RETURN_ON_ERROR(gfx_qrcode_set_ecc(s_config_runtime.qr_obj, GFX_QRCODE_ECC_LOW),
                        TAG, "set qr ecc failed");
    ESP_RETURN_ON_ERROR(gfx_qrcode_set_data(s_config_runtime.qr_obj, s_config_runtime.qr_payload),
                        TAG, "set qr data failed");
    (void)gfx_obj_set_pos(s_config_runtime.qr_obj, qr_x, qr_y);
    (void)gfx_obj_set_size(s_config_runtime.qr_obj, (gfx_coord_t)qr_size, (gfx_coord_t)qr_size);

    ESP_RETURN_ON_ERROR(config_runtime_create_label_locked(disp,
                                                           &s_config_runtime.hint_label,
                                                           s_config_runtime.hint,
                                                           qr_y + (gfx_coord_t)qr_size + 4,
                                                           CONFIG_RUNTIME_HINT_H,
                                                           GFX_COLOR_HEX(0xFFFFFF)),
                        TAG, "create hint label failed");
    ESP_RETURN_ON_ERROR(config_runtime_create_label_locked(disp,
                                                           &s_config_runtime.status_label,
                                                           s_config_runtime.status,
                                                           qr_y + (gfx_coord_t)qr_size + CONFIG_RUNTIME_HINT_H + 2,
                                                           CONFIG_RUNTIME_STATUS_H,
                                                           GFX_COLOR_HEX(0xB8C0CC)),
                        TAG, "create status label failed");
    gfx_disp_refresh_all(disp);
    return ESP_OK;
}

esp_err_t config_runtime_show_qr(const char *title,
                                 const char *qr_payload,
                                 const char *hint,
                                 const char *status)
{
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(qr_payload != NULL && qr_payload[0] != '\0',
                        ESP_ERR_INVALID_ARG, TAG, "qr payload is required");
    ESP_RETURN_ON_ERROR(config_runtime_ensure_lock(), TAG, "ensure lock failed");

    xSemaphoreTake(s_config_runtime.lock, portMAX_DELAY);
    strlcpy(s_config_runtime.title, title ? title : "", sizeof(s_config_runtime.title));
    strlcpy(s_config_runtime.qr_payload, qr_payload, sizeof(s_config_runtime.qr_payload));
    strlcpy(s_config_runtime.hint, hint ? hint : "", sizeof(s_config_runtime.hint));
    strlcpy(s_config_runtime.status, status ? status : "", sizeof(s_config_runtime.status));

    if (s_config_runtime.session == NULL) {
        const display_session_config_t session_config = {0};
        ret = display_session_start(&s_config_runtime.session, &session_config);
        if (ret != ESP_OK) {
            xSemaphoreGive(s_config_runtime.lock);
            return ret;
        }
    }

    ret = display_session_lock(s_config_runtime.session);
    if (ret == ESP_OK) {
        ret = config_runtime_create_ui_locked();
        display_session_unlock(s_config_runtime.session);
    }
    xSemaphoreGive(s_config_runtime.lock);
    return ret;
}

esp_err_t config_runtime_clear_qr(void)
{
    ESP_RETURN_ON_ERROR(config_runtime_ensure_lock(), TAG, "ensure lock failed");

    xSemaphoreTake(s_config_runtime.lock, portMAX_DELAY);
    s_config_runtime.title[0] = '\0';
    s_config_runtime.qr_payload[0] = '\0';
    s_config_runtime.hint[0] = '\0';
    s_config_runtime.status[0] = '\0';
    if (s_config_runtime.session != NULL) {
        if (display_session_lock(s_config_runtime.session) == ESP_OK) {
            gfx_disp_t *disp = display_session_display(s_config_runtime.session);
            config_runtime_delete_ui_locked();
            if (disp != NULL) {
                (void)gfx_disp_set_bg_color(disp, GFX_COLOR_HEX(0x000000));
                gfx_disp_refresh_all(disp);
            }
            display_session_unlock(s_config_runtime.session);
        }
        display_session_stop(&s_config_runtime.session);
    }
    xSemaphoreGive(s_config_runtime.lock);
    return ESP_OK;
}
