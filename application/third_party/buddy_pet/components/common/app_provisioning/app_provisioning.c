/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_provisioning.h"

#include <stdio.h>
#include <string.h>

#include "app_runtime_state.h"
#include "captive_dns.h"
#include "claw_event_publisher.h"
#include "config_runtime.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"

static const char *TAG = "app_provisioning";

static app_provisioning_config_t s_config;
static bool s_match_watch_network_ready_sent;

static esp_err_t app_provisioning_publish_match_watch_network_ready_reason(const char *reason)
{
    char payload[64];
    int written;

    written = snprintf(payload, sizeof(payload), "{\"reason\":\"%s\"}",
                       reason != NULL && reason[0] ? reason : "wifi_connected");
    if (written <= 0 || (size_t)written >= sizeof(payload)) {
        strlcpy(payload, "{\"reason\":\"wifi_connected\"}", sizeof(payload));
    }

    return claw_event_router_publish_trigger("match_watch", "network", "ready", payload);
}

void app_provisioning_publish_match_watch_network_ready_force(const char *reason)
{
    esp_err_t err = app_provisioning_publish_match_watch_network_ready_reason(reason);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "publish Match Watch network ready failed: %s", esp_err_to_name(err));
    }
}

void app_provisioning_publish_match_watch_network_ready(void)
{
    esp_err_t err;

    if (s_match_watch_network_ready_sent) {
        return;
    }
    s_match_watch_network_ready_sent = true;
    err = app_provisioning_publish_match_watch_network_ready_reason("wifi_connected");
    if (err != ESP_OK) {
        s_match_watch_network_ready_sent = false;
        ESP_LOGW(TAG, "publish Match Watch network ready failed: %s", esp_err_to_name(err));
    }
}

static bool app_provisioning_escape_wifi_qr_field(const char *input, char *output, size_t output_size)
{
    size_t pos = 0;

    if (output == NULL || output_size == 0) {
        return false;
    }
    output[0] = '\0';
    if (input == NULL) {
        return true;
    }

    for (const char *p = input; *p != '\0'; ++p) {
        const bool escape = *p == '\\' || *p == ';' || *p == ',' || *p == ':' || *p == '"';
        size_t need = escape ? 2 : 1;

        if (pos + need >= output_size) {
            output[0] = '\0';
            return false;
        }
        if (escape) {
            output[pos++] = '\\';
        }
        output[pos++] = *p;
    }
    output[pos] = '\0';
    return true;
}

static void app_provisioning_build_setup_url(const wifi_manager_status_t *status,
                                             char *out_url,
                                             size_t out_url_size)
{
    const char *ap_ip = (status && status->ap_ip && status->ap_ip[0]) ? status->ap_ip : "192.168.4.1";

    if (out_url == NULL || out_url_size == 0) {
        return;
    }
    snprintf(out_url, out_url_size, "http://%s/#start", ap_ip);
}

static bool app_provisioning_build_wifi_join_qr(const char *ssid,
                                                const char *password,
                                                char *out_qr,
                                                size_t out_qr_size)
{
    char escaped_ssid[96] = {0};
    char escaped_password[160] = {0};
    int written;

    if (ssid == NULL || ssid[0] == '\0' || out_qr == NULL || out_qr_size == 0) {
        return false;
    }
    if (!app_provisioning_escape_wifi_qr_field(ssid, escaped_ssid, sizeof(escaped_ssid)) ||
            !app_provisioning_escape_wifi_qr_field(password, escaped_password, sizeof(escaped_password))) {
        return false;
    }

    if (password != NULL && password[0] != '\0') {
        written = snprintf(out_qr, out_qr_size, "WIFI:T:WPA;S:%s;P:%s;;",
                           escaped_ssid, escaped_password);
    } else {
        written = snprintf(out_qr, out_qr_size, "WIFI:T:nopass;S:%s;;", escaped_ssid);
    }
    return written > 0 && (size_t)written < out_qr_size;
}

