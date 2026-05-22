/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
#include "rover_s3_settings.h"

extern rover_s3_settings_t g_settings;

esp_err_t app_rover_s3_start(void);
