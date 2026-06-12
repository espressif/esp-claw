/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wifi_manager.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_RETRY_BASE_MS  1000
#define WIFI_RETRY_MAX_MS   30000

#ifndef CONFIG_APP_WIFI_AP_SSID_PREFIX
#define CONFIG_APP_WIFI_AP_SSID_PREFIX "esp-claw"
#endif
#ifndef CONFIG_APP_WIFI_AP_CHANNEL
#define CONFIG_APP_WIFI_AP_CHANNEL 1
#endif
#ifndef CONFIG_APP_WIFI_AP_MAX_CONN
#define CONFIG_APP_WIFI_AP_MAX_CONN 4
#endif
#ifndef CONFIG_APP_WIFI_MAX_RETRY
#define CONFIG_APP_WIFI_MAX_RETRY 5
#endif

typedef enum {
    WIFI_MODE_OFF = 0,
    WIFI_MODE_PROVISION_AP,
    WIFI_MODE_APSTA_TRYING,
    WIFI_MODE_APSTA_OK,
    WIFI_MODE_STA_ONLY,
    WIFI_MODE_AP_FALLBACK,
} wifi_mode_state_t;

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count;
static bool s_connected;
static bool s_ap_active;
static bool s_sta_configured;
static char s_ip_addr[16] = "0.0.0.0";
static char s_ap_ip[16] = "192.168.4.1";
static EXT_RAM_BSS_ATTR char s_ap_ssid[33];
static EXT_RAM_BSS_ATTR char s_sta_ssid[33];
static EXT_RAM_BSS_ATTR char s_sta_password[65];
static EXT_RAM_BSS_ATTR char s_ap_ssid_override[33];
static EXT_RAM_BSS_ATTR char s_ap_password[65];
static EXT_RAM_BSS_ATTR char s_ap_behavior[16];
static EXT_RAM_BSS_ATTR char s_ap_ssid_prefix[33];
static wifi_mode_state_t s_mode = WIFI_MODE_OFF;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static wifi_manager_state_cb_t s_state_cb;
static void *s_state_cb_user_ctx;
static esp_timer_handle_t s_reconnect_timer;
static wifi_manager_config_t s_config;
static bool s_wifi_started;

static const char *wifi_manager_mode_string(wifi_mode_state_t mode)
{
    switch (mode) {
    case WIFI_MODE_PROVISION_AP: return "provision";
    case WIFI_MODE_APSTA_TRYING: return "apsta";
    case WIFI_MODE_APSTA_OK:     return "sta_ok";
    case WIFI_MODE_STA_ONLY:     return "sta_only";
    case WIFI_MODE_AP_FALLBACK:  return "fallback";
    default:                     return "off";
    }
}

static void notify_state_changed(bool force);
static esp_err_t fallback_to_ap(void);
static void reconnect_timer_cb(void *arg);
static esp_err_t configure_sta_mode(const wifi_manager_config_t *config);
static void reset_sta_runtime_state(void);

static bool wifi_manager_ap_behavior_is_valid(const char *ap_behavior)
{
    return !ap_behavior || ap_behavior[0] == '\0' ||
           strcmp(ap_behavior, "keep") == 0 ||
           strcmp(ap_behavior, "close_on_sta") == 0;
}

static void copy_owned_string(char *dst, size_t dst_size, const char *src)
{
    strlcpy(dst, src ? src : "", dst_size);
}

static void sync_owned_config(const wifi_manager_config_t *config)
{
    copy_owned_string(s_sta_ssid, sizeof(s_sta_ssid), config->sta_ssid);
    copy_owned_string(s_sta_password, sizeof(s_sta_password), config->sta_password);
    copy_owned_string(s_ap_ssid_prefix, sizeof(s_ap_ssid_prefix), config->ap_ssid_prefix);
    copy_owned_string(s_ap_ssid_override, sizeof(s_ap_ssid_override), config->ap_ssid);
    copy_owned_string(s_ap_password, sizeof(s_ap_password), config->ap_password);
    copy_owned_string(s_ap_behavior, sizeof(s_ap_behavior), config->ap_behavior);

    s_config = *config;
    s_config.sta_ssid = s_sta_ssid[0] ? s_sta_ssid : NULL;
    s_config.sta_password = s_sta_password[0] ? s_sta_password : NULL;
    s_config.ap_ssid_prefix = s_ap_ssid_prefix[0] ? s_ap_ssid_prefix : NULL;
    s_config.ap_ssid = s_ap_ssid_override[0] ? s_ap_ssid_override : NULL;
    s_config.ap_password = s_ap_password[0] ? s_ap_password : NULL;
    s_config.ap_behavior = s_ap_behavior[0] ? s_ap_behavior : NULL;
}

