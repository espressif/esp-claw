/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_wechat_setup.h"

#include <stdlib.h>
#include <string.h>

#include "app_provisioning.h"
#include "config_runtime.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cap_im_wechat.h"
#endif

static const char *TAG = "app_wechat_setup";

static app_wechat_setup_config_t s_config;

static const char *app_wechat_setup_screen_status(const char *status)
{
    if (status == NULL || status[0] == '\0' || strcmp(status, "waiting_scan") == 0) {
        return "Waiting for scan";
    }
    if (strcmp(status, "scanned") == 0) {
        return "Confirm in WeChat";
    }
    if (strcmp(status, "redirected") == 0) {
        return "Waiting for confirmation";
    }
    if (strcmp(status, "confirmed") == 0) {
        return "Binding complete";
    }
    if (strcmp(status, "expired") == 0) {
        return "QR expired";
    }
    if (strcmp(status, "cancelled") == 0) {
        return "Binding cancelled";
    }
    if (strcmp(status, "error") == 0) {
        return "Binding failed";
    }
    return "Waiting for scan";
}

esp_err_t app_wechat_setup_init(const app_wechat_setup_config_t *config)
{
    if (config != NULL) {
        s_config = *config;
    } else {
        memset(&s_config, 0, sizeof(s_config));
    }
    return ESP_OK;
}

static void app_wechat_setup_show_qr(const char *qr_payload, const char *status_text)
{
    /* The on-device renderer encodes this string into a QR matrix itself, exactly
     * like the web UI does (generate(qr_data_url).toCanvas()). It must therefore be
     * the full scannable login URL (raw->qr_data_url,
     * e.g. https://liteapp.weixin.qq.com/q/<code>?qrcode=<token>&bot_type=3), NOT the
     * bare raw->qrcode token, which scans to a meaningless hex string. */
    if (qr_payload == NULL || qr_payload[0] == '\0') {
        return;
    }

    esp_err_t err = config_runtime_show_qr("",
                                           qr_payload,
                                           "Scan in WeChat",
                                           status_text && status_text[0] ? status_text : "Waiting for scan");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "show WeChat QR failed: %s", esp_err_to_name(err));
    }
}

