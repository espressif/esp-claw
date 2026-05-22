/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_s3_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "rover_s3_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     8

static EventGroupHandle_t s_event_group;
static int s_retry_count;
static bool s_connected;
static bool s_initialized;
static bool s_created_event_loop;
static char s_ip_str[16] = "0.0.0.0";
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_event_instance;
static esp_event_handler_instance_t s_ip_event_instance;
static rover_s3_wifi_state_cb_t s_state_cb;
static void *s_state_cb_ctx;

static void notify_state(void)
{
    if (s_state_cb) {
        s_state_cb(s_connected, s_state_cb_ctx);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(err));
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_event_group) {
            xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT);
        }
        strlcpy(s_ip_str, "0.0.0.0", sizeof(s_ip_str));
        bool was_connected = s_connected;
        s_connected = false;
        if (was_connected) {
            notify_state();
        }
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "retry Wi-Fi connection %d/%d", s_retry_count, WIFI_MAX_RETRY);
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Wi-Fi reconnect failed: %s", esp_err_to_name(err));
            }
        } else {
            if (s_event_group) {
                xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        ESP_LOGI(TAG, "connected, IP=%s", s_ip_str);
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
        notify_state();
    }
}

esp_err_t rover_s3_wifi_init(void)
{
    esp_err_t ret;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    if (s_initialized) {
        return ESP_OK;
    }

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        return ESP_ERR_NO_MEM;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = esp_event_loop_create_default();
    if (ret == ESP_OK) {
        s_created_event_loop = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    } else {
        goto fail;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ret = ESP_FAIL;
        goto fail;
    }
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        goto fail;
    }
    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &wifi_event_handler, NULL, &s_wifi_event_instance);
    if (ret != ESP_OK) {
        goto fail_deinit_wifi;
    }
    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              &wifi_event_handler, NULL, &s_ip_event_instance);
    if (ret != ESP_OK) {
        goto fail_deinit_wifi;
    }
    s_initialized = true;
    return ESP_OK;

fail_deinit_wifi:
    if (s_ip_event_instance) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_event_instance);
        s_ip_event_instance = NULL;
    }
    if (s_wifi_event_instance) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_event_instance);
        s_wifi_event_instance = NULL;
    }
    esp_wifi_deinit();
fail:
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }
    if (s_event_group) {
        vEventGroupDelete(s_event_group);
        s_event_group = NULL;
    }
    if (s_created_event_loop) {
        esp_event_loop_delete_default();
        s_created_event_loop = false;
    }
    return ret;
}

esp_err_t rover_s3_wifi_register_state_cb(rover_s3_wifi_state_cb_t cb, void *user_ctx)
{
    s_state_cb = cb;
    s_state_cb_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t rover_s3_wifi_start(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password ? password : "", sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    xEventGroupClearBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;
    s_connected = false;
    strlcpy(s_ip_str, "0.0.0.0", sizeof(s_ip_str));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    esp_err_t ret = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set Wi-Fi power save failed: %s", esp_err_to_name(ret));
        esp_err_t stop_ret = esp_wifi_stop();
        if (stop_ret != ESP_OK) {
            ESP_LOGW(TAG, "stop Wi-Fi after power save failure failed: %s", esp_err_to_name(stop_ret));
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t rover_s3_wifi_wait_connected(uint32_t timeout_ms)
{
    if (!s_event_group) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

bool rover_s3_wifi_is_connected(void)
{
    return s_connected;
}

const char *rover_s3_wifi_get_ip(void)
{
    return s_ip_str;
}

/* ── AP (captive portal) mode ──────────────────────────────────────── */

static esp_netif_t *s_ap_netif;
static bool s_ap_active;

esp_err_t rover_s3_wifi_start_ap(const char *ssid)
{
    if (s_ap_active) return ESP_OK;

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) return ESP_FAIL;
    }

    wifi_config_t ap_cfg = {
        .ap = {
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ssid ? ssid : "Rover-S3-Setup",
            sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen((char *)ap_cfg.ap.ssid);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start AP");
    s_ap_active = true;
    ESP_LOGI(TAG, "AP started SSID='%s' IP=192.168.4.1", ap_cfg.ap.ssid);
    return ESP_OK;
}

bool rover_s3_wifi_is_ap_active(void)
{
    return s_ap_active;
}

esp_netif_t *rover_s3_wifi_get_ap_netif(void)
{
    return s_ap_netif;
}
