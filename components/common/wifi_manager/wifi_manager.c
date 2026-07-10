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

#ifdef CONFIG_IDF_TARGET_ESP32P4
#include "esp_hosted_misc.h"
#endif

static const char *TAG = "wifi_manager";

#define WIFI_CONNECTED_BIT BIT0

#ifndef CONFIG_APP_WIFI_AP_SSID_PREFIX
#define CONFIG_APP_WIFI_AP_SSID_PREFIX "esp-claw"
#endif
#ifndef CONFIG_APP_WIFI_AP_CHANNEL
#define CONFIG_APP_WIFI_AP_CHANNEL 1
#endif
#ifndef CONFIG_APP_WIFI_AP_MAX_CONN
#define CONFIG_APP_WIFI_AP_MAX_CONN 4
#endif
/*
 * Fixed reconnect cadence. Whenever the STA interface disconnects (or the
 * connect attempt fails), we schedule the next attempt this many ms later
 * and just keep trying. There is no retry counter, no exponential backoff
 * and no "give up" state: the AP is always reachable while STA is not
 * connected, so users can always fix credentials via the portal.
 */
#ifndef CONFIG_APP_WIFI_RETRY_MS
#define CONFIG_APP_WIFI_RETRY_MS 10000
#endif
#define WIFI_RETRY_MS CONFIG_APP_WIFI_RETRY_MS

typedef enum {
    WM_STATE_OFF = 0,
    WM_STATE_PROVISION_AP, /* STA not configured, AP only. */
    WM_STATE_APSTA,        /* STA configured; AP + STA both up. */
    WM_STATE_STA_ONLY,     /* STA connected + close_on_sta closed the AP. */
} wifi_mode_state_t;

static EventGroupHandle_t s_wifi_event_group;
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
static wifi_mode_state_t s_mode = WM_STATE_OFF;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static wifi_manager_state_cb_t s_state_cb;
static void *s_state_cb_user_ctx;
static esp_timer_handle_t s_reconnect_timer;
static wifi_manager_config_t s_config;
static bool s_wifi_started;

static void notify_state_changed(bool force);
static esp_err_t configure_sta_mode(const wifi_manager_config_t *config);
static void reset_sta_runtime_state(void);
static void reconnect_timer_cb(void *arg);
static void arm_reconnect(void);
static void reopen_ap_if_needed(void);

static const char *wifi_manager_mode_string(wifi_mode_state_t mode)
{
    switch (mode) {
    case WM_STATE_PROVISION_AP: return "provision";
    case WM_STATE_APSTA:        return "apsta";
    case WM_STATE_STA_ONLY:     return "sta_only";
    default:                    return "off";
    }
}

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
           ? s_config.ap_ssid_prefix : CONFIG_APP_WIFI_AP_SSID_PREFIX;
}

static uint8_t wifi_manager_ap_channel(void)
{
    return s_config.ap_channel ? s_config.ap_channel : CONFIG_APP_WIFI_AP_CHANNEL;
}

static uint8_t wifi_manager_ap_max_conn(void)
{
    return s_config.ap_max_conn ? s_config.ap_max_conn : CONFIG_APP_WIFI_AP_MAX_CONN;
}

static bool wifi_manager_sta_password_is_set(void)
{
    return s_config.sta_password && s_config.sta_password[0] != '\0';
}

static bool wifi_manager_close_on_sta(void)
{
    return s_config.ap_behavior && strcmp(s_config.ap_behavior, "close_on_sta") == 0;
}

static void compose_ap_ssid(void)
{
    if (s_config.ap_ssid && s_config.ap_ssid[0] != '\0') {
        strlcpy(s_ap_ssid, s_config.ap_ssid, sizeof(s_ap_ssid));
        ESP_LOGI(TAG, "Custom AP SSID: %s", s_ap_ssid);
        return;
    }
    uint8_t mac[6] = {0};
#ifdef CONFIG_IDF_TARGET_ESP32P4
    size_t mac_len = esp_hosted_iface_mac_addr_len_get(ESP_MAC_WIFI_SOFTAP);
    esp_err_t ret = esp_hosted_iface_mac_addr_get(mac, mac_len, ESP_MAC_WIFI_SOFTAP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get AP MAC address: %s", esp_err_to_name(ret));
    }
#else
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
#endif
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
    if (!s_ap_netif) return;
    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        snprintf(s_ap_ip, sizeof(s_ap_ip), IPSTR, IP2STR(&ip_info.ip));
    }
}

