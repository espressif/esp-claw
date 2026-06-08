/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wr_wifi.h"
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "wr_wifi";
static EventGroupHandle_t s_wifi_eg;
static char s_ip[20] = {0};
static bool s_connected = false;

#define WIFI_CONNECTED_BIT BIT0

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_ip[0] = '\0';
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "STA connected, IP=%s", s_ip);
    }
}

esp_err_t wr_wifi_init(const wave_rover_config_t *cfg)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    if (cfg->wifi_mode == 0 || cfg->wifi_mode == 2) {
        esp_netif_create_default_wifi_ap();
    }
    if (cfg->wifi_mode == 1 || cfg->wifi_mode == 2) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                        on_event, NULL), TAG, "ev reg");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                        on_event, NULL), TAG, "ip ev");

    if (cfg->wifi_mode == 0) {
        wifi_config_t ap = {0};
        strlcpy((char *)ap.ap.ssid,     cfg->wifi_ap_ssid,     sizeof(ap.ap.ssid));
        strlcpy((char *)ap.ap.password, cfg->wifi_ap_password, sizeof(ap.ap.password));
        ap.ap.ssid_len       = (uint8_t)strlen((char *)ap.ap.ssid);
        ap.ap.max_connection = 4;
        ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    } else if (cfg->wifi_mode == 1) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid,     cfg->wifi_ssid,     sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, cfg->wifi_password, sizeof(sta.sta.password));
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "sta config");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    if (cfg->wifi_mode == 1) {
        /* A connect failure (e.g. bad/empty SSID, AP out of range) must never
         * be fatal here — ESP_ERROR_CHECK on this would abort() and reboot,
         * and since the bad config persists in NVS the device would crash-loop
         * forever with no way to reach it over the network. Degrade to
         * offline operation instead so the device stays reachable for repair
         * (USB serial, or AP fallback once wave_rover_config_load sanity-checks
         * the stored mode/SSID combination). */
        esp_err_t connect_err = esp_wifi_connect();
        if (connect_err != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect failed: %s — continuing offline",
                     esp_err_to_name(connect_err));
        } else {
            ESP_LOGI(TAG, "connecting to STA SSID '%s'...", cfg->wifi_ssid);
            xEventGroupWaitBits(s_wifi_eg, WIFI_CONNECTED_BIT,
                                pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
            if (!s_connected) {
                ESP_LOGW(TAG, "STA connect timeout, continuing offline");
            }
        }
    } else {
        ESP_LOGI(TAG, "AP started: SSID=%s", cfg->wifi_ap_ssid);
    }
    return ESP_OK;
}

bool wr_wifi_is_connected(void) { return s_connected; }
const char *wr_wifi_get_ip(void) { return s_ip; }
