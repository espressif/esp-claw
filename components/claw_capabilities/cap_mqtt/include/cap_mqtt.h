/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool        enabled;
    const char *broker;
    uint16_t    port;
    bool        tls;
    const char *username;
    const char *password;
    const char *client_id;
    uint16_t    keepalive;
    uint8_t     default_qos;
    const char *base_topic;
} cap_mqtt_config_t;

/* Apply configuration and (re)start or stop the underlying MQTT client. Safe to
 * call multiple times; when config->enabled is false the client is stopped. */
esp_err_t cap_mqtt_apply_config(const cap_mqtt_config_t *config);

/* Register the mqtt_publish / mqtt_status / mqtt_subscribe agent tools. */
esp_err_t cap_mqtt_register_group(void);

#ifdef __cplusplus
}
#endif