static void reset_sta_runtime_state(void)
{
    strlcpy(s_ip_addr, "0.0.0.0", sizeof(s_ip_addr));
    s_connected = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
}

esp_err_t wifi_manager_validate_config(const wifi_manager_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (config->sta_ssid && config->sta_ssid[0] != '\0') {
        if (strlen(config->sta_ssid) >= sizeof(((wifi_config_t *)0)->sta.ssid)) return ESP_ERR_INVALID_ARG;
    }
    if (config->sta_password && config->sta_password[0] != '\0') {
        size_t n = strlen(config->sta_password);
        if (n < 8 || n >= sizeof(((wifi_config_t *)0)->sta.password)) return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_password && config->ap_password[0] != '\0') {
        size_t n = strlen(config->ap_password);
        if (n < 8 || n >= sizeof(((wifi_config_t *)0)->ap.password)) return ESP_ERR_INVALID_ARG;
    }
    if (config->ap_ssid && strlen(config->ap_ssid) > sizeof(((wifi_config_t *)0)->ap.ssid)) return ESP_ERR_INVALID_ARG;
    if (config->ap_ssid_prefix && strlen(config->ap_ssid_prefix) >= sizeof(s_ap_ssid) - 7) return ESP_ERR_INVALID_ARG;
    if (!wifi_manager_ap_behavior_is_valid(config->ap_behavior)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t configure_sta_mode(const wifi_manager_config_t *config)
{
    esp_err_t err = wifi_manager_validate_config(config);
    if (err != ESP_OK) return err;

    sync_owned_config(config);
    compose_ap_ssid();
    s_sta_configured = (s_config.sta_ssid && s_config.sta_ssid[0] != '\0');
    ESP_LOGI(TAG, "Applying Wi-Fi config: sta_configured=%d sta_ssid_len=%u sta_password_empty=%d ap_password_empty=%d ap_behavior=%s",
             s_sta_configured,
             (unsigned)(s_config.sta_ssid ? strlen(s_config.sta_ssid) : 0U),
             wifi_manager_sta_password_is_set() ? 0 : 1,
             (s_config.ap_password && s_config.ap_password[0] != '\0') ? 0 : 1,
             s_config.ap_behavior ? s_config.ap_behavior : "keep");

    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    reset_sta_runtime_state();

    if (s_sta_configured) {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid, s_config.sta_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password,
                s_config.sta_password ? s_config.sta_password : "",
                sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = wifi_manager_sta_password_is_set() ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;

        s_mode = WM_STATE_APSTA;
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) return err;
        apply_ap_config();
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) return err;
        return ESP_OK;
    }

    s_mode = WM_STATE_PROVISION_AP;
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    apply_ap_config();
    return ESP_OK;
}

static void arm_reconnect(void)
{
    if (!s_reconnect_timer) return;
    esp_timer_stop(s_reconnect_timer);
    esp_err_t err = esp_timer_start_once(s_reconnect_timer, (uint64_t)WIFI_RETRY_MS * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm reconnect timer: %s", esp_err_to_name(err));
    }
}

/*
 * When close_on_sta closed the AP after a successful connect and STA later
 * drops, bring the AP back so users can always fall through to the portal.
 * Called only from the disconnect path.
 */
