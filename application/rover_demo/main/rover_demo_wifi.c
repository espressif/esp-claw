/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_demo_wifi.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "rover_demo_wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRY     8

static EventGroupHandle_t s_event_group;
static int s_retry_count;
static bool s_connected;
static char s_ip_str[16] = "0.0.0.0";
static esp_netif_t *s_sta_netif;
static rover_demo_wifi_state_cb_t s_state_cb;
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
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        strlcpy(s_ip_str, "0.0.0.0", sizeof(s_ip_str));
        if (s_connected) {
            s_connected = false;
            notify_state();
        }
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "retry Wi-Fi connection %d/%d", s_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
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

esp_err_t rover_demo_wifi_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    s_event_group = xEventGroupCreate();
    if (!s_event_group) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    return ESP_OK;
}

esp_err_t rover_demo_wifi_register_state_cb(rover_demo_wifi_state_cb_t cb, void *user_ctx)
{
    s_state_cb = cb;
    s_state_cb_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t rover_demo_wifi_start(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password ? password : "", sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = password && password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "set STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi");
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return ESP_OK;
}

esp_err_t rover_demo_wifi_wait_connected(uint32_t timeout_ms)
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

bool rover_demo_wifi_is_connected(void)
{
    return s_connected;
}

const char *rover_demo_wifi_get_ip(void)
{
    return s_ip_str;
}
