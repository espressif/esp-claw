/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of characters (excluding the null terminator) for the various
 * string fields exposed in the status snapshot. Kept small to bound RAM. */
#define MQTT_MANAGER_BROKER_MAX     128
#define MQTT_MANAGER_CLIENT_ID_MAX  64
#define MQTT_MANAGER_TOPIC_MAX      128
#define MQTT_MANAGER_DEVICE_ID_LEN  16

typedef enum {
    MQTT_MANAGER_STATE_DISABLED = 0,   /* not configured / not started */
    MQTT_MANAGER_STATE_DISCONNECTED,   /* started, waiting/backing off */
    MQTT_MANAGER_STATE_CONNECTING,     /* TCP/MQTT handshake in progress */
    MQTT_MANAGER_STATE_CONNECTED,      /* session established */
} mqtt_manager_state_t;

/*
 * Connection parameters. Strings are copied into the manager on init, so the
 * caller does not need to keep them alive. All fields are optional except
 * `broker`; sensible defaults are applied for the rest.
 */
typedef struct {
    const char *broker;        /* hostname or IP, no scheme (required) */
    uint16_t    port;          /* 0 => 1883 (or 8883 when tls) */
    bool        tls;           /* wrap the transport in TLS */
    const char *username;      /* optional */
    const char *password;      /* optional (secret, never logged) */
    const char *client_id;     /* empty/NULL => "espclaw-<device_id>" */
    uint16_t    keepalive;     /* seconds, 0 => 60 */
    uint8_t     default_qos;   /* default publish QoS (0 or 1) */
    const char *base_topic;    /* topic root, empty/NULL => "espclaw" */
} mqtt_manager_config_t;

typedef struct {
    mqtt_manager_state_t state;
    bool     tls;
    uint16_t port;
    uint8_t  default_qos;
    char     broker[MQTT_MANAGER_BROKER_MAX];
    char     client_id[MQTT_MANAGER_CLIENT_ID_MAX];
    char     base_topic[MQTT_MANAGER_TOPIC_MAX];
    char     device_id[MQTT_MANAGER_DEVICE_ID_LEN];
    uint32_t tx_count;             /* messages published since boot */
    uint32_t rx_count;             /* messages received since boot */
    uint32_t reconnect_count;      /* successful (re)connections */
    int64_t  connected_since_us;   /* esp_timer time of last connect, 0 if down */
    int      last_error;           /* esp_err_t of last transport error */
} mqtt_manager_status_t;

/*
 * Called from the MQTT client task for every message received on a subscribed
 * topic. `topic`/`data` are NOT null-terminated across the whole payload for
 * very large messages; use the provided lengths. Keep the callback short and
 * non-blocking — hand off heavy work to another task/queue.
 */
typedef void (*mqtt_manager_message_cb_t)(const char *topic, int topic_len,
                                          const char *data, int data_len,
                                          void *user_ctx);

/* Lifecycle. init() may be called again to apply a new configuration; it stops
 * any running client first. */
esp_err_t mqtt_manager_init(const mqtt_manager_config_t *config);
esp_err_t mqtt_manager_start(void);
esp_err_t mqtt_manager_stop(void);
bool      mqtt_manager_is_connected(void);
void      mqtt_manager_get_status(mqtt_manager_status_t *out_status);

/* Publish. `len < 0` treats `data` as a C string. Returns ESP_ERR_INVALID_STATE
 * when not connected. On success and out_msg_id != NULL, the broker message id
 * (or 0 for QoS 0) is written. */
esp_err_t mqtt_manager_publish(const char *topic, const char *data, int len,
                               int qos, bool retain, int *out_msg_id);

esp_err_t mqtt_manager_subscribe(const char *topic, int qos);
esp_err_t mqtt_manager_unsubscribe(const char *topic);
esp_err_t mqtt_manager_register_message_cb(mqtt_manager_message_cb_t cb, void *user_ctx);

/* Stable per-device id derived from the factory MAC (lower 3 bytes, lowercase
 * hex, e.g. "84aee5"). Valid after mqtt_manager_init(). */
const char *mqtt_manager_get_device_id(void);

/* Build "<base_topic>/<device_id>/<suffix>" into out. Returns ESP_ERR_INVALID_SIZE
 * if it does not fit. */
esp_err_t mqtt_manager_build_topic(const char *suffix, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