static const char *wifi_manager_ap_ssid_prefix(void)
{
    return (s_config.ap_ssid_prefix && s_config.ap_ssid_prefix[0] != '\0')
           ? s_config.ap_ssid_prefix
           : CONFIG_APP_WIFI_AP_SSID_PREFIX;
}

static uint8_t wifi_manager_ap_channel(void)
{
    return s_config.ap_channel ? s_config.ap_channel : CONFIG_APP_WIFI_AP_CHANNEL;
}

static uint8_t wifi_manager_ap_max_conn(void)
{
    return s_config.ap_max_conn ? s_config.ap_max_conn : CONFIG_APP_WIFI_AP_MAX_CONN;
}

static uint32_t wifi_manager_max_retry(void)
{
    return s_config.max_retry ? s_config.max_retry : CONFIG_APP_WIFI_MAX_RETRY;
}

static bool wifi_manager_sta_password_is_set(void)
{
    return s_config.sta_password && s_config.sta_password[0] != '\0';
}

static void compose_ap_ssid(void)
{
    if (s_config.ap_ssid && s_config.ap_ssid[0] != '\0') {
        strlcpy(s_ap_ssid, s_config.ap_ssid, sizeof(s_ap_ssid));
        ESP_LOGI(TAG, "Custom AP SSID: %s", s_ap_ssid);
        return;
    }
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X%02X",
             wifi_manager_ap_ssid_prefix(), mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Provisioning AP SSID: %s", s_ap_ssid);
}

static void apply_ap_config(void)
{
    wifi_config_t ap_cfg = {0};
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(ap_cfg.ap.ssid, s_ap_ssid, ap_cfg.ap.ssid_len);
    ap_cfg.ap.channel = wifi_manager_ap_channel();
    ap_cfg.ap.max_connection = wifi_manager_ap_max_conn();
    if (s_config.ap_password && s_config.ap_password[0] != '\0') {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)ap_cfg.ap.password, s_config.ap_password, sizeof(ap_cfg.ap.password));
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
}

static void refresh_ap_ip_str(void)
{
    if (!s_ap_netif) {
        return;
    }
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip_info.ip));
    }
}

static void reset_sta_runtime_state(void)
{
    strlcpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
    s_connected = false;
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
}

