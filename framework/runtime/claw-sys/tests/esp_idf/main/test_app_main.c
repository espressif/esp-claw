/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdio.h>

#include "esp_log.h"
#include "test_secrets.h"
#include "test_wifi.h"
#include "unity.h"
#include "unity_test_runner.h"

static const char *TAG = "claw_sys_test_app";

void app_main(void)
{
    /* Bring Wi-Fi up once so the [network] cases have connectivity. A failure
     * is logged (not fatal): the non-network cases still run and the network
     * cases will fail loudly. */
    if (test_wifi_connect(TEST_WIFI_SSID, TEST_WIFI_PASS) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connect failed; [network] cases will fail");
    }

    printf("  ___ _      ___      __  _____   _____ ___ ___ _____\r\n");
    printf(" / __| |    /_\\ \\    / / / __\\ \\ / / __|_   _| __/__   \\\r\n");
    printf("| (__| |__ / _ \\ \\/\\/ /  \\__ \\\\ V /\\__ \\ | | | _|  | |\r\n");
    printf(" \\___|____/_/ \\_\\_/\\_/   |___/ |_| |___/ |_| |___| |_|\r\n");
    unity_run_menu();
}
