/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Waveshare ESP32-S3-Zero N8R8 board-specific device includes.
 * The onboard WS2812 is declared as an init-skipped led_strip device; Lua can
 * create the LED strip driver on demand without Board Manager owning it at boot.
 */
#include "esp_board_manager_includes.h"
#include "gen_board_device_custom.h"
