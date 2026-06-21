/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include "wave_rover_config.h"
#include "wave_rover_power_mgr.h"

/* Registers GET /metrics — a Prometheus text-exposition endpoint covering
 * power, motor/state, IMU, and system-health telemetry. Mirrors
 * wr_mcp_web_register's signature and ownership model: the caller owns
 * `server` and is responsible for starting/stopping the httpd instance. */
esp_err_t wr_mcp_metrics_register(httpd_handle_t server, const wave_rover_config_t *cfg);
void      wr_mcp_metrics_set_power_mgr(wr_power_mgr_handle_t pm);
