/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mqtt.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_manager.h"

static const char *TAG = "cap_mqtt";

static bool s_configured;

static const char *cap_mqtt_state_str(mqtt_manager_state_t state)
{
    switch (state) {
    case MQTT_MANAGER_STATE_CONNECTED:    return "connected";
    case MQTT_MANAGER_STATE_CONNECTING:   return "connecting";
    case MQTT_MANAGER_STATE_DISCONNECTED: return "disconnected";
    case MQTT_MANAGER_STATE_DISABLED:     return "disabled";
    default:                              return "unknown";
    }
}

/* Emit a cJSON object into the caller-provided output buffer, then free it. */
static esp_err_t cap_mqtt_emit(cJSON *root, char *output, size_t output_size)
{
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        snprintf(output, output_size, "Error: failed to encode result");
        return ESP_ERR_NO_MEM;
    }
    strlcpy(output, text, output_size);
    cJSON_free(text);
    return ESP_OK;
}

/* Reject empty topics and MQTT wildcards; the tool addresses only suffixes
 * under this device's own topic root, so callers cannot publish elsewhere. */
static bool cap_mqtt_topic_suffix_is_valid(const char *suffix)
{
    if (!suffix || !suffix[0]) {
        return false;
    }
    for (const char *c = suffix; *c; c++) {
        if (*c == '#' || *c == '+') {
            return false;
        }
    }
    return true;
}

static esp_err_t cap_mqtt_status_execute(const char *input_json,
                                         const claw_cap_call_context_t *ctx,
                                         char *output,
                                         size_t output_size)
{
    (void)input_json;
    (void)ctx;

    mqtt_manager_status_t status;
    mqtt_manager_get_status(&status);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "Error: out of memory");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "state", cap_mqtt_state_str(status.state));
    cJSON_AddBoolToObject(root, "connected", status.state == MQTT_MANAGER_STATE_CONNECTED);
    cJSON_AddStringToObject(root, "broker", status.broker);
    cJSON_AddNumberToObject(root, "port", status.port);
    cJSON_AddBoolToObject(root, "tls", status.tls);
    cJSON_AddStringToObject(root, "device_id", status.device_id);
    cJSON_AddStringToObject(root, "client_id", status.client_id);
    cJSON_AddStringToObject(root, "base_topic", status.base_topic);
    cJSON_AddNumberToObject(root, "tx", status.tx_count);
    cJSON_AddNumberToObject(root, "rx", status.rx_count);
    cJSON_AddNumberToObject(root, "reconnects", status.reconnect_count);
    if (status.state == MQTT_MANAGER_STATE_CONNECTED && status.connected_since_us > 0) {
        int64_t uptime_s = (esp_timer_get_time() - status.connected_since_us) / 1000000;
        cJSON_AddNumberToObject(root, "uptime_s", (double)uptime_s);
    }
    return cap_mqtt_emit(root, output, output_size);
}

static esp_err_t cap_mqtt_publish_execute(const char *input_json,
                                          const claw_cap_call_context_t *ctx,
                                          char *output,
                                          size_t output_size)
{
    (void)ctx;

    if (!mqtt_manager_is_connected()) {
        snprintf(output, output_size, "Error: MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *topic = cJSON_GetObjectItem(input, "topic");
    cJSON *payload = cJSON_GetObjectItem(input, "payload");
    cJSON *qos = cJSON_GetObjectItem(input, "qos");
    cJSON *retain = cJSON_GetObjectItem(input, "retain");

    if (!cJSON_IsString(topic) || !cap_mqtt_topic_suffix_is_valid(topic->valuestring)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: missing or invalid topic (no wildcards)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsString(payload)) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: missing payload");
        return ESP_ERR_INVALID_ARG;
    }

    char full_topic[MQTT_MANAGER_TOPIC_MAX];
    esp_err_t err = mqtt_manager_build_topic(topic->valuestring, full_topic, sizeof(full_topic));
    if (err != ESP_OK) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: topic too long");
        return err;
    }

    int qos_val = cJSON_IsNumber(qos) ? qos->valueint : -1;
    bool retain_val = cJSON_IsBool(retain) ? cJSON_IsTrue(retain) : false;
    int msg_id = 0;
    err = mqtt_manager_publish(full_topic, payload->valuestring, -1, qos_val, retain_val, &msg_id);
    cJSON_Delete(input);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: publish failed (%s)", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "topic", full_topic);
        cJSON_AddNumberToObject(root, "msg_id", msg_id);
    }
    return cap_mqtt_emit(root, output, output_size);
}

