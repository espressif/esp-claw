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

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *token;
    const char *base_url;
    const char *cdn_base_url;
    const char *account_id;
    const char *app_id;
    const char *client_version;
    const char *route_tag;
} cap_im_wechat_client_config_t;

typedef struct {
    const char *storage_root_dir;
    size_t max_inbound_file_bytes;
    bool enable_inbound_attachments;
} cap_im_wechat_attachment_config_t;

typedef struct {
    bool active;
    bool configured;
    bool completed;
    bool persisted;
    char session_key[64];
    char status[32];
    char message[160];
    char qrcode[96];          /* Bare WeChat qrcode token (the `qrcode=` URL param only);
                                 NOT scannable on its own. */
    char qr_data_url[256];    /* Full scannable login URL
                                 (https://liteapp.weixin.qq.com/q/<code>?qrcode=<token>&bot_type=3).
                                 Encode THIS into a QR — both web and on-device. */
    char account_id[64];
    char user_id[96];
    char token[256];
    char base_url[160];
} cap_im_wechat_qr_login_status_t;

esp_err_t cap_im_wechat_register_group(void);
esp_err_t cap_im_wechat_set_client_config(const cap_im_wechat_client_config_t *config);
esp_err_t cap_im_wechat_set_attachment_config(
    const cap_im_wechat_attachment_config_t *config);
esp_err_t cap_im_wechat_start(void);
esp_err_t cap_im_wechat_stop(void);
esp_err_t cap_im_wechat_send_text(const char *chat_id, const char *text);
esp_err_t cap_im_wechat_send_image(const char *chat_id, const char *path, const char *caption);
esp_err_t cap_im_wechat_qr_login_start(const char *account_id, bool force);
esp_err_t cap_im_wechat_qr_login_get_status(cap_im_wechat_qr_login_status_t *out_status);
esp_err_t cap_im_wechat_qr_login_cancel(void);
esp_err_t cap_im_wechat_qr_login_mark_persisted(void);

#ifdef __cplusplus
}
#endif