esp_err_t wifi_manager_validate_config(const wifi_manager_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->sta_ssid && config->sta_ssid[0] != '\0') {
        size_t sta_ssid_len = strlen(config->sta_ssid);
        if (sta_ssid_len >= sizeof(((wifi_config_t *)0)->sta.ssid)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->sta_password && config->sta_password[0] != '\0') {
        size_t sta_password_len = strlen(config->sta_password);
        if (sta_password_len < 8 || sta_password_len >= sizeof(((wifi_config_t *)0)->sta.password)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->ap_password && config->ap_password[0] != '\0') {
        size_t ap_password_len = strlen(config->ap_password);
        if (ap_password_len < 8 || ap_password_len >= sizeof(((wifi_config_t *)0)->ap.password)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (config->ap_ssid && strlen(config->ap_ssid) > sizeof(((wifi_config_t *)0)->ap.ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_ssid_prefix &&
            strlen(config->ap_ssid_prefix) >= sizeof(s_ap_ssid) - 7) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!wifi_manager_ap_behavior_is_valid(config->ap_behavior)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t configure_sta_mode(const wifi_manager_config_t *config)
{
    wifi_mode_t target_mode = WIFI_MODE_AP;
    esp_err_t err;

    err = wifi_manager_validate_config(config);
    if (err != ESP_OK) {
        return err;
    }

    sync_owned_config(config);
    compose_ap_ssid();
    s_sta_configured = (s_config.sta_ssid && s_config.sta_ssid[0] != '\0');
    ESP_LOGI(TAG,
             "Applying Wi-Fi config: sta_configured=%d sta_ssid_len=%u sta_password_empty=%d ap_password_empty=%d ap_behavior=%s",
             s_sta_configured,
             (unsigned)(s_config.sta_ssid ? strlen(s_config.sta_ssid) : 0U),
             wifi_manager_sta_password_is_set() ? 0 : 1,
             (s_config.ap_password && s_config.ap_password[0] != '\0') ? 0 : 1,
             s_config.ap_behavior ? s_config.ap_behavior : "keep");
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    reset_sta_runtime_state();

    if (s_sta_configured) {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid, s_config.sta_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password,
                s_config.sta_password ? s_config.sta_password : "",
                sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode =
            wifi_manager_sta_password_is_set()
            ? WIFI_AUTH_WPA2_PSK
            : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;

        s_mode = WIFI_MODE_APSTA_TRYING;
        target_mode = WIFI_MODE_APSTA;
        err = esp_wifi_set_mode(target_mode);
        if (err != ESP_OK) {
            return err;
        }
        apply_ap_config();
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            return err;
        }
        return ESP_OK;
    }

    s_mode = WIFI_MODE_PROVISION_AP;
    err = esp_wifi_set_mode(target_mode);
    if (err != ESP_OK) {
        return err;
    }
    apply_ap_config();
    return ESP_OK;
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (s_sta_configured) {
                ESP_LOGI(TAG,
                         "STA start: ssid_len=%u auth_threshold=%s mode=%s",
                         (unsigned)strlen(s_config.sta_ssid),
                         wifi_manager_sta_password_is_set() ? "wpa2_psk" : "open",
                         wifi_manager_mode_string(s_mode));
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Wi-Fi STA not configured, skipping connection");
            }
            return;

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            const wifi_event_sta_disconnected_t *disc =
                event_data ? (const wifi_event_sta_disconnected_t *)event_data : NULL;
            uint16_t reason = disc ? disc->reason : 0;
            strlcpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
            if (s_connected) {
                s_connected = false;
                notify_state_changed(false);
            }
            if (!s_sta_configured) {
                ESP_LOGW(TAG, "STA disconnected but not configured, reason=%u", (unsigned)reason);
                return;
            }
            if (s_retry_count < (int)wifi_manager_max_retry()) {
                uint32_t delay_ms = WIFI_RETRY_BASE_MS << s_retry_count;
                if (delay_ms > WIFI_RETRY_MAX_MS) {
                    delay_ms = WIFI_RETRY_MAX_MS;
                }
                s_retry_count++;
                s_mode = s_ap_active ? WIFI_MODE_APSTA_TRYING : s_mode;
                ESP_LOGI(TAG,
                         "STA disconnected: reason=%u retry=%d/%" PRIu32 " delay_ms=%" PRIu32 " mode=%s ap_active=%d",
                         (unsigned)reason,
                         s_retry_count,
                         wifi_manager_max_retry(),
                         delay_ms,
                         wifi_manager_mode_string(s_mode),
                         s_ap_active);
                if (s_reconnect_timer) {
                    esp_timer_stop(s_reconnect_timer);
                    esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
                } else {
                    esp_wifi_connect();
                }
            } else {
                ESP_LOGE(TAG,
                         "STA failed after %" PRIu32 " retries, reason=%u, falling back to AP",
                         wifi_manager_max_retry(),
                         (unsigned)reason);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                fallback_to_ap();
            }
            return;
        }

        case WIFI_EVENT_AP_START:
            s_ap_active = true;
            refresh_ap_ip_str();
            ESP_LOGW(TAG, "*** Provisioning AP active: %s @ %s ***", s_ap_ssid, s_ap_ip);
            notify_state_changed(true);
            return;

        case WIFI_EVENT_AP_STOP:
            s_ap_active = false;
            notify_state_changed(true);
            return;

        default:
            return;
        }
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_connected = true;
        s_mode = s_ap_active ? WIFI_MODE_APSTA_OK : s_mode;
        if (s_config.ap_behavior && strcmp(s_config.ap_behavior, "close_on_sta") == 0 && s_ap_active) {
            ESP_LOGI(TAG, "STA connected, closing AP per ap_behavior setting");
            esp_err_t _ap_err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (_ap_err == ESP_OK) {
                s_ap_active = false;
                s_mode = WIFI_MODE_STA_ONLY;
            } else {
                ESP_LOGW(TAG, "Failed to switch to STA-only mode: %s", esp_err_to_name(_ap_err));
            }
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        notify_state_changed(true);
    }
}

static void notify_state_changed(bool force)
{
    static bool s_last_connected;
    static bool s_last_ap_active;
    static bool s_initialized;

    if (!force && s_initialized && s_last_connected == s_connected &&
            s_last_ap_active == s_ap_active) {
        return;
    }

    s_last_connected = s_connected;
    s_last_ap_active = s_ap_active;
    s_initialized = true;

    if (s_state_cb) {
        s_state_cb(s_connected, s_state_cb_user_ctx);
    }
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_sta_configured) {
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

static esp_err_t fallback_to_ap(void)
{
    s_mode = WIFI_MODE_AP_FALLBACK;
    s_sta_configured = false;
    s_retry_count = 0;

    ESP_LOGW(TAG,
             "Switching to AP fallback: ap_ssid=%s ap_auth=%s",
             s_ap_ssid[0] ? s_ap_ssid : "-",
             (s_config.ap_password && s_config.ap_password[0] != '\0') ? "wpa2_psk" : "open");

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }
    apply_ap_config();
    refresh_ap_ip_str();
    ESP_LOGW(TAG, "AP fallback active: ap_ssid=%s ap_ip=%s", s_ap_ssid, s_ap_ip);
    notify_state_changed(true);
    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi_event_group) {
        return ESP_OK;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    memset(&s_config, 0, sizeof(s_config));
    s_wifi_started = false;
    compose_ap_ssid();
    return ESP_OK;
}

esp_err_t wifi_manager_start(const wifi_manager_config_t *config)
{
    esp_err_t err;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    err = configure_sta_mode(config);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            return err;
        }
        s_wifi_started = true;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_apply_sta_config(const wifi_manager_config_t *config)
{
    esp_err_t err;
    bool was_connected;

    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    was_connected = s_connected;
    err = configure_sta_mode(config);
    if (err != ESP_OK) {
        return err;
    }

    if (was_connected) {
        notify_state_changed(false);
    }

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) {
            return err;
        }
        s_wifi_started = true;
    }

    if (!s_sta_configured) {
        return ESP_OK;
    }

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return err;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (!s_sta_configured) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           ticks);
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_register_state_callback(wifi_manager_state_cb_t cb, void *user_ctx)
{
    s_state_cb = cb;
    s_state_cb_user_ctx = user_ctx;

    if (s_state_cb) {
        s_state_cb(s_connected, s_state_cb_user_ctx);
    }

    return ESP_OK;
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (!status) {
        return;
    }

    status->sta_connected = s_connected;
    status->ap_active = s_ap_active;
    status->sta_configured = s_sta_configured;
    status->sta_ip = s_ip_addr;
    status->ap_ip = s_ap_ip;
    status->ap_ssid = s_ap_ssid;
    status->mode = wifi_manager_mode_string(s_mode);
}

