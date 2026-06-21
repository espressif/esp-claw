/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "app_config.h"
#include "esp_err.h"
#include "http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_err_t (*open_default_mode)(void);
    /* Optional, only needed for the on-screen (device-driven) binding flow:
     * load the current persisted config and save it back. Used to persist the
     * WeChat credentials obtained after a successful scan, mirroring what the
     * web "Save" button does via /api/config. */
    esp_err_t (*load_config)(app_config_t *config);
    esp_err_t (*save_config)(const app_config_t *config);
} app_wechat_setup_config_t;

esp_err_t app_wechat_setup_init(const app_wechat_setup_config_t *config);
esp_err_t app_wechat_setup_login_start(const char *account_id, bool force);
esp_err_t app_wechat_setup_login_get_status(http_server_wechat_login_status_t *status);
esp_err_t app_wechat_setup_login_cancel(void);
esp_err_t app_wechat_setup_login_mark_persisted(void);

/* Drives the full on-screen WeChat binding to completion: starts the QR login,
 * then polls the status (refreshing the on-screen QR/status text) until the user
 * confirms in WeChat. On completion it persists the credentials and switches to
 * the default product mode (the "next step"). Blocking — run it on its own task
 * with an adequate stack, never on the Wi-Fi event task. */
esp_err_t app_wechat_setup_run_screen_binding(void);

#ifdef __cplusplus
}
#endif
