/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mqtt_manager.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt_manager";

#define MQTT_MANAGER_USERNAME_MAX 128
#define MQTT_MANAGER_PASSWORD_MAX 128
#define MQTT_MANAGER_KEEPALIVE_DEFAULT 60
#define MQTT_MANAGER_STATUS_ONLINE  "online"
#define MQTT_MANAGER_STATUS_OFFLINE "offline"

/* Exponential backoff schedule (seconds) used when auto-reconnect is disabled so
 * we control the cadence ourselves: gentle on the radio, the CPU and the broker. */
static const uint32_t s_backoff_seconds[] = { 1, 2, 4, 8, 16, 30, 60 };
#define MQTT_MANAGER_BACKOFF_COUNT (sizeof(s_backoff_seconds) / sizeof(s_backoff_seconds[0]))

typedef struct {
    bool                     initialized;
    bool                     running;      /* start() called, should stay connected */
    mqtt_manager_state_t     state;

    esp_mqtt_client_handle_t client;
    esp_timer_handle_t       reconnect_timer;
    size_t                   backoff_index;

    /* configuration snapshot */
    char     broker[MQTT_MANAGER_BROKER_MAX];
    uint16_t port;
    bool     tls;
    char     username[MQTT_MANAGER_USERNAME_MAX];
    char     password[MQTT_MANAGER_PASSWORD_MAX];
    char     client_id[MQTT_MANAGER_CLIENT_ID_MAX];
    uint16_t keepalive;
    uint8_t  default_qos;
    char     base_topic[MQTT_MANAGER_TOPIC_MAX];

    /* derived */
    char device_id[MQTT_MANAGER_DEVICE_ID_LEN];
    char status_topic[MQTT_MANAGER_TOPIC_MAX];
    char command_topic[MQTT_MANAGER_TOPIC_MAX];
    char config_topic[MQTT_MANAGER_TOPIC_MAX];

    /* counters / diagnostics */
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t reconnect_count;
    int64_t  connected_since_us;
    int      last_error;

    mqtt_manager_message_cb_t message_cb;
    void                     *message_ctx;
} mqtt_manager_t;

static mqtt_manager_t s_mgr;

/* ── Helpers ────────────────────────────────────────────────────────── */

static void mqtt_manager_derive_device_id(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        strlcpy(s_mgr.device_id, "000000", sizeof(s_mgr.device_id));
        return;
    }
    snprintf(s_mgr.device_id, sizeof(s_mgr.device_id), "%02x%02x%02x", mac[3], mac[4], mac[5]);
}

