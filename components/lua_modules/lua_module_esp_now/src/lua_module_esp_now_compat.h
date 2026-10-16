/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/*
 * ESP-IDF 5.5.x / 6.x compatibility helpers for the ESP-NOW Lua module.
 *
 * The only ESP-NOW API surface that changed shape between the supported IDF
 * ranges is the send callback's tx_info. Since IDF 5.5 the send callback is
 *   void (*)(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
 * and esp_now_send_info_t is a typedef of wifi_tx_info_t, whose des_addr /
 * src_addr are pointers to 6-byte MAC addresses in both 5.5.x and 6.x.
 *
 * Everything else the module uses (recv callback esp_now_recv_info_t, peer
 * management, esp_now_set_pmk / set_wake_window, WIFI_IF_STA / WIFI_IF_AP) is
 * source-compatible across 5.5.x and 6.x, so no version guards are needed
 * elsewhere. The module deliberately avoids APIs removed in 6.0 such as the
 * ESP_IF_WIFI_* macros and esp_wifi_config_espnow_rate().
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Copy the destination MAC (6 bytes) from a send-callback tx_info.
 *
 * Isolated here so the one layout-sensitive access lives in a single place.
 *
 * @param tx_info Send callback information pointer.
 * @param out_mac Output buffer of at least 6 bytes.
 * @return true if a destination address was available and copied.
 */
static inline bool lua_espnow_send_info_dest(const esp_now_send_info_t *tx_info, uint8_t out_mac[6])
{
    if (tx_info == NULL || tx_info->des_addr == NULL) {
        return false;
    }
    memcpy(out_mac, tx_info->des_addr, ESP_NOW_ETH_ALEN);
    return true;
}

#ifdef __cplusplus
}
#endif
