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
    const char *channel;
    const char *chat_id;
    const char *sender_id;
    const char *message_id;
    const char *text;
    /** Optional outbound attachment / download link (e.g. /files/...). */
    const char *link_url;
    /** Optional short label for link_url. */
    const char *link_label;
    int64_t timestamp_ms;
} cap_im_local_message_t;

typedef esp_err_t (*cap_im_local_outbound_callback_t)(const cap_im_local_message_t *message,
                                                      void *user_ctx);

typedef struct {
    const char *default_channel;
    const char *default_sender_id;
    bool log_outbound_messages;
} cap_im_local_config_t;

esp_err_t cap_im_local_register_group(void);
esp_err_t cap_im_local_set_config(const cap_im_local_config_t *config);
esp_err_t cap_im_local_set_outbound_callback(cap_im_local_outbound_callback_t callback,
                                             void *user_ctx);
esp_err_t cap_im_local_start(void);
esp_err_t cap_im_local_stop(void);
esp_err_t cap_im_local_send_text(const char *channel,
                                 const char *chat_id,
                                 const char *message_id,
                                 const char *text);
esp_err_t cap_im_local_send_text_with_link(const char *channel,
                                           const char *chat_id,
                                           const char *message_id,
                                           const char *text,
                                           const char *link_url,
                                           const char *link_label);
esp_err_t cap_im_local_emit_text(const char *channel,
                                 const char *chat_id,
                                 const char *sender_id,
                                 const char *message_id,
                                 const char *text);
esp_err_t cap_im_local_emit_user_message(const char *channel,
                                         const char *chat_id,
                                         const char *sender_id,
                                         const char *message_id,
                                         const char *text,
                                         const char *const *link_urls,
                                         size_t link_count);

#ifdef __cplusplus
}
#endif
