/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ir_device.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"

#define IR_DEVICE_TX_MEM_BLOCK       64
#define IR_DEVICE_RX_MEM_BLOCK       128
#define IR_DEVICE_MAX_TX_SYMBOLS     8192
#define IR_DEVICE_MAX_DURATION_TICKS 0x7FFF

static const char *TAG = "ir_device";

struct ir_device_t {
    rmt_channel_handle_t tx_channel;
    rmt_channel_handle_t rx_channel;
    rmt_encoder_handle_t tx_encoder;
    rmt_symbol_word_t *tx_buffer;
    QueueHandle_t rx_queue;
    rmt_symbol_word_t *rx_buffer;
    ir_device_config_t config;
    uint32_t applied_carrier_hz;
    bool ctrl_configured;
    bool tx_enabled;
    bool rx_enabled;
    bool rx_receiving;
};

static bool IRAM_ATTR ir_device_rx_done_cb(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *event, void *user_ctx)
{
    BaseType_t high_task_wakeup = pdFALSE;
    xQueueSendFromISR((QueueHandle_t)user_ctx, event, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static esp_err_t ir_device_set_ctrl(ir_device_handle_t handle, bool active)
{
    if (handle->config.ctrl_gpio < 0) return ESP_OK;
    if (!handle->ctrl_configured) {
        gpio_config_t io_config = {
            .pin_bit_mask = BIT64(handle->config.ctrl_gpio),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&io_config);
        if (err != ESP_OK) return err;
        handle->ctrl_configured = true;
    }
    return gpio_set_level(handle->config.ctrl_gpio, active ? handle->config.ctrl_active_level : !handle->config.ctrl_active_level);
}

static esp_err_t ir_device_release_tx(ir_device_handle_t handle)
{
    esp_err_t first_error = ESP_OK;
    if (handle->tx_enabled && handle->tx_channel) {
        esp_err_t err = rmt_disable(handle->tx_channel);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
        if (err == ESP_OK) handle->tx_enabled = false;
    }
    if (handle->tx_channel && !handle->tx_enabled) {
        esp_err_t err = rmt_del_channel(handle->tx_channel);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
        if (err == ESP_OK) handle->tx_channel = NULL;
    }
    if (handle->tx_encoder && !handle->tx_channel) {
        esp_err_t err = rmt_del_encoder(handle->tx_encoder);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
        if (err == ESP_OK) handle->tx_encoder = NULL;
    }
    if (!handle->tx_channel) {
        free(handle->tx_buffer);
        handle->tx_buffer = NULL;
    }
    handle->applied_carrier_hz = 0;
    return first_error;
}

static esp_err_t ir_device_release_rx(ir_device_handle_t handle)
{
    esp_err_t first_error = ESP_OK;
    if (handle->rx_enabled && handle->rx_channel) {
        esp_err_t err = rmt_disable(handle->rx_channel);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
        if (err == ESP_OK) {
            handle->rx_enabled = false;
            handle->rx_receiving = false;
        }
    }
    if (handle->rx_channel && !handle->rx_enabled) {
        esp_err_t err = rmt_del_channel(handle->rx_channel);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
        if (err == ESP_OK) handle->rx_channel = NULL;
    }
    if (!handle->rx_channel) {
        if (handle->rx_queue) {
            vQueueDelete(handle->rx_queue);
            handle->rx_queue = NULL;
        }
        free(handle->rx_buffer);
        handle->rx_buffer = NULL;
    }
    return first_error;
}

static void ir_device_release_unused_ctrl(ir_device_handle_t handle)
{
    if (handle->tx_channel || handle->rx_channel || !handle->ctrl_configured) return;
    if (ir_device_set_ctrl(handle, false) == ESP_OK && gpio_reset_pin(handle->config.ctrl_gpio) == ESP_OK) handle->ctrl_configured = false;
}

static esp_err_t ir_device_ensure_tx(ir_device_handle_t handle, uint32_t carrier_hz)
{
    esp_err_t err;
    if (!handle->tx_channel) {
        rmt_tx_channel_config_t channel_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = handle->config.tx_resolution_hz,
            .mem_block_symbols = IR_DEVICE_TX_MEM_BLOCK,
            .trans_queue_depth = 1,
            .gpio_num = handle->config.tx_gpio,
            .flags.invert_out = handle->config.tx_invert,
        };
        err = rmt_new_tx_channel(&channel_config, &handle->tx_channel);
        if (err != ESP_OK) goto fail;
        rmt_copy_encoder_config_t encoder_config = {};
        err = rmt_new_copy_encoder(&encoder_config, &handle->tx_encoder);
        if (err != ESP_OK) goto fail;
        err = rmt_enable(handle->tx_channel);
        if (err != ESP_OK) goto fail;
        handle->tx_enabled = true;
        err = ir_device_set_ctrl(handle, true);
        if (err != ESP_OK) goto fail;
    }
    if (!handle->tx_enabled) return ESP_ERR_INVALID_STATE;
    if (handle->applied_carrier_hz != carrier_hz) {
        rmt_carrier_config_t carrier_config = {
            .frequency_hz = carrier_hz,
            .duty_cycle = handle->config.carrier_duty,
        };
        err = rmt_apply_carrier(handle->tx_channel, &carrier_config);
        if (err != ESP_OK) goto fail;
        handle->applied_carrier_hz = carrier_hz;
    }
    return ESP_OK;

fail:
    (void)ir_device_release_tx(handle);
    ir_device_release_unused_ctrl(handle);
    return err;
}

static esp_err_t ir_device_ensure_rx(ir_device_handle_t handle)
{
    if (handle->rx_channel) return handle->rx_enabled ? ESP_OK : ESP_ERR_INVALID_STATE;
    esp_err_t err = ir_device_set_ctrl(handle, true);
    if (err != ESP_OK) return err;
    rmt_rx_channel_config_t channel_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = handle->config.rx_resolution_hz,
        .mem_block_symbols = IR_DEVICE_RX_MEM_BLOCK,
        .gpio_num = handle->config.rx_gpio,
        .flags.invert_in = handle->config.rx_invert,
    };
    err = rmt_new_rx_channel(&channel_config, &handle->rx_channel);
    if (err != ESP_OK) goto fail;
    handle->rx_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    if (!handle->rx_queue) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    rmt_rx_event_callbacks_t callbacks = { .on_recv_done = ir_device_rx_done_cb };
    err = rmt_rx_register_event_callbacks(handle->rx_channel, &callbacks, handle->rx_queue);
    if (err != ESP_OK) goto fail;
    handle->rx_buffer = calloc((size_t)handle->config.rx_max_symbols, sizeof(*handle->rx_buffer));
    if (!handle->rx_buffer) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    err = rmt_enable(handle->rx_channel);
    if (err != ESP_OK) goto fail;
    handle->rx_enabled = true;
    return ESP_OK;

fail:
    (void)ir_device_release_rx(handle);
    ir_device_release_unused_ctrl(handle);
    return err;
}