esp_netif_t *wifi_manager_get_ap_netif(void)
{
    return s_ap_netif;
}

esp_err_t wifi_manager_scan_aps(wifi_manager_scan_record_t *records,
                                uint16_t max_records,
                                uint16_t *out_count)
{
    wifi_mode_t original_mode = WIFI_MODE_NULL;
    wifi_mode_t scan_mode = WIFI_MODE_NULL;
    wifi_scan_config_t scan_cfg = {
        .show_hidden = true,
    };
    wifi_ap_record_t *ap_records = NULL;
    uint16_t ap_count = 0;
    esp_err_t err;

    if (!records || max_records == 0 || !out_count) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_count = 0;

    err = esp_wifi_get_mode(&original_mode);
    if (err != ESP_OK) {
        return err;
    }

    scan_mode = original_mode;
    if (original_mode == WIFI_MODE_AP) {
        scan_mode = WIFI_MODE_APSTA;
        err = esp_wifi_set_mode(scan_mode);
        if (err != ESP_OK) {
            return err;
        }
    } else if (original_mode != WIFI_MODE_STA && original_mode != WIFI_MODE_APSTA) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        goto cleanup;
    }

    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (ap_count == 0) {
        err = ESP_OK;
        goto cleanup;
    }

    if (ap_count > max_records) {
        ap_count = max_records;
    }

    ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        goto cleanup;
    }

    for (uint16_t i = 0; i < ap_count; ++i) {
        strlcpy(records[i].ssid, (const char *)ap_records[i].ssid, sizeof(records[i].ssid));
        records[i].rssi = ap_records[i].rssi;
        records[i].primary = ap_records[i].primary;
        records[i].authmode = ap_records[i].authmode;
    }
    *out_count = ap_count;
    err = ESP_OK;

cleanup:
    free(ap_records);
    if (scan_mode != original_mode) {
        esp_err_t restore_err = esp_wifi_set_mode(original_mode);
        if (err == ESP_OK && restore_err != ESP_OK) {
            err = restore_err;
        }
    }
    return err;
}
