#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "wave_rover_config.h"

esp_err_t   wr_wifi_init(const wave_rover_config_t *cfg);
bool        wr_wifi_is_connected(void);
const char *wr_wifi_get_ip(void);
