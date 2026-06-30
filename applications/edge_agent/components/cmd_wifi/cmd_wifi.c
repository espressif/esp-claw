/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_wifi.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "wifi_manager.h"

#define WIFI_SCAN_LIMIT 20
#define TAG             "CMD_WIFI"

/*
 * Structured output format: space-separated key=value pairs on a single log line.
 *
 * Fields that may contain spaces (e.g. ssid, msg) are placed LAST so they can be
 * captured unambiguously with a greedy regex:  ssid=(.+)$
 *
 * Regex examples (Python):
 *   ok      = re.search(r'ok=(\d)', line).group(1)
 *   rssi    = re.search(r'rssi=(-?\d+)', line).group(1)
 *   ssid    = re.search(r'ssid=(.+)$', line).group(1).strip()
 */

static struct {
    struct arg_lit *status;
    struct arg_lit *scan;
    struct arg_lit *set;
    struct arg_lit *apply;
    struct arg_str *ssid;
    struct arg_str *password;
    struct arg_end *end;
} wifi_args;

static const char *wifi_auth_mode_to_string(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "wep";
    case WIFI_AUTH_WPA_PSK:         return "wpa_psk";
    case WIFI_AUTH_WPA2_PSK:        return "wpa2_psk";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa_wpa2_psk";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK:        return "wpa3_psk";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2_wpa3_psk";
    case WIFI_AUTH_WAPI_PSK:        return "wapi_psk";
    default:                        return "unknown";
    }
}

/* Returns 1 so callers can propagate it as an error exit code. */
static int log_error(const char *command, esp_err_t err, const char *message)
{
    ESP_LOGW(TAG, "cmd=%s ok=0 err=%s msg=%s", command, esp_err_to_name(err), message);
    return 1;
}

/* Emit a status line; ssid field is last to tolerate spaces in the SSID value. */
static void log_status_line(const char *command,
                            const wifi_manager_status_t *st,
                            const app_config_t *cfg,
                            const char *extra_msg)
{
    const char *sta_ip  = st->sta_ip[0]  ? st->sta_ip  : "-";
    const char *ap_ip   = st->ap_ip[0]   ? st->ap_ip   : "-";
    const char *ap_ssid = st->ap_ssid[0] ? st->ap_ssid : "-";
    const char *mode    = st->mode[0]    ? st->mode    : "-";
    int saved_configured = cfg && cfg->wifi_ssid[0] != '\0';
    const char *saved_ssid = (cfg && cfg->wifi_ssid[0]) ? cfg->wifi_ssid : "-";

    if (extra_msg) {
        ESP_LOGI(TAG, "cmd=%s ok=1 msg=%s "
                 "sta_connected=%d ap_active=%d sta_configured=%d "
                 "sta_ip=%s ap_ip=%s ap_ssid=%s mode=%s "
                 "saved_configured=%d saved_ssid=%s",
                 command, extra_msg,
                 st->sta_connected, st->ap_active, st->sta_configured,
                 sta_ip, ap_ip, ap_ssid, mode,
                 saved_configured, saved_ssid);
    } else {
        ESP_LOGI(TAG, "cmd=%s ok=1 "
                 "sta_connected=%d ap_active=%d sta_configured=%d "
                 "sta_ip=%s ap_ip=%s ap_ssid=%s mode=%s "
                 "saved_configured=%d saved_ssid=%s",
                 command,
                 st->sta_connected, st->ap_active, st->sta_configured,
                 sta_ip, ap_ip, ap_ssid, mode,
                 saved_configured, saved_ssid);
    }
}

static int cmd_wifi_status(void)
{
    app_config_t config = {0};
    wifi_manager_status_t status = {0};
    esp_err_t err = app_config_load(&config);

    if (err != ESP_OK) {
        return log_error("status", err, "failed_to_load_saved_config");
    }

    wifi_manager_get_status(&status);
    log_status_line("status", &status, &config, NULL);
    return 0;
}

static int cmd_wifi_scan(void)
{
    wifi_manager_scan_record_t records[WIFI_SCAN_LIMIT] = {0};
    uint16_t count = 0;
    esp_err_t err = wifi_manager_scan_aps(records, WIFI_SCAN_LIMIT, &count);

    if (err != ESP_OK) {
        return log_error("scan", err, "wifi_scan_failed");
    }

    ESP_LOGI(TAG, "cmd=scan ok=1 count=%u", count);

    for (uint16_t i = 0; i < count; ++i) {
        /* ssid is last: regex  ssid=(.+)$  captures it even with spaces */
        ESP_LOGI(TAG, "ap idx=%u rssi=%d ch=%u auth=%s ssid=%s",
                 i,
                 records[i].rssi,
                 records[i].primary,
                 wifi_auth_mode_to_string(records[i].authmode),
                 records[i].ssid[0] ? records[i].ssid : "-");
    }
    return 0;
}