static void reopen_ap_if_needed(void)
{
    if (s_mode != WM_STATE_STA_ONLY) return;
    ESP_LOGI(TAG, "Reopening AP after STA disconnect (was closed via close_on_sta)");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reopen AP: %s", esp_err_to_name(err));
        return;
    }
    apply_ap_config();
    s_mode = WM_STATE_APSTA;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (s_sta_configured) {
                ESP_LOGI(TAG, "STA start: ssid_len=%u auth_threshold=%s mode=%s",
                         (unsigned)strlen(s_config.sta_ssid),
                         wifi_manager_sta_password_is_set() ? "wpa2_psk" : "open",
                         wifi_manager_mode_string(s_mode));
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Wi-Fi STA not configured, skipping connection");
            }
            return;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disc = event_data;
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
            reopen_ap_if_needed();
            ESP_LOGI(TAG, "STA disconnected: reason=%u next_retry_ms=%d ap_active=%d mode=%s",
                     (unsigned)reason, WIFI_RETRY_MS, s_ap_active, wifi_manager_mode_string(s_mode));
            arm_reconnect();
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
        ip_event_got_ip_t *event = event_data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        if (wifi_manager_close_on_sta() && s_ap_active) {
            ESP_LOGI(TAG, "STA connected, closing AP per ap_behavior=close_on_sta");
            esp_err_t ap_err = esp_wifi_set_mode(WIFI_MODE_STA);
            if (ap_err == ESP_OK) {
                s_ap_active = false;
                s_mode = WM_STATE_STA_ONLY;
            } else {
                ESP_LOGW(TAG, "Failed to switch to STA-only mode: %s", esp_err_to_name(ap_err));
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

    if (!force && s_initialized && s_last_connected == s_connected && s_last_ap_active == s_ap_active) return;
    s_last_connected = s_connected;
    s_last_ap_active = s_ap_active;
    s_initialized = true;
    if (s_state_cb) s_state_cb(s_connected, s_state_cb_user_ctx);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_sta_configured) return;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "esp_wifi_connect failed: %s; retrying in %d ms", esp_err_to_name(err), WIFI_RETRY_MS);
        arm_reconnect();
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_wifi_event_group) return ESP_OK;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    const esp_timer_create_args_t timer_args = { .callback = reconnect_timer_cb, .name = "wifi_reconnect" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    memset(&s_config, 0, sizeof(s_config));
    s_wifi_started = false;
    compose_ap_ssid();
    return ESP_OK;
}

esp_err_t wifi_manager_start(const wifi_manager_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    esp_err_t err = configure_sta_mode(config);
    if (err != ESP_OK) return err;

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_wifi_started = true;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_apply_sta_config(const wifi_manager_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;

    bool was_connected = s_connected;
    esp_err_t err = configure_sta_mode(config);
    if (err != ESP_OK) return err;

    if (was_connected) notify_state_changed(false);

    if (!s_wifi_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_wifi_started = true;
    }

    if (!s_sta_configured) return ESP_OK;

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) return err;
    err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) return err;
    return ESP_OK;
}

esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms)
{
    if (!s_sta_configured) return ESP_ERR_INVALID_STATE;

    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, ticks);
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_manager_register_state_callback(wifi_manager_state_cb_t cb, void *user_ctx)
{
    s_state_cb = cb;
    s_state_cb_user_ctx = user_ctx;
    if (s_state_cb) s_state_cb(s_connected, s_state_cb_user_ctx);
    return ESP_OK;
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (!status) return;
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

esp_err_t wifi_manager_scan_aps(wifi_manager_scan_record_t *records, uint16_t max_records, uint16_t *out_count)
{
    wifi_mode_t original_mode = WIFI_MODE_NULL;
    wifi_mode_t scan_mode = WIFI_MODE_NULL;
    wifi_scan_config_t scan_cfg = { .show_hidden = true };
    wifi_ap_record_t *ap_records = NULL;
    uint16_t ap_count = 0;
    esp_err_t err;

    if (!records || max_records == 0 || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

    err = esp_wifi_get_mode(&original_mode);
    if (err != ESP_OK) return err;

    scan_mode = original_mode;
    if (original_mode == WIFI_MODE_AP) {
        scan_mode = WIFI_MODE_APSTA;
        err = esp_wifi_set_mode(scan_mode);
        if (err != ESP_OK) return err;
    } else if (original_mode != WIFI_MODE_STA && original_mode != WIFI_MODE_APSTA) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) goto cleanup;

    err = esp_wifi_scan_get_ap_num(&ap_count);
    if (err != ESP_OK) goto cleanup;

    if (ap_count == 0) { err = ESP_OK; goto cleanup; }
    if (ap_count > max_records) ap_count = max_records;

    ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (!ap_records) { err = ESP_ERR_NO_MEM; goto cleanup; }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) goto cleanup;

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
        if (err == ESP_OK && restore_err != ESP_OK) err = restore_err;
    }
    return err;
}
