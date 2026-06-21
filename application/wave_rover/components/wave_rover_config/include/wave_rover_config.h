/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WR_CFG_SSID_LEN      64
#define WR_CFG_PASS_LEN      64
#define WR_CFG_HOST_LEN      32
#define WR_CFG_TOKEN_LEN     64

typedef struct {
    char     wifi_ssid[WR_CFG_SSID_LEN];
    char     wifi_password[WR_CFG_PASS_LEN];
    char     wifi_ap_ssid[WR_CFG_SSID_LEN];
    char     wifi_ap_password[WR_CFG_PASS_LEN];
    uint8_t  wifi_mode;                         /* 0=ap, 1=sta, 2=ap_sta */
    char     hostname[WR_CFG_HOST_LEN];
    uint16_t mcp_port;
    bool     auth_enabled;
    char     auth_token[WR_CFG_TOKEN_LEN];
    bool     safe_mode;
    float    max_speed;                         /* [0.0, 1.0] */
    uint16_t max_command_duration_ms;
    bool     syslog_enabled;
    char     syslog_host[WR_CFG_HOST_LEN];       /* empty = subnet broadcast */
    uint16_t syslog_port;
    uint8_t  syslog_facility;                    /* syslog facility 0-23 */

    /* Power management (Wave Rover power optimization) */
    bool     power_mgr_enabled;            /* default true */
    uint16_t power_active_timeout_sec;     /* ACTIVE -> IDLE after this many idle seconds */
    uint16_t power_idle_to_low_power_sec;  /* total idle seconds before LOW_POWER */
    bool     power_wifi_power_save;        /* enable esp_wifi_set_ps() per mode */
    bool     power_reduce_cpu_frequency;   /* enable esp_pm DFS per mode */
    bool     power_disable_display_idle;   /* turn SSD1306 off in LOW_POWER */
    float    power_critical_battery_v;     /* forces LOW_POWER + motor stop below this */
    uint16_t power_telemetry_active_sec;   /* INA219 poll interval in ACTIVE */
    uint16_t power_telemetry_idle_sec;     /* INA219 poll interval in IDLE */
    uint16_t power_telemetry_low_power_sec; /* INA219 poll interval in LOW_POWER */
} wave_rover_config_t;

esp_err_t wave_rover_config_init(void);
esp_err_t wave_rover_config_load(wave_rover_config_t *cfg);
esp_err_t wave_rover_config_save(const wave_rover_config_t *cfg);
void      wave_rover_config_defaults(wave_rover_config_t *cfg);

#ifdef __cplusplus
}
#endif