static int cmd_wifi_apply_loaded_config(const app_config_t *config, const char *command_name)
{
    wifi_manager_status_t status = {0};
    const char *validation_message = NULL;
    esp_err_t err = app_config_validate_wifi(config, &validation_message);
    if (err != ESP_OK) {
        return log_error(command_name, err, validation_message ? validation_message : "invalid_ap_config");
    }

    wifi_manager_config_t wifi_config = {
        .sta_ssid = config->wifi_ssid,
        .sta_password = config->wifi_password,
        .ap_ssid = config->ap_ssid[0] ? config->ap_ssid : NULL,
        .ap_password = config->ap_password[0] ? config->ap_password : NULL,
        .ap_behavior = config->ap_behavior,
    };

    err = wifi_manager_apply_sta_config(&wifi_config);

    if (err != ESP_OK) {
        return log_error(command_name, err, "failed_to_apply_config");
    }

    wifi_manager_get_status(&status);
    log_status_line(command_name, &status, config, "config_applied");
    return 0;
}

static int cmd_wifi_apply(void)
{
    app_config_t config = {0};
    esp_err_t err = app_config_load(&config);

    if (err != ESP_OK) {
        return log_error("apply", err, "failed_to_load_saved_config");
    }

    return cmd_wifi_apply_loaded_config(&config, "apply");
}

static int cmd_wifi_set(bool apply_now)
{
    app_config_t config = {0};
    esp_err_t err = app_config_load(&config);

    if (err != ESP_OK) {
        return log_error("set", err, "failed_to_load_saved_config");
    }

    if (!wifi_args.ssid->count) {
        return log_error("set", ESP_ERR_INVALID_ARG, "missing_ssid");
    }

    strlcpy(config.wifi_ssid, wifi_args.ssid->sval[0], sizeof(config.wifi_ssid));
    if (wifi_args.password->count) {
        strlcpy(config.wifi_password, wifi_args.password->sval[0], sizeof(config.wifi_password));
    }

    const char *validation_message = NULL;
    err = app_config_validate_wifi(&config, &validation_message);
    if (err != ESP_OK) {
        return log_error("set", err, validation_message ? validation_message : "invalid_ap_config");
    }

    err = app_config_save(&config);
    if (err != ESP_OK) {
        return log_error("set", err, "failed_to_save_config");
    }

    if (apply_now) {
        return cmd_wifi_apply_loaded_config(&config, "set");
    }

    /* ssid last to tolerate spaces */
    ESP_LOGI(TAG, "cmd=set ok=1 msg=config_saved applied=0 ssid=%s", config.wifi_ssid);
    return 0;
}

static int wifi_func(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wifi_args);
    int op_count = wifi_args.status->count + wifi_args.scan->count + wifi_args.set->count;

    if (nerrors != 0) {
        return log_error("parse", ESP_ERR_INVALID_ARG, "invalid_arguments");
    }

    if (wifi_args.apply->count && !wifi_args.set->count) {
        op_count += wifi_args.apply->count;
    }

    if (op_count != 1) {
        return log_error("parse", ESP_ERR_INVALID_ARG,
                         "exactly_one_of_status_scan_set_apply_required");
    }

    if (wifi_args.status->count) {
        return cmd_wifi_status();
    }
    if (wifi_args.scan->count) {
        return cmd_wifi_scan();
    }
    if (wifi_args.set->count) {
        return cmd_wifi_set(wifi_args.apply->count > 0);
    }
    return cmd_wifi_apply();
}

void register_wifi_command(void)
{
    wifi_args.status   = arg_lit0(NULL, "status",   "Print Wi-Fi runtime and saved config status");
    wifi_args.scan     = arg_lit0(NULL, "scan",     "Scan nearby Wi-Fi APs");
    wifi_args.set      = arg_lit0(NULL, "set",      "Save Wi-Fi credentials");
    wifi_args.apply    = arg_lit0(NULL, "apply",    "Apply saved Wi-Fi config, or combine with --set");
    wifi_args.ssid     = arg_str0(NULL, "ssid",     "<ssid>",     "Wi-Fi SSID used by --set");
    wifi_args.password = arg_str0(NULL, "password", "<password>", "Wi-Fi password used by --set");
    wifi_args.end = arg_end(8);

    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Wi-Fi operations (key=value structured log output via ESP_LOGI/LOGW).\n"
                "Examples:\n"
                " wifi --status\n"
                " wifi --scan\n"
                " wifi --set --ssid MyWiFi --password secret\n"
                " wifi --set --ssid MyWiFi --password secret --apply\n"
                " wifi --apply\n",
        .func = wifi_func,
        .argtable = &wifi_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));
}
