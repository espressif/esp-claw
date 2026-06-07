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

/* Call once at boot before any logging (hooks esp_log_set_vprintf). */
esp_err_t wr_syslog_init(void);

/* Call after WiFi has an IP — (re)opens the UDP forwarder socket.
 * host: target IP string, or NULL/empty for subnet broadcast.
 * port: 0 selects the default port (5514).
 * facility: syslog facility number (0-23) used in the PRI header. */
void wr_syslog_start(bool enabled, const char *host, uint16_t port, uint8_t facility);

#ifdef __cplusplus
}
#endif