static void app_provisioning_show_wifi_setup_qr(const wifi_manager_status_t *status, const char *status_text)
{
    /* static: these buffers are too large for the sys_evt task stack (called from the
     * Wi-Fi/IP event handler context); calls are serialized by the event loop. */
    static app_runtime_wifi_fields_t wifi_fields;
    static char setup_url[96];
    static char wifi_qr[256];
    static char detail[128];
    const char *ap_ssid = (status && status->ap_ssid && status->ap_ssid[0]) ? status->ap_ssid : NULL;
    const char *ap_ip = (status && status->ap_ip && status->ap_ip[0]) ? status->ap_ip : "192.168.4.1";

    (void)app_runtime_state_wifi_fields_snapshot(&wifi_fields);
    app_provisioning_build_setup_url(status, setup_url, sizeof(setup_url));
    if (ap_ssid != NULL) {
        snprintf(detail, sizeof(detail), "AP: %s  %s", ap_ssid, setup_url);
    } else {
        snprintf(detail, sizeof(detail), "%s", ap_ip);
    }
    if (!app_provisioning_build_wifi_join_qr(ap_ssid,
                                             wifi_fields.ap_password[0] ? wifi_fields.ap_password : NULL,
                                             wifi_qr,
                                             sizeof(wifi_qr))) {
        ESP_LOGW(TAG, "Failed to build Wi-Fi join QR, fallback to portal URL");
        strlcpy(wifi_qr, setup_url, sizeof(wifi_qr));
    }

    esp_err_t err = config_runtime_show_qr("",
                                           wifi_qr,
                                           "Scan with Camera to Join AP",
                                           status_text ? status_text : detail);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "show setup QR failed: %s", esp_err_to_name(err));
    }
}

static void app_provisioning_get_wifi_status_after_start(wifi_manager_status_t *status)
{
    if (status == NULL) {
        return;
    }

    for (int i = 0; i < 20; ++i) {
        wifi_manager_get_status(status);
        if (status->ap_active || status->sta_connected) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    wifi_manager_get_status(status);
}

static void app_provisioning_log_portal(const wifi_manager_status_t *status)
{
    static app_runtime_wifi_fields_t wifi_fields;

    if (status == NULL || !status->ap_active) {
        return;
    }
    (void)app_runtime_state_wifi_fields_snapshot(&wifi_fields);
    ESP_LOGW(TAG,
             "*** Provisioning portal: SSID=\"%s\" (auth=%s) IP=%s URL=http://%s/ ***",
             status->ap_ssid,
             wifi_fields.ap_password[0] ? "wpa2" : "open",
             status->ap_ip,
             status->ap_ip);
}

static void app_provisioning_log_current_wifi_credentials(const char *reason)
{
    static app_runtime_wifi_fields_t wifi_fields;

    if (app_runtime_state_wifi_fields_snapshot(&wifi_fields) != ESP_OK) {
        ESP_LOGW(TAG, "Current STA config%s: unavailable", reason != NULL ? reason : "");
        return;
    }
    ESP_LOGW(TAG,
             "Current STA config%s: ssid=\"%s\" passwd=\"%s\"",
             reason != NULL ? reason : "",
             wifi_fields.sta_ssid[0] ? wifi_fields.sta_ssid : "(empty)",
             wifi_fields.sta_password[0] ? wifi_fields.sta_password : "(empty)");
}

static void app_provisioning_on_wifi_state_changed(bool connected, void *user_ctx)
{
    (void)user_ctx;

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    const char *ap_ssid = status.ap_active ? status.ap_ssid : NULL;
    bool business_ready = s_config.is_business_ready != NULL && s_config.is_business_ready();

    ESP_LOGI(TAG, "Wi-Fi state: sta_connected=%d ap_active=%d mode=%s ap_ssid=%s",
             connected,
             status.ap_active,
             status.mode ? status.mode : "off",
             ap_ssid ? ap_ssid : "(none)");

    if (business_ready && !connected) {
        ESP_LOGI(TAG, "Business setup already complete; suppress setup QR while Wi-Fi reconnects");
        if (s_config.open_default_mode != NULL) {
            (void)s_config.open_default_mode();
        }
    } else if (status.ap_active && !connected) {
        app_provisioning_show_wifi_setup_qr(&status, "Waiting for Wi-Fi setup");
    } else if (connected) {
        if (business_ready) {
            if (s_config.open_default_mode != NULL) {
                (void)s_config.open_default_mode();
            }
            app_provisioning_publish_match_watch_network_ready();
        } else if (s_config.start_business_setup != NULL) {
            ESP_LOGI(TAG, "Wi-Fi connected; starting business setup (e.g. WeChat binding)");
            esp_err_t err = s_config.start_business_setup();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "start business setup failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGI(TAG, "Wi-Fi connected; staying in config runtime until setup is complete");
        }
    }
}

esp_err_t app_provisioning_init(const app_provisioning_config_t *config)
{
    if (config != NULL) {
        s_config = *config;
    } else {
        memset(&s_config, 0, sizeof(s_config));
    }
    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "init Wi-Fi manager failed");
    return wifi_manager_register_state_callback(app_provisioning_on_wifi_state_changed, NULL);
}

