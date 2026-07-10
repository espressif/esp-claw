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
    const char *platform;
    const char *attachment_kind;
    const char *saved_path;
    const char *saved_dir;
    const char *saved_name;
    const char *original_filename;
    const char *mime;
    const char *caption;
    const char *source_key;
    const char *source_value;
    size_t size_bytes;
    int64_t saved_at_ms;
} cap_im_attachment_payload_config_t;

const char *cap_im_attachment_ext_from_mime(const char *mime);
const char *cap_im_attachment_guess_extension(const char *path_or_url,
                                              const char *original_filename,
                                              const char *mime);
const char *cap_im_attachment_normalize_url(const char *url,
                                            char *buf,
                                            size_t buf_size);
esp_err_t cap_im_attachment_build_saved_paths(const char *root_dir,
                                              const char *platform_dir,
                                              const char *chat_id,
                                              const char *message_id,
                                              const char *kind,
                                              const char *extension,
                                              char *saved_dir,
                                              size_t saved_dir_size,
                                              char *saved_name,
                                              size_t saved_name_size,
                                              char *saved_path,
                                              size_t saved_path_size);
esp_err_t cap_im_attachment_download_url_to_file(const char *log_tag,
                                                 const char *url,
                                                 const char *dest_path,
                                                 size_t max_bytes,
                                                 size_t *out_bytes);
esp_err_t cap_im_attachment_save_buffer_to_file(const char *log_tag,
                                                const char *dest_path,
                                                const unsigned char *buf,
                                                size_t buf_len);
char *cap_im_attachment_build_payload_json(
    const cap_im_attachment_payload_config_t *config);

#ifdef __cplusplus
}
#endif
