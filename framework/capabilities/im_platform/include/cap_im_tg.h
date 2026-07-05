/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "claw_cap.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *storage_root_dir;
    size_t max_inbound_file_bytes;
    bool enable_inbound_attachments;
} cap_im_tg_attachment_config_t;

esp_err_t cap_im_tg_register_group(void);
esp_err_t cap_im_tg_set_token(const char *bot_token);
esp_err_t cap_im_tg_set_attachment_config(
    const cap_im_tg_attachment_config_t *config);
esp_err_t cap_im_tg_start(void);
esp_err_t cap_im_tg_stop(void);
esp_err_t cap_im_tg_send_text(const char *chat_id, const char *text);
esp_err_t cap_im_tg_send_image(const char *chat_id, const char *path, const char *caption);
esp_err_t cap_im_tg_send_file(const char *chat_id, const char *path, const char *caption);

#ifdef __cplusplus
}
#endif