esp_err_t app_provisioning_start(const app_config_t *config)
{
    esp_err_t err;
    wifi_manager_status_t status = {0};

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "missing config");
    app_provisioning_log_current_wifi_credentials(" before start");
    err = wifi_manager_start(&(wifi_manager_config_t) {
        .sta_ssid = config->wifi_ssid,
        .sta_password = config->wifi_password,
        .ap_ssid = config->ap_ssid[0] ? config->ap_ssid : NULL,
        .ap_password = config->ap_password[0] ? config->ap_password : NULL,
        .ap_behavior = config->ap_behavior,
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(http_server_start(), TAG, "start HTTP server failed");
    if (captive_dns_start(&(captive_dns_config_t) {
            .ap_netif = wifi_manager_get_ap_netif(),
            .configure_dhcp_dns = true,
        }) != ESP_OK) {
        ESP_LOGW(TAG, "Captive DNS could not start, portal pop-up disabled");
    }

    app_provisioning_get_wifi_status_after_start(&status);
    app_provisioning_log_portal(&status);
    bool business_ready = s_config.is_business_ready != NULL && s_config.is_business_ready();
    if (business_ready) {
        if (s_config.open_default_mode != NULL) {
            (void)s_config.open_default_mode();
        }
        if (status.sta_connected) {
            app_provisioning_publish_match_watch_network_ready();
        } else {
            ESP_LOGI(TAG, "Business setup already complete; suppress setup QR while Wi-Fi connects");
        }
    } else if (status.ap_active && !status.sta_connected) {
        app_provisioning_show_wifi_setup_qr(&status, "Waiting for Wi-Fi setup");
        if (config->wifi_ssid[0] != '\0') {
            ESP_LOGI(TAG, "STA connection is continuing in background");
        }
    } else {
        ESP_LOGI(TAG, "Staying in config runtime until setup is complete");
    }
    return ESP_OK;
}

esp_err_t app_provisioning_apply_wifi_config(const app_config_t *config)
{
    app_runtime_wifi_fields_t active_config = {0};
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = app_config_validate_wifi(config, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = app_config_save(config);
    if (err != ESP_OK) {
        return err;
    }
    ESP_RETURN_ON_ERROR(app_runtime_state_update_config(config), TAG, "update runtime config failed");
    ESP_RETURN_ON_ERROR(app_runtime_state_wifi_fields_snapshot(&active_config),
                        TAG, "snapshot runtime Wi-Fi fields failed");

    wifi_manager_status_t status = {0};
    wifi_manager_get_status(&status);
    app_provisioning_show_wifi_setup_qr(&status, "Connecting Wi-Fi");

    return wifi_manager_apply_sta_config(&(wifi_manager_config_t) {
        .sta_ssid = active_config.sta_ssid,
        .sta_password = active_config.sta_password,
        .ap_ssid = active_config.ap_ssid[0] ? active_config.ap_ssid : NULL,
        .ap_password = active_config.ap_password[0] ? active_config.ap_password : NULL,
        .ap_behavior = "keep",
    });
}

esp_err_t app_provisioning_get_wifi_status(http_server_wifi_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_manager_status_t wifi_status = {0};
    wifi_manager_get_status(&wifi_status);
    status->wifi_connected = wifi_status.sta_connected;
    status->ip = wifi_status.sta_ip;
    status->ap_active = wifi_status.ap_active;
    status->ap_ssid = wifi_status.ap_ssid;
    status->ap_ip = wifi_status.ap_ip;
    status->wifi_mode = wifi_status.mode;
    return ESP_OK;
}
