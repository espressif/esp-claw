/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_unitv.h"
#include "cap_unitv_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "cmd_cap_unitv";

static int cmd_unitv_scan(int argc, char **argv)
{
    char resp[1024] = {0};
    (void)argc;
    (void)argv;

    esp_err_t err = cap_unitv_uart_cmd("SCAN", "{\"mode\":\"FAST\",\"frames\":1}",
                                       resp, sizeof(resp), 5000);
    if (err != ESP_OK) {
        printf("scan: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("scan: %s\n", resp);
    return 0;
}

static int cmd_unitv_capture(int argc, char **argv)
{
    int quality = argc >= 2 ? atoi(argv[1]) : 75;
    uint8_t *jpeg = NULL;
    size_t jpeg_size = 0;
    esp_err_t err = cap_unitv_uart_capture_jpeg(quality, &jpeg, &jpeg_size);
    if (err != ESP_OK) {
        printf("capture: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("capture: %u bytes\n", (unsigned)jpeg_size);
    free(jpeg);
    return 0;
}

void cap_unitv_register_cli(void)
{
    const esp_console_cmd_t scan_cmd = {
        .command = "unitv_scan",
        .help = "UnitV SCAN command",
        .func = cmd_unitv_scan,
    };
    const esp_console_cmd_t capture_cmd = {
        .command = "unitv_capture",
        .help = "UnitV CAPTURE; optional arg: quality 30-95",
        .func = cmd_unitv_capture,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&scan_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&capture_cmd));
    ESP_LOGI(TAG, "CLI commands registered");
}
