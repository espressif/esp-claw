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
} cap_im_qq_attachment_config_t;

esp_err_t cap_im_qq_register_group(void);
esp_err_t cap_im_qq_set_credentials(const char *app_id, const char *app_secret);
void cap_im_qq_set_msg_type(int msg_type);
esp_err_t cap_im_qq_set_attachment_config(
    const cap_im_qq_attachment_config_t *config);
esp_err_t cap_im_qq_start(void);
esp_err_t cap_im_qq_stop(void);
esp_err_t cap_im_qq_send_text(const char *chat_id, const char *text);
esp_err_t cap_im_qq_send_image(const char *chat_id, const char *path, const char *caption);
esp_err_t cap_im_qq_send_file(const char *chat_id, const char *path, const char *caption);

#ifdef __cplusplus
}
#endif