esp_err_t app_wechat_setup_login_start(const char *account_id, bool force)
{
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    esp_err_t err = cap_im_wechat_qr_login_start(account_id, force);
    if (err != ESP_OK) {
        return err;
    }

    cap_im_wechat_qr_login_status_t *raw = calloc(1, sizeof(*raw));
    if (raw == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = cap_im_wechat_qr_login_get_status(raw);
    if (err == ESP_OK && raw->qr_data_url[0]) {
        app_wechat_setup_show_qr(raw->qr_data_url, app_wechat_setup_screen_status(raw->status));
    }
    free(raw);
    return err;
#else
    (void)account_id;
    (void)force;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t app_wechat_setup_login_get_status(http_server_wechat_login_status_t *status)
{
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    cap_im_wechat_qr_login_status_t *raw = NULL;
    esp_err_t err;

    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    raw = calloc(1, sizeof(*raw));
    if (raw == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = cap_im_wechat_qr_login_get_status(raw);
    if (err != ESP_OK) {
        free(raw);
        return err;
    }

    memset(status, 0, sizeof(*status));
    status->active = raw->active;
    status->configured = raw->configured;
    status->completed = raw->completed;
    status->persisted = raw->persisted;
    strlcpy(status->session_key, raw->session_key, sizeof(status->session_key));
    strlcpy(status->status, raw->status, sizeof(status->status));
    strlcpy(status->message, raw->message, sizeof(status->message));
    strlcpy(status->qr_data_url, raw->qr_data_url, sizeof(status->qr_data_url));
    strlcpy(status->account_id, raw->account_id, sizeof(status->account_id));
    strlcpy(status->user_id, raw->user_id, sizeof(status->user_id));
    strlcpy(status->token, raw->token, sizeof(status->token));
    strlcpy(status->base_url, raw->base_url, sizeof(status->base_url));

    if (raw->qr_data_url[0]) {
        app_wechat_setup_show_qr(raw->qr_data_url, app_wechat_setup_screen_status(raw->status));
    }
    free(raw);
    return ESP_OK;
#else
    (void)status;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t app_wechat_setup_login_cancel(void)
{
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    return cap_im_wechat_qr_login_cancel();
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t app_wechat_setup_login_mark_persisted(void)
{
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    esp_err_t err = cap_im_wechat_qr_login_mark_persisted();
    if (err == ESP_OK && s_config.open_default_mode != NULL) {
        err = s_config.open_default_mode();
        if (err == ESP_OK) {
            app_provisioning_publish_match_watch_network_ready_force("wechat_bound");
        }
    }
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#if CONFIG_APP_CLAW_CAP_IM_WECHAT

#define APP_WECHAT_SETUP_POLL_INTERVAL_MS 2000
/* Safety net only: the cap-internal QR session already expires/refreshes a
 * bounded number of times. ~20 min upper bound on the whole binding. */
#define APP_WECHAT_SETUP_MAX_POLLS 600

/* Persist the credentials obtained after a successful scan, mirroring the web
 * "Save" path (/api/config -> save_config -> live apply + mark_persisted). */
static esp_err_t app_wechat_setup_persist_credentials(const http_server_wechat_login_status_t *status)
{
    app_config_t *config = NULL;
    esp_err_t err;

    if (s_config.load_config == NULL || s_config.save_config == NULL) {
        ESP_LOGW(TAG, "no load/save config callback; cannot persist WeChat credentials");
        return ESP_ERR_INVALID_STATE;
    }
    if (status->token[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    /* app_config_t is ~5 KB; keep it off the task stack. */
    config = calloc(1, sizeof(*config));
    if (config == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = s_config.load_config(config);
    if (err != ESP_OK) {
        free(config);
        return err;
    }

    strlcpy(config->wechat_token, status->token, sizeof(config->wechat_token));
    if (status->base_url[0]) {
        strlcpy(config->wechat_base_url, status->base_url, sizeof(config->wechat_base_url));
    }
    if (status->account_id[0]) {
        strlcpy(config->wechat_account_id, status->account_id, sizeof(config->wechat_account_id));
    }

    err = s_config.save_config(config);
    free(config);
    return err;
}

static esp_err_t app_wechat_setup_activate_credentials(const http_server_wechat_login_status_t *status)
{
    esp_err_t err;

    if (status == NULL || status->token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_im_wechat_set_client_config(&(cap_im_wechat_client_config_t) {
        .token = status->token,
        .base_url = status->base_url,
        .account_id = status->account_id[0] ? status->account_id : NULL,
    });
    if (err != ESP_OK) {
        return err;
    }

    /* The gateway is created during normal capability startup, but on first-run
     * binding it may still be waiting for credentials. Starting is idempotent and
     * makes the newly bound token usable without requiring a reboot. */
    return cap_im_wechat_start();
}

esp_err_t app_wechat_setup_run_screen_binding(void)
{
    http_server_wechat_login_status_t *status = NULL;
    esp_err_t err;

    err = app_wechat_setup_login_start(NULL, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WeChat setup login start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* http_server_wechat_login_status_t is large; keep it off the task stack. */
    status = calloc(1, sizeof(*status));
    if (status == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = ESP_ERR_TIMEOUT;
    for (int i = 0; i < APP_WECHAT_SETUP_MAX_POLLS; i++) {
        vTaskDelay(pdMS_TO_TICKS(APP_WECHAT_SETUP_POLL_INTERVAL_MS));

        /* Also refreshes the on-screen QR/status text as a side effect. */
        if (app_wechat_setup_login_get_status(status) != ESP_OK) {
            continue;
        }

        if (status->completed) {
            esp_err_t persist_err = app_wechat_setup_persist_credentials(status);
            if (persist_err != ESP_OK) {
                ESP_LOGW(TAG, "persist WeChat credentials failed: %s",
                         esp_err_to_name(persist_err));
            }
            esp_err_t activate_err = app_wechat_setup_activate_credentials(status);
            if (activate_err != ESP_OK) {
                ESP_LOGW(TAG, "activate WeChat credentials failed: %s",
                         esp_err_to_name(activate_err));
            }
            /* Marks persisted and switches to the default product mode. */
            err = app_wechat_setup_login_mark_persisted();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "mark WeChat login persisted failed: %s", esp_err_to_name(err));
            }
            if (err == ESP_OK && persist_err != ESP_OK) {
                err = persist_err;
            }
            if (err == ESP_OK && activate_err != ESP_OK) {
                err = activate_err;
            }
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "WeChat binding complete; switched to default mode");
            }
            break;
        }

        if (!status->active) {
            ESP_LOGW(TAG, "WeChat login ended without completion (status=%s)", status->status);
            err = ESP_FAIL;
            break;
        }
    }

    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "WeChat binding timed out; cancelling login");
        (void)app_wechat_setup_login_cancel();
    }

    free(status);
    return err;
}

#else /* !CONFIG_APP_CLAW_CAP_IM_WECHAT */

esp_err_t app_wechat_setup_run_screen_binding(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