static esp_err_t cap_mqtt_subscribe_execute(const char *input_json,
                                            const claw_cap_call_context_t *ctx,
                                            char *output,
                                            size_t output_size)
{
    (void)ctx;

    if (!mqtt_manager_is_connected()) {
        snprintf(output, output_size, "Error: MQTT not connected");
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *topic = cJSON_GetObjectItem(input, "topic");
    if (!cJSON_IsString(topic) || !topic->valuestring[0]) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: missing topic");
        return ESP_ERR_INVALID_ARG;
    }

    char full_topic[MQTT_MANAGER_TOPIC_MAX];
    esp_err_t err = mqtt_manager_build_topic(topic->valuestring, full_topic, sizeof(full_topic));
    if (err != ESP_OK) {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: topic too long");
        return err;
    }

    err = mqtt_manager_subscribe(full_topic, 1);
    cJSON_Delete(input);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: subscribe failed (%s)", esp_err_to_name(err));
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "topic", full_topic);
    }
    return cap_mqtt_emit(root, output, output_size);
}

static const claw_cap_descriptor_t s_mqtt_descriptors[] = {
    {
        .id = "mqtt_status",
        .name = "mqtt_status",
        .family = "system",
        .description = "Report the MQTT client connection state, broker and counters.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = cap_mqtt_status_execute,
    },
    {
        .id = "mqtt_publish",
        .name = "mqtt_publish",
        .family = "system",
        .description = "Publish a message to a topic under this device's MQTT root "
                       "(topic is the suffix after espclaw/<device_id>/).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{"
        "\"topic\":{\"type\":\"string\"},"
        "\"payload\":{\"type\":\"string\"},"
        "\"qos\":{\"type\":\"integer\",\"enum\":[0,1]},"
        "\"retain\":{\"type\":\"boolean\"}},"
        "\"required\":[\"topic\",\"payload\"]}",
        .execute = cap_mqtt_publish_execute,
    },
    {
        .id = "mqtt_subscribe",
        .name = "mqtt_subscribe",
        .family = "system",
        .description = "Subscribe to a topic under this device's MQTT root "
                       "(topic is the suffix after espclaw/<device_id>/).",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
        "{\"type\":\"object\",\"properties\":{\"topic\":{\"type\":\"string\"}},"
        "\"required\":[\"topic\"]}",
        .execute = cap_mqtt_subscribe_execute,
    },
};

static const claw_cap_group_t s_mqtt_group = {
    .group_id = "cap_mqtt",
    .descriptors = s_mqtt_descriptors,
    .descriptor_count = sizeof(s_mqtt_descriptors) / sizeof(s_mqtt_descriptors[0]),
};

esp_err_t cap_mqtt_apply_config(const cap_mqtt_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!config->enabled) {
        if (s_configured) {
            mqtt_manager_stop();
        }
        ESP_LOGI(TAG, "MQTT disabled by configuration");
        return ESP_OK;
    }

    if (!config->broker || !config->broker[0]) {
        ESP_LOGW(TAG, "MQTT enabled but no broker configured; skipping start");
        return ESP_OK;
    }

    mqtt_manager_config_t mgr_cfg = {
        .broker = config->broker,
        .port = config->port,
        .tls = config->tls,
        .username = config->username,
        .password = config->password,
        .client_id = config->client_id,
        .keepalive = config->keepalive,
        .default_qos = config->default_qos,
        .base_topic = config->base_topic,
    };

    esp_err_t err = mqtt_manager_init(&mgr_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_manager_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = mqtt_manager_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_manager_start failed: %s", esp_err_to_name(err));
        return err;
    }
    s_configured = true;
    return ESP_OK;
}

esp_err_t cap_mqtt_register_group(void)
{
    if (claw_cap_group_exists(s_mqtt_group.group_id)) {
        return ESP_OK;
    }
    return claw_cap_register_group(&s_mqtt_group);
}
