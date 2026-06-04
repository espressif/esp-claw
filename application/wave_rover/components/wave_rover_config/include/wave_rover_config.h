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
    bool     dry_run;                           /* true=no real hardware */
    float    max_speed;                         /* [0.0, 1.0] */
    uint16_t max_command_duration_ms;
} wave_rover_config_t;

esp_err_t wave_rover_config_init(void);
esp_err_t wave_rover_config_load(wave_rover_config_t *cfg);
esp_err_t wave_rover_config_save(const wave_rover_config_t *cfg);
void      wave_rover_config_defaults(wave_rover_config_t *cfg);

#ifdef __cplusplus
}
#endif
