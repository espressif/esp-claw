/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_unitv_internal.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "cap_unitv_uart";

cap_unitv_state_t g_cap_unitv = {0};

esp_err_t cap_unitv_init(const cap_unitv_config_t *config)
{
    if (g_cap_unitv.initialized) {
        return ESP_OK;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    g_cap_unitv.cfg = *config;
    atomic_init(&g_cap_unitv.req_seq, 0);
    atomic_init(&g_cap_unitv.available, false);
    g_cap_unitv.uart_mutex = xSemaphoreCreateMutex();
    if (!g_cap_unitv.uart_mutex) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t uart_cfg = {
        .baud_rate = config->baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(config->uart_port,
                                            config->rx_buffer_bytes * 2,
                                            config->rx_buffer_bytes,
                                            0, NULL, 0),
                        TAG, "uart_driver_install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(config->uart_port, &uart_cfg),
                        TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(config->uart_port,
                                     config->tx_gpio, config->rx_gpio,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");

    g_cap_unitv.initialized = true;
    ESP_LOGI(TAG, "UART ready port=%d tx=%d rx=%d baud=%d",
             config->uart_port, config->tx_gpio, config->rx_gpio, config->baud_rate);
    return ESP_OK;
}

void cap_unitv_set_vision_config(const cap_unitv_vision_config_t *config)
{
    if (!config) {
        g_cap_unitv.vision_configured = false;
        return;
    }

    strlcpy(g_cap_unitv.vision_api_key, config->api_key ? config->api_key : "",
            sizeof(g_cap_unitv.vision_api_key));
    strlcpy(g_cap_unitv.vision_model, config->model ? config->model : "",
            sizeof(g_cap_unitv.vision_model));
    strlcpy(g_cap_unitv.vision_base_url, config->base_url ? config->base_url : "",
            sizeof(g_cap_unitv.vision_base_url));
    strlcpy(g_cap_unitv.vision_backend_type,
            config->backend_type && config->backend_type[0] ? config->backend_type : "openai_compatible",
            sizeof(g_cap_unitv.vision_backend_type));
    strlcpy(g_cap_unitv.vision_auth_type,
            config->auth_type && config->auth_type[0] ? config->auth_type : "bearer",
            sizeof(g_cap_unitv.vision_auth_type));

    g_cap_unitv.vision_cfg = *config;
    g_cap_unitv.vision_cfg.api_key = g_cap_unitv.vision_api_key;
    g_cap_unitv.vision_cfg.model = g_cap_unitv.vision_model;
    g_cap_unitv.vision_cfg.base_url = g_cap_unitv.vision_base_url;
    g_cap_unitv.vision_cfg.backend_type = g_cap_unitv.vision_backend_type;
    g_cap_unitv.vision_cfg.auth_type = g_cap_unitv.vision_auth_type;
    if (g_cap_unitv.vision_cfg.timeout_ms == 0) {
        g_cap_unitv.vision_cfg.timeout_ms = 30000;
    }
    if (g_cap_unitv.vision_cfg.max_response_tokens == 0) {
        g_cap_unitv.vision_cfg.max_response_tokens = 256;
    }
    g_cap_unitv.vision_configured = g_cap_unitv.vision_api_key[0] && g_cap_unitv.vision_model[0];
    ESP_LOGI(TAG, "vision model=%s configured=%d",
             g_cap_unitv.vision_model, (int)g_cap_unitv.vision_configured);
}

static esp_err_t read_until_newline(char *buf, size_t buf_size, TickType_t deadline)
{
    int pos = 0;

    while (pos < (int)buf_size - 1) {
        TickType_t now = xTaskGetTickCount();
        uint8_t b = 0;
        if ((int32_t)(deadline - now) <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        int rd = uart_read_bytes(g_cap_unitv.cfg.uart_port, &b, 1, deadline - now);
        if (rd <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (b == '\n') {
            break;
        }
        if (b >= 0x20) {
            buf[pos++] = (char)b;
        }
    }

    buf[pos] = '\0';
    return pos > 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t cap_unitv_uart_cmd(const char *cmd, const char *args_json,
                             char *resp, size_t resp_size, int timeout_ms)
{
    if (!g_cap_unitv.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!cmd || !resp || resp_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t rid = (uint32_t)atomic_fetch_add(&g_cap_unitv.req_seq, 1) + 1;
    char req[256] = {0};
    int n = snprintf(req, sizeof(req),
                     "{\"cmd\":\"%s\",\"req_id\":\"%" PRIu32 "\",\"args\":%s}\n",
                     cmd, rid, args_json ? args_json : "{}");
    if (n <= 0 || n >= (int)sizeof(req)) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(g_cap_unitv.uart_mutex, portMAX_DELAY);
    uart_flush_input(g_cap_unitv.cfg.uart_port);
    int sent = uart_write_bytes(g_cap_unitv.cfg.uart_port, req, n);
    if (sent != n) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_FAIL;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    esp_err_t err = read_until_newline(resp, resp_size, deadline);
    xSemaphoreGive(g_cap_unitv.uart_mutex);

    if (err == ESP_OK) {
        atomic_store(&g_cap_unitv.available, true);
    }
    return err;
}

esp_err_t cap_unitv_uart_capture_jpeg(int quality, uint8_t **jpeg_out, size_t *jpeg_size_out)
{
    if (!g_cap_unitv.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!jpeg_out || !jpeg_size_out) {
        return ESP_ERR_INVALID_ARG;
    }

    *jpeg_out = NULL;
    *jpeg_size_out = 0;

    uint32_t rid = (uint32_t)atomic_fetch_add(&g_cap_unitv.req_seq, 1) + 1;
    char req[128] = {0};
    int n = snprintf(req, sizeof(req),
                     "{\"cmd\":\"CAPTURE\",\"req_id\":\"%" PRIu32 "\",\"args\":{\"quality\":%d}}\n",
                     rid, quality);
    if (n <= 0 || n >= (int)sizeof(req)) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(g_cap_unitv.uart_mutex, portMAX_DELAY);
    uart_flush_input(g_cap_unitv.cfg.uart_port);
    int sent = uart_write_bytes(g_cap_unitv.cfg.uart_port, req, n);
    if (sent != n) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_FAIL;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(g_cap_unitv.cfg.capture_timeout_ms);
    char hdr[256] = {0};
    esp_err_t err = read_until_newline(hdr, sizeof(hdr), deadline);
    if (err != ESP_OK) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return err;
    }

    cJSON *root = cJSON_Parse(hdr);
    if (!root) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    cJSON *result = cJSON_GetObjectItem(root, "result");
    cJSON *size_field = result ? cJSON_GetObjectItem(result, "size") : NULL;
    if (!cJSON_IsTrue(ok) || !cJSON_IsNumber(size_field)) {
        cJSON_Delete(root);
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int jpeg_size = size_field->valueint;
    cJSON_Delete(root);
    if (jpeg_size <= 0 || jpeg_size > g_cap_unitv.cfg.max_jpeg_bytes) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)jpeg_size);
    if (!buf) {
        xSemaphoreGive(g_cap_unitv.uart_mutex);
        return ESP_ERR_NO_MEM;
    }

    int total = 0;
    while (total < jpeg_size) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            free(buf);
            xSemaphoreGive(g_cap_unitv.uart_mutex);
            return ESP_ERR_TIMEOUT;
        }
        int want = jpeg_size - total;
        if (want > 2048) {
            want = 2048;
        }
        int rd = uart_read_bytes(g_cap_unitv.cfg.uart_port, buf + total, want, deadline - now);
        if (rd <= 0) {
            free(buf);
            xSemaphoreGive(g_cap_unitv.uart_mutex);
            return ESP_ERR_TIMEOUT;
        }
        total += rd;
    }

    xSemaphoreGive(g_cap_unitv.uart_mutex);
    atomic_store(&g_cap_unitv.available, true);
    *jpeg_out = buf;
    *jpeg_size_out = (size_t)jpeg_size;
    return ESP_OK;
}