static esp_err_t ir_device_us_to_ticks(uint32_t duration_us, uint32_t resolution_hz, uint32_t *ticks)
{
    uint64_t value = ((uint64_t)duration_us * resolution_hz + 500000) / 1000000;
    if (value == 0 || value > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    *ticks = (uint32_t)value;
    return ESP_OK;
}

static uint64_t ir_device_ticks_to_us(uint64_t ticks, uint32_t resolution_hz)
{
    uint64_t duration_us = (ticks * 1000000 + resolution_hz / 2) / resolution_hz;
    return duration_us == 0 ? 1 : duration_us;
}

static esp_err_t ir_device_count_parts(const ir_frame_t *frame, uint32_t resolution_hz, size_t *part_count)
{
    *part_count = 0;
    for (size_t i = 0; i < frame->count; i++) {
        uint32_t ticks;
        esp_err_t err = ir_device_us_to_ticks(frame->pulses_us[i], resolution_hz, &ticks);
        if (err != ESP_OK) return err;
        *part_count += (ticks - 1) / IR_DEVICE_MAX_DURATION_TICKS + 1;
        if ((*part_count + 1) / 2 > IR_DEVICE_MAX_TX_SYMBOLS) return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void ir_device_append_ticks(rmt_symbol_word_t *symbols, size_t *part_index, int level, uint32_t ticks)
{
    while (ticks > 0) {
        uint32_t duration = ticks > IR_DEVICE_MAX_DURATION_TICKS ? IR_DEVICE_MAX_DURATION_TICKS : ticks;
        rmt_symbol_word_t *symbol = &symbols[*part_index / 2];
        if ((*part_index & 1) == 0) {
            symbol->level0 = (uint32_t)level;
            symbol->duration0 = duration;
        } else {
            symbol->level1 = (uint32_t)level;
            symbol->duration1 = duration;
        }
        (*part_index)++;
        ticks -= duration;
    }
}

static esp_err_t ir_device_encode_frame(const ir_frame_t *frame, uint32_t resolution_hz, rmt_symbol_word_t *symbols)
{
    size_t part_index = 0;
    int level = 1;
    for (size_t i = 0; i < frame->count; i++) {
        uint32_t ticks;
        esp_err_t err = ir_device_us_to_ticks(frame->pulses_us[i], resolution_hz, &ticks);
        if (err != ESP_OK) return err;
        ir_device_append_ticks(symbols, &part_index, level, ticks);
        level = !level;
    }
    return ESP_OK;
}

static esp_err_t ir_device_decode_frame(ir_device_handle_t handle, const rmt_rx_done_event_data_t *event, ir_device_receive_result_t *result)
{
    if (!event->received_symbols || event->num_symbols == 0) {
        result->status = IR_DEVICE_RECEIVE_EMPTY_FRAME;
        return ESP_OK;
    }
    if (event->num_symbols >= (size_t)handle->config.rx_max_symbols) {
        ESP_LOGW(TAG, "IR frame reached receive capacity: %u symbols", (unsigned)event->num_symbols);
        result->status = IR_DEVICE_RECEIVE_FRAME_TOO_LONG;
        return ESP_OK;
    }
    ir_frame_t *frame = ir_frame_create(event->num_symbols * 2);
    if (!frame) return ESP_ERR_NO_MEM;
    int current_level = -1;
    uint64_t ticks = 0;
    /* Merge split RMT durations before rounding. */
    for (size_t i = 0; i <= event->num_symbols; i++) {
        for (int half = 0; half < 2; half++) {
            bool end = i == event->num_symbols;
            const rmt_symbol_word_t *symbol = end ? NULL : &event->received_symbols[i];
            uint32_t duration = end ? 0 : (half ? symbol->duration1 : symbol->duration0);
            if (!duration && !end) continue;
            int level = end ? -1 : (half ? symbol->level1 : symbol->level0);
            if (level != current_level && current_level >= 0) {
                uint64_t us = ir_device_ticks_to_us(ticks, handle->config.rx_resolution_hz);
                if (us > IR_FRAME_MAX_PULSE_US) {
                    ir_frame_delete(frame);
                    result->status = IR_DEVICE_RECEIVE_FRAME_TOO_LONG;
                    return ESP_OK;
                }
                (void)ir_frame_push_level(frame, current_level, (uint32_t)us);
                ticks = 0;
            }
            current_level = level;
            ticks += duration;
            if (end) break;
        }
    }
    ir_frame_normalize(frame, IR_FRAME_GLITCH_US);
    if (!frame->count) {
        ir_frame_delete(frame);
        result->status = IR_DEVICE_RECEIVE_EMPTY_FRAME;
        return ESP_OK;
    }
    result->status = IR_DEVICE_RECEIVE_FRAME;
    result->frame = frame;
    return ESP_OK;
}

esp_err_t ir_device_create(const ir_device_config_t *config, ir_device_handle_t *ret_handle)
{
    if (!config || !ret_handle) return ESP_ERR_INVALID_ARG;
    *ret_handle = NULL;
    if (config->tx_gpio < 0 && config->rx_gpio < 0) return ESP_ERR_INVALID_ARG;
    if ((config->tx_gpio >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(config->tx_gpio)) ||
        (config->rx_gpio >= 0 && !GPIO_IS_VALID_GPIO(config->rx_gpio)) ||
        (config->ctrl_gpio >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(config->ctrl_gpio))) return ESP_ERR_INVALID_ARG;
    if ((config->tx_gpio >= 0 && config->tx_gpio == config->rx_gpio) ||
        (config->ctrl_gpio >= 0 && (config->ctrl_gpio == config->tx_gpio || config->ctrl_gpio == config->rx_gpio))) return ESP_ERR_INVALID_ARG;
    if ((config->ctrl_active_level != 0 && config->ctrl_active_level != 1) ||
        config->carrier_hz < IR_FRAME_MIN_CARRIER_HZ || config->carrier_hz > IR_FRAME_MAX_CARRIER_HZ ||
        !(config->carrier_duty > 0.0f && config->carrier_duty < 1.0f) || config->tx_resolution_hz == 0 ||
        config->rx_resolution_hz == 0 || config->rx_max_symbols < 2 || config->rx_max_symbols > 2048 ||
        config->signal_range_min_ns == 0 || config->signal_range_min_ns >= config->signal_range_max_ns) return ESP_ERR_INVALID_ARG;
    ir_device_handle_t handle = calloc(1, sizeof(*handle));
    if (!handle) return ESP_ERR_NO_MEM;
    handle->config = *config;
    *ret_handle = handle;
    return ESP_OK;
}

esp_err_t ir_device_delete(ir_device_handle_t handle)
{
    if (!handle) return ESP_OK;
    esp_err_t first_error = ir_device_release_tx(handle);
    esp_err_t err = ir_device_release_rx(handle);
    if (first_error == ESP_OK && err != ESP_OK) first_error = err;
    if (handle->ctrl_configured) {
        err = ir_device_set_ctrl(handle, false);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
        err = gpio_reset_pin(handle->config.ctrl_gpio);
        if (first_error == ESP_OK && err != ESP_OK) first_error = err;
        if (err == ESP_OK) handle->ctrl_configured = false;
    }
    if (first_error == ESP_OK) free(handle);
    return first_error;
}

esp_err_t ir_device_send(ir_device_handle_t handle, const ir_frame_t *frame, int timeout_ms)
{
    if (!handle || !frame || timeout_ms < 0 || handle->config.tx_gpio < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ir_frame_validate(frame);
    if (err != ESP_OK) return err;
    if (handle->tx_buffer) return ESP_ERR_INVALID_STATE;
    size_t part_count;
    err = ir_device_count_parts(frame, handle->config.tx_resolution_hz, &part_count);
    if (err != ESP_OK) return err;
    size_t symbol_count = (part_count + 1) / 2;
    err = ir_device_ensure_tx(handle, frame->carrier_hz ? frame->carrier_hz : handle->config.carrier_hz);
    if (err != ESP_OK) return err;
    handle->tx_buffer = calloc(symbol_count, sizeof(*handle->tx_buffer));
    if (!handle->tx_buffer) return ESP_ERR_NO_MEM;
    err = ir_device_encode_frame(frame, handle->config.tx_resolution_hz, handle->tx_buffer);
    if (err == ESP_OK) {
        rmt_transmit_config_t transmit_config = { .loop_count = 0, .flags.eot_level = 0 };
        err = rmt_transmit(handle->tx_channel, handle->tx_encoder, handle->tx_buffer, symbol_count * sizeof(*handle->tx_buffer), &transmit_config);
    }
    if (err != ESP_OK) {
        free(handle->tx_buffer);
        handle->tx_buffer = NULL;
        return err;
    }
    err = rmt_tx_wait_all_done(handle->tx_channel, timeout_ms);
    if (err != ESP_OK) {
        /* Stop hardware before releasing its payload. */
        esp_err_t cleanup_err = ir_device_release_tx(handle);
        if (cleanup_err != ESP_OK) ESP_LOGE(TAG, "TX recovery failed: %s", esp_err_to_name(cleanup_err));
        ir_device_release_unused_ctrl(handle);
        return err;
    }
    free(handle->tx_buffer);
    handle->tx_buffer = NULL;
    return ESP_OK;
}

esp_err_t ir_device_start_receive(ir_device_handle_t handle, const ir_device_receive_config_t *config)
{
    if (!handle || !config || handle->config.rx_gpio < 0 || config->signal_range_min_ns == 0 ||
        config->signal_range_min_ns >= config->signal_range_max_ns) return ESP_ERR_INVALID_ARG;
    if (handle->rx_receiving) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ir_device_ensure_rx(handle);
    if (err != ESP_OK) return err;
    (void)xQueueReset(handle->rx_queue);
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = config->signal_range_min_ns,
        .signal_range_max_ns = config->signal_range_max_ns,
    };
    err = rmt_receive(handle->rx_channel, handle->rx_buffer, (size_t)handle->config.rx_max_symbols * sizeof(*handle->rx_buffer), &receive_config);
    if (err == ESP_OK) handle->rx_receiving = true;
    return err;
}

esp_err_t ir_device_read(ir_device_handle_t handle, int timeout_ms, bool cancel_on_timeout, ir_device_receive_result_t *result)
{
    if (!handle || !result || timeout_ms < 0) return ESP_ERR_INVALID_ARG;
    if (!handle->rx_receiving) return ESP_ERR_INVALID_STATE;
    result->status = IR_DEVICE_RECEIVE_TIMEOUT;
    result->frame = NULL;
    rmt_rx_done_event_data_t event;
    TickType_t wait = timeout_ms == 0 ? 0 : pdMS_TO_TICKS((TickType_t)timeout_ms);
    if (xQueueReceive(handle->rx_queue, &event, wait) != pdPASS) {
        if (cancel_on_timeout) return ir_device_stop_receive(handle);
        return ESP_OK;
    }
    handle->rx_receiving = false;
    return ir_device_decode_frame(handle, &event, result);
}

esp_err_t ir_device_stop_receive(ir_device_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    if (!handle->rx_receiving) return ESP_OK;
    esp_err_t err = rmt_disable(handle->rx_channel);
    if (err != ESP_OK) return err;
    handle->rx_enabled = false;
    handle->rx_receiving = false;
    err = rmt_enable(handle->rx_channel);
    if (err != ESP_OK) return err;
    handle->rx_enabled = true;
    (void)xQueueReset(handle->rx_queue);
    return ESP_OK;
}

bool ir_device_is_receiving(ir_device_handle_t handle)
{
    return handle && handle->rx_receiving;
}

const ir_device_config_t *ir_device_get_config(ir_device_handle_t handle)
{
    return handle ? &handle->config : NULL;
}

void ir_device_receive_result_clear(ir_device_receive_result_t *result)
{
    if (!result) return;
    ir_frame_delete(result->frame);
    result->frame = NULL;
    result->status = IR_DEVICE_RECEIVE_EMPTY_FRAME;
}
