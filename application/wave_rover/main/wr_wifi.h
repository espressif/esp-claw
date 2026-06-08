#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "wave_rover_config.h"

esp_err_t   wr_wifi_init(const wave_rover_config_t *cfg);
bool        wr_wifi_is_connected(void);
const char *wr_wifi_get_ip(void);

/* Returns the current STA AP's RSSI in dBm via esp_wifi_sta_get_ap_info().
 * Returns an error (e.g. ESP_ERR_WIFI_NOT_CONNECT) when not connected to an
 * AP — caller should skip emitting the metric in that case. */
esp_err_t   wr_wifi_get_rssi(int8_t *out_rssi_dbm);