esp_err_t mqtt_manager_build_topic(const char *suffix, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    int written = snprintf(out, out_size, "%s/%s/%s",
                           s_mgr.base_topic[0] ? s_mgr.base_topic : "espclaw",
                           s_mgr.device_id,
                           suffix ? suffix : "");
    if (written < 0 || (size_t)written >= out_size) {
        out[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

const char *mqtt_manager_get_device_id(void)
{
    return s_mgr.device_id;
}

static uint32_t mqtt_manager_next_backoff_ms(void)
{
    uint32_t seconds = s_backoff_seconds[s_mgr.backoff_index];
    if (s_mgr.backoff_index + 1 < MQTT_MANAGER_BACKOFF_COUNT) {
        s_mgr.backoff_index++;
    }
    return seconds * 1000U;
}

static void mqtt_manager_arm_reconnect(void)
{
    if (!s_mgr.reconnect_timer || !s_mgr.running) {
        return;
    }
    esp_timer_stop(s_mgr.reconnect_timer);
    uint32_t delay_ms = mqtt_manager_next_backoff_ms();
    esp_err_t err = esp_timer_start_once(s_mgr.reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to arm reconnect timer: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Reconnect scheduled in %u ms", (unsigned)delay_ms);
    }
}

static void mqtt_manager_reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!s_mgr.running || !s_mgr.client) {
        return;
    }
    ESP_LOGI(TAG, "Attempting reconnect to %s:%u", s_mgr.broker, (unsigned)s_mgr.port);
    esp_err_t err = esp_mqtt_client_reconnect(s_mgr.client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reconnect failed: %s", esp_err_to_name(err));
        mqtt_manager_arm_reconnect();
    }
}

static void mqtt_manager_on_connected(void)
{
    s_mgr.state = MQTT_MANAGER_STATE_CONNECTED;
    s_mgr.backoff_index = 0;
    s_mgr.reconnect_count++;
    s_mgr.connected_since_us = esp_timer_get_time();
    if (s_mgr.reconnect_timer) {
        esp_timer_stop(s_mgr.reconnect_timer);
    }
    ESP_LOGI(TAG, "Connected (client_id=%s, device_id=%s)", s_mgr.client_id, s_mgr.device_id);

    /* Announce presence (retained) so late subscribers see we are online. */
    esp_mqtt_client_publish(s_mgr.client, s_mgr.status_topic,
                            MQTT_MANAGER_STATUS_ONLINE, 0, 1, 1);
    /* Listen for inbound command/config traffic addressed to this device. */
    esp_mqtt_client_subscribe(s_mgr.client, s_mgr.command_topic, 1);
    esp_mqtt_client_subscribe(s_mgr.client, s_mgr.config_topic, 1);
}

static void mqtt_manager_event_handler(void *args, esp_event_base_t base,
                                       int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_manager_on_connected();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mgr.state = MQTT_MANAGER_STATE_DISCONNECTED;
        s_mgr.connected_since_us = 0;
        ESP_LOGW(TAG, "Disconnected from broker");
        mqtt_manager_arm_reconnect();
        break;
    case MQTT_EVENT_DATA:
        s_mgr.rx_count++;
        if (s_mgr.message_cb) {
            s_mgr.message_cb(event->topic, event->topic_len,
                             event->data, event->data_len, s_mgr.message_ctx);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle) {
            s_mgr.last_error = event->error_handle->esp_transport_sock_errno;
            ESP_LOGE(TAG, "MQTT error (type=%d, tls_stack=0x%x)",
                     event->error_handle->error_type,
                     event->error_handle->esp_tls_stack_err);
        }
        break;
    default:
        break;
    }
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

esp_err_t mqtt_manager_init(const mqtt_manager_config_t *config)
{
    if (!config || !config->broker || !config->broker[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Reconfiguring: tear down any running client first. */
    if (s_mgr.client) {
        mqtt_manager_stop();
        esp_mqtt_client_destroy(s_mgr.client);
        s_mgr.client = NULL;
    }

    mqtt_manager_message_cb_t saved_cb = s_mgr.message_cb;
    void *saved_ctx = s_mgr.message_ctx;
    esp_timer_handle_t saved_timer = s_mgr.reconnect_timer;
    memset(&s_mgr, 0, sizeof(s_mgr));
    s_mgr.message_cb = saved_cb;
    s_mgr.message_ctx = saved_ctx;
    s_mgr.reconnect_timer = saved_timer;

    strlcpy(s_mgr.broker, config->broker, sizeof(s_mgr.broker));
    s_mgr.tls = config->tls;
    s_mgr.port = config->port ? config->port : (config->tls ? 8883 : 1883);
    s_mgr.keepalive = config->keepalive ? config->keepalive : MQTT_MANAGER_KEEPALIVE_DEFAULT;
    s_mgr.default_qos = (config->default_qos == 1) ? 1 : 0;
    if (config->username) {
        strlcpy(s_mgr.username, config->username, sizeof(s_mgr.username));
    }
    if (config->password) {
        strlcpy(s_mgr.password, config->password, sizeof(s_mgr.password));
    }
    strlcpy(s_mgr.base_topic,
            (config->base_topic && config->base_topic[0]) ? config->base_topic : "espclaw",
            sizeof(s_mgr.base_topic));

    mqtt_manager_derive_device_id();

    if (config->client_id && config->client_id[0]) {
        strlcpy(s_mgr.client_id, config->client_id, sizeof(s_mgr.client_id));
    } else {
        snprintf(s_mgr.client_id, sizeof(s_mgr.client_id), "espclaw-%s", s_mgr.device_id);
    }

    mqtt_manager_build_topic("status", s_mgr.status_topic, sizeof(s_mgr.status_topic));
    mqtt_manager_build_topic("command", s_mgr.command_topic, sizeof(s_mgr.command_topic));
    mqtt_manager_build_topic("config", s_mgr.config_topic, sizeof(s_mgr.config_topic));

    if (!s_mgr.reconnect_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = mqtt_manager_reconnect_timer_cb,
            .name = "mqtt_reconnect",
        };
        esp_err_t err = esp_timer_create(&timer_args, &s_mgr.reconnect_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create reconnect timer: %s", esp_err_to_name(err));
            return err;
        }
    }

    s_mgr.state = MQTT_MANAGER_STATE_DISABLED;
    s_mgr.initialized = true;
    ESP_LOGI(TAG, "Initialized: broker=%s port=%u tls=%d client_id=%s base_topic=%s (user=%s)",
             s_mgr.broker, (unsigned)s_mgr.port, (int)s_mgr.tls, s_mgr.client_id,
             s_mgr.base_topic, s_mgr.username[0] ? "set" : "none");
    return ESP_OK;
}

esp_err_t mqtt_manager_start(void)
{
    if (!s_mgr.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mgr.running) {
        return ESP_OK;
    }

    esp_mqtt_client_config_t cfg = {
        .broker = {
            .address = {
                .hostname = s_mgr.broker,
                .port = s_mgr.port,
                .transport = s_mgr.tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP,
            },
        },
        .credentials = {
            .client_id = s_mgr.client_id,
        },
        .session = {
            .keepalive = s_mgr.keepalive,
            .last_will = {
                .topic = s_mgr.status_topic,
                .msg = MQTT_MANAGER_STATUS_OFFLINE,
                .msg_len = (int)strlen(MQTT_MANAGER_STATUS_OFFLINE),
                .qos = 1,
                .retain = 1,
            },
        },
        .network = {
            /* We drive reconnection ourselves for exponential backoff. */
            .disable_auto_reconnect = true,
        },
    };

    if (s_mgr.username[0]) {
        cfg.credentials.username = s_mgr.username;
    }
    if (s_mgr.password[0]) {
        cfg.credentials.authentication.password = s_mgr.password;
    }
    if (s_mgr.tls) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_mgr.client = esp_mqtt_client_init(&cfg);
    if (!s_mgr.client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_mgr.client, ESP_EVENT_ANY_ID,
                                                   mqtt_manager_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_event failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(s_mgr.client);
        s_mgr.client = NULL;
        return err;
    }

    s_mgr.running = true;
    s_mgr.backoff_index = 0;
    s_mgr.state = MQTT_MANAGER_STATE_CONNECTING;
    err = esp_mqtt_client_start(s_mgr.client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client_start failed: %s", esp_err_to_name(err));
        s_mgr.running = false;
        s_mgr.state = MQTT_MANAGER_STATE_DISABLED;
        esp_mqtt_client_destroy(s_mgr.client);
        s_mgr.client = NULL;
        return err;
    }
    ESP_LOGI(TAG, "Started");
    return ESP_OK;
}

esp_err_t mqtt_manager_stop(void)
{
    if (!s_mgr.running) {
        return ESP_OK;
    }
    s_mgr.running = false;
    if (s_mgr.reconnect_timer) {
        esp_timer_stop(s_mgr.reconnect_timer);
    }
    if (s_mgr.client) {
        esp_mqtt_client_stop(s_mgr.client);
    }
    s_mgr.state = MQTT_MANAGER_STATE_DISABLED;
    s_mgr.connected_since_us = 0;
    ESP_LOGI(TAG, "Stopped");
    return ESP_OK;
}

bool mqtt_manager_is_connected(void)
{
    return s_mgr.state == MQTT_MANAGER_STATE_CONNECTED;
}

void mqtt_manager_get_status(mqtt_manager_status_t *out_status)
{
    if (!out_status) {
        return;
    }
    memset(out_status, 0, sizeof(*out_status));
    out_status->state = s_mgr.state;
    out_status->tls = s_mgr.tls;
    out_status->port = s_mgr.port;
    out_status->default_qos = s_mgr.default_qos;
    strlcpy(out_status->broker, s_mgr.broker, sizeof(out_status->broker));
    strlcpy(out_status->client_id, s_mgr.client_id, sizeof(out_status->client_id));
    strlcpy(out_status->base_topic, s_mgr.base_topic, sizeof(out_status->base_topic));
    strlcpy(out_status->device_id, s_mgr.device_id, sizeof(out_status->device_id));
    out_status->tx_count = s_mgr.tx_count;
    out_status->rx_count = s_mgr.rx_count;
    out_status->reconnect_count = s_mgr.reconnect_count;
    out_status->connected_since_us = s_mgr.connected_since_us;
    out_status->last_error = s_mgr.last_error;
}

esp_err_t mqtt_manager_publish(const char *topic, const char *data, int len,
                               int qos, bool retain, int *out_msg_id)
{
    if (!topic || !topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mgr.client || s_mgr.state != MQTT_MANAGER_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (qos < 0 || qos > 1) {
        qos = s_mgr.default_qos;
    }
    int msg_id = esp_mqtt_client_publish(s_mgr.client, topic, data,
                                         (data && len < 0) ? 0 : len,
                                         qos, retain ? 1 : 0);
    if (msg_id < 0) {
        return ESP_FAIL;
    }
    s_mgr.tx_count++;
    if (out_msg_id) {
        *out_msg_id = msg_id;
    }
    return ESP_OK;
}

esp_err_t mqtt_manager_subscribe(const char *topic, int qos)
{
    if (!topic || !topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mgr.client || s_mgr.state != MQTT_MANAGER_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_subscribe(s_mgr.client, topic, (qos == 1) ? 1 : 0) < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_manager_unsubscribe(const char *topic)
{
    if (!topic || !topic[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mgr.client || s_mgr.state != MQTT_MANAGER_STATE_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_unsubscribe(s_mgr.client, topic) < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_manager_register_message_cb(mqtt_manager_message_cb_t cb, void *user_ctx)
{
    s_mgr.message_cb = cb;
    s_mgr.message_ctx = user_ctx;
    return ESP_OK;
}
