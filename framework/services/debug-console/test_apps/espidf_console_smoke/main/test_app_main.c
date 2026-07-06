/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "debug_console_test";

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
#define DEBUG_CONSOLE_TEST_BACKEND "uart"
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#define DEBUG_CONSOLE_TEST_BACKEND "usb_serial_jtag"
#elif CONFIG_ESP_CONSOLE_USB_CDC
#define DEBUG_CONSOLE_TEST_BACKEND "usb_cdc"
#else
#error "debug-console smoke test requires one supported ESP console backend"
#endif

extern esp_err_t claw_debug_console_espidf_init(void);

void app_main(void)
{
    ESP_ERROR_CHECK(claw_debug_console_espidf_init());
    ESP_LOGI(TAG, "selected backend: %s", DEBUG_CONSOLE_TEST_BACKEND);
    ESP_LOGI(TAG, "debug-console espidf backend initialized");
}
