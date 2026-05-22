/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t rover_s3_httpd_start(void);
esp_err_t rover_s3_httpd_stop(void);

/* Called by web_send_message cap execute — stores LLM response for /chat_result */
void rover_s3_httpd_set_response(const char *message);

#ifdef __cplusplus
}
#endif
