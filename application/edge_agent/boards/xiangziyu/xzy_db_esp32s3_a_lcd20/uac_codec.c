/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "uac_codec.h"

#define UAC_CODEC_BUFFER_SIZE     16000
#define UAC_CODEC_THRESHOLD_SIZE  4000
#define UAC_CODEC_DEFAULT_RATE    16000

static const char *TAG = "uac_codec";

typedef struct {
    audio_codec_if_t base;
    uac_codec_config_t config;
    uac_host_device_handle_t dev_handle;
    esp_codec_dev_sample_info_t fs;
    esp_codec_dev_type_t dev_type;
    bool is_open;
    bool is_data_enabled;
    bool is_dev_enabled;
    bool is_enabled;
    bool stream_started;
    bool disconnected;
    int size_per_second;
} uac_codec_t;

typedef struct {
    audio_codec_data_if_t base;
    uac_codec_t *codec;
} uac_codec_data_t;

static uint8_t clamp_percent(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return (uint8_t)value;
}

static uint8_t out_db_to_percent(float db)
{
    if (db <= -96.0f) {
        return 0;
    }
    if (db >= 0.0f) {
        return 100;
    }

    /* Match esp_codec_dev default output curve: [1, 100] -> [-49.5 dB, 0 dB]. */
    if (db <= -49.5f) {
        return 1;
    }
    return clamp_percent((int)lroundf(100.0f + db * 2.0f));
}

static uint8_t mic_volume_db_to_percent(float db)
{
    /* Lua input volume uses 0..100 percent and esp_codec_dev receives the mapped 0..30 dB range. */
    if (db <= 0.0f) {
        return 0;
    }
    if (db >= 30.0f) {
        return 100;
    }
    return clamp_percent((int)lroundf(db * 100.0f / 30.0f));
}

static uint32_t select_uac_sample_rate(const uac_host_dev_alt_param_t *alt_param, uint32_t preferred_rate)
{
    if (preferred_rate == 0) {
        preferred_rate = UAC_CODEC_DEFAULT_RATE;
    }
    if (alt_param->sample_freq_type == 0) {
        if (preferred_rate >= alt_param->sample_freq_lower && preferred_rate <= alt_param->sample_freq_upper) {
            return preferred_rate;
        }
        return alt_param->sample_freq_lower;
    }
    for (int i = 0; i < alt_param->sample_freq_type && i < UAC_FREQ_NUM_MAX; i++) {
        if (alt_param->sample_freq[i] == preferred_rate) {
            return preferred_rate;
        }
    }
    return alt_param->sample_freq[0];
}

static void uac_codec_device_callback(uac_host_device_handle_t uac_device_handle,
                                      const uac_host_device_event_t event, void *arg)
{
    uac_codec_t *codec = (uac_codec_t *)arg;

    if (event != UAC_HOST_DRIVER_EVENT_DISCONNECTED || codec == NULL) {
        return;
    }

    ESP_LOGI(TAG, "UAC %s disconnected", codec->config.is_input ? "microphone" : "speaker");
    codec->disconnected = true;
    codec->is_enabled = false;
    codec->stream_started = false;
    codec->size_per_second = 0;
    if (codec->dev_handle == uac_device_handle) {
        codec->dev_handle = NULL;
    }
    (void)uac_host_device_close(uac_device_handle);
}

static esp_err_t uac_codec_open_device(uac_codec_t *codec)
{
    if (codec->dev_handle != NULL) {
        return ESP_OK;
    }

    const uac_host_device_config_t dev_config = {
        .addr = codec->config.addr,
        .iface_num = codec->config.iface_num,
        .buffer_size = UAC_CODEC_BUFFER_SIZE,
        .buffer_threshold = UAC_CODEC_THRESHOLD_SIZE,
        .callback = uac_codec_device_callback,
        .callback_arg = codec,
    };

    ESP_RETURN_ON_ERROR(uac_host_device_open(&dev_config, &codec->dev_handle), TAG, "failed to open UAC device");
    return ESP_OK;
}

static esp_err_t uac_codec_start_device(uac_codec_t *codec)
{
    uac_host_dev_alt_param_t alt_param = {0};
    esp_err_t ret;

    if (codec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = uac_codec_open_device(codec);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uac_host_get_device_alt_param(codec->dev_handle, 1, &alt_param);
    if (ret != ESP_OK) {
        (void)uac_host_device_close(codec->dev_handle);
        codec->dev_handle = NULL;
        return ret;
    }

    if (codec->fs.sample_rate == 0) {
        codec->fs.sample_rate = codec->config.preferred_sample_rate;
    }
    if (codec->fs.sample_rate == 0) {
        codec->fs.sample_rate = UAC_CODEC_DEFAULT_RATE;
    }
    if (codec->fs.bits_per_sample == 0) {
        codec->fs.bits_per_sample = alt_param.bit_resolution;
    }
    if (codec->fs.channel == 0) {
        codec->fs.channel = alt_param.channels;
    }

    const uac_host_stream_config_t stream_config = {
        .channels = codec->fs.channel,
        .bit_resolution = codec->fs.bits_per_sample,
        .sample_freq = select_uac_sample_rate(&alt_param, codec->fs.sample_rate),
    };
    if (stream_config.sample_freq != codec->fs.sample_rate) {
        ESP_LOGI(TAG, "UAC device does not support preferred sample rate %" PRIu32 " Hz, using %" PRIu32 " Hz",
                 codec->fs.sample_rate, stream_config.sample_freq);
    }

    ret = uac_host_device_start(codec->dev_handle, &stream_config);
    if (ret != ESP_OK) {
        (void)uac_host_device_close(codec->dev_handle);
        codec->dev_handle = NULL;
        return ret;
    }

    codec->fs.sample_rate = stream_config.sample_freq;
    codec->fs.bits_per_sample = stream_config.bit_resolution;
    codec->fs.channel = stream_config.channels;
    codec->size_per_second = codec->fs.sample_rate * codec->fs.bits_per_sample / 8 * codec->fs.channel;
    codec->disconnected = false;
    codec->stream_started = true;

    if (codec->config.is_input) {
        (void)uac_host_device_set_mute(codec->dev_handle, false);
        ESP_LOGI(TAG, "UAC microphone started: %" PRIu32 " Hz, %u bits, %u channels",
                 stream_config.sample_freq, stream_config.bit_resolution, stream_config.channels);
    } else {
        (void)uac_host_device_set_volume(codec->dev_handle, 50);
        (void)uac_host_device_set_mute(codec->dev_handle, false);
        ESP_LOGI(TAG, "UAC speaker started: %" PRIu32 " Hz, %u bits, %u channels",
                 stream_config.sample_freq, stream_config.bit_resolution, stream_config.channels);
    }

    return ESP_OK;
}

static void uac_codec_stop_device(uac_codec_t *codec)
{
    if (codec == NULL || codec->dev_handle == NULL || !codec->stream_started) {
        return;
    }

    (void)uac_host_device_stop(codec->dev_handle);
    codec->stream_started = false;
    codec->size_per_second = 0;
}

static void uac_codec_close_device(uac_codec_t *codec)
{
    if (codec == NULL || codec->dev_handle == NULL) {
        return;
    }

    uac_codec_stop_device(codec);
    (void)uac_host_device_close(codec->dev_handle);
    codec->dev_handle = NULL;
}

static int uac_codec_try_enable(uac_codec_t *codec, bool enable)
{
    if (codec == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    if (!enable) {
        uac_codec_stop_device(codec);
        codec->is_enabled = false;
        return ESP_CODEC_DEV_OK;
    }

    if (codec->is_enabled) {
        return ESP_CODEC_DEV_OK;
    }
    if (!codec->is_dev_enabled || !codec->is_data_enabled) {
        return ESP_CODEC_DEV_OK;
    }
    if (codec->disconnected) {
        return ESP_CODEC_DEV_NOT_FOUND;
    }

    esp_err_t ret = uac_codec_start_device(codec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to start UAC %s: %s",
                 codec->config.is_input ? "microphone" : "speaker", esp_err_to_name(ret));
        return ret;
    }

    codec->is_enabled = true;
    return ESP_CODEC_DEV_OK;
}

static int uac_codec_open(const audio_codec_if_t *h, void *cfg, int cfg_size)
{
    (void)cfg;
    (void)cfg_size;
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    codec->is_open = true;
    return ESP_CODEC_DEV_OK;
}

static bool uac_codec_is_open(const audio_codec_if_t *h)
{
    const uac_codec_t *codec = (const uac_codec_t *)h;
    return codec != NULL && codec->is_open;
}

static int uac_codec_enable(const audio_codec_if_t *h, bool enable)
{
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    codec->is_dev_enabled = enable;
    return uac_codec_try_enable(codec, enable);
}

static int uac_codec_set_fs(const audio_codec_if_t *h, esp_codec_dev_sample_info_t *fs)
{
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL || fs == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    codec->fs = *fs;
    return ESP_CODEC_DEV_OK;
}

static int uac_codec_mute(const audio_codec_if_t *h, bool mute)
{
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL || codec->dev_handle == NULL || codec->config.is_input) {
        return ESP_CODEC_DEV_NOT_SUPPORT;
    }
    return uac_host_device_set_mute(codec->dev_handle, mute);
}

static int uac_codec_set_vol(const audio_codec_if_t *h, float db)
{
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL || codec->dev_handle == NULL || codec->config.is_input) {
        return ESP_CODEC_DEV_NOT_SUPPORT;
    }
    return uac_host_device_set_volume(codec->dev_handle, out_db_to_percent(db));
}

static int uac_codec_set_mic_gain(const audio_codec_if_t *h, float db)
{
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL || codec->dev_handle == NULL || !codec->config.is_input) {
        return ESP_CODEC_DEV_NOT_SUPPORT;
    }
    return uac_host_device_set_volume(codec->dev_handle, mic_volume_db_to_percent(db));
}

static int uac_codec_set_mic_channel_gain(const audio_codec_if_t *h, uint16_t channel_mask, float db)
{
    (void)h;
    (void)channel_mask;
    (void)db;
    return ESP_CODEC_DEV_NOT_SUPPORT;
}

static int uac_codec_mute_mic(const audio_codec_if_t *h, bool mute)
{
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL || codec->dev_handle == NULL || !codec->config.is_input) {
        return ESP_CODEC_DEV_NOT_SUPPORT;
    }
    return uac_host_device_set_mute(codec->dev_handle, mute);
}

static int uac_codec_set_reg(const audio_codec_if_t *h, int reg, int value)
{
    (void)h;
    (void)reg;
    (void)value;
    return ESP_CODEC_DEV_NOT_SUPPORT;
}

static int uac_codec_get_reg(const audio_codec_if_t *h, int reg, int *value)
{
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL || value == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    /* Expose negotiated UAC stream format through virtual read-only registers. */
    switch (reg) {
    case UAC_CODEC_VREG_FORMAT_MAGIC:
        *value = UAC_CODEC_VREG_FORMAT_MAGIC_VALUE;
        return ESP_CODEC_DEV_OK;
    case UAC_CODEC_VREG_FORMAT_SAMPLE_RATE:
        *value = (int)codec->fs.sample_rate;
        return ESP_CODEC_DEV_OK;
    case UAC_CODEC_VREG_FORMAT_CHANNELS:
        *value = (int)codec->fs.channel;
        return ESP_CODEC_DEV_OK;
    case UAC_CODEC_VREG_FORMAT_BITS:
        *value = (int)codec->fs.bits_per_sample;
        return ESP_CODEC_DEV_OK;
    default:
        return ESP_CODEC_DEV_NOT_SUPPORT;
    }
}

static void uac_codec_dump_reg(const audio_codec_if_t *h)
{
    (void)h;
}

static int uac_codec_close(const audio_codec_if_t *h)
{
    uac_codec_t *codec = (uac_codec_t *)h;

    if (codec == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    codec->is_open = false;
    codec->is_dev_enabled = false;
    return uac_codec_try_enable(codec, false);
}

static int uac_data_open(const audio_codec_data_if_t *h, void *data_cfg, int cfg_size)
{
    (void)data_cfg;
    (void)cfg_size;
    uac_codec_data_t *data_if = (uac_codec_data_t *)h;

    if (data_if == NULL || data_if->codec == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    return ESP_CODEC_DEV_OK;
}

static bool uac_data_is_open(const audio_codec_data_if_t *h)
{
    const uac_codec_data_t *data_if = (const uac_codec_data_t *)h;
    return data_if != NULL && data_if->codec != NULL;
}

static int uac_data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t dev_type, bool enable)
{
    uac_codec_data_t *data_if = (uac_codec_data_t *)h;

    if (data_if == NULL || data_if->codec == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    data_if->codec->dev_type = dev_type;
    data_if->codec->is_data_enabled = enable;
    return uac_codec_try_enable(data_if->codec, enable);
}

static int uac_data_set_fmt(const audio_codec_data_if_t *h, esp_codec_dev_type_t dev_type,
                            esp_codec_dev_sample_info_t *fs)
{
    uac_codec_data_t *data_if = (uac_codec_data_t *)h;

    (void)dev_type;
    if (data_if == NULL || data_if->codec == NULL || fs == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    data_if->codec->fs = *fs;
    return ESP_CODEC_DEV_OK;
}

static int uac_data_read(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    uac_codec_data_t *data_if = (uac_codec_data_t *)h;

    if (data_if == NULL || data_if->codec == NULL || data == NULL || size <= 0) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    uac_codec_t *codec = data_if->codec;
    if (!codec->config.is_input || codec->dev_handle == NULL || !codec->is_enabled || codec->disconnected) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }

    int timeout_ms = 200;
    if (codec->size_per_second > 0) {
        timeout_ms = (int)(((int64_t)size * 1000 * 4 + codec->size_per_second - 1) / codec->size_per_second);
        timeout_ms = (timeout_ms < 200) ? 200 : timeout_ms;
    }

    int bytes_total = size;
    while (size > 0 && !codec->disconnected) {
        uint32_t bytes_read = 0;
        esp_err_t ret = uac_host_device_read(codec->dev_handle, data, size, &bytes_read, pdMS_TO_TICKS(timeout_ms));
        if (ret != ESP_OK || bytes_read == 0) {
            ESP_LOGE(TAG, "UAC microphone read failed: err=%s req=%d remaining=%d read=%" PRIu32
                     " timeout=%dms bps=%d fmt=%" PRIu32 "/%u/%u enabled=%d disconnected=%d",
                     esp_err_to_name(ret), bytes_total, size, bytes_read, timeout_ms, codec->size_per_second, codec->fs.sample_rate,
                     codec->fs.channel, codec->fs.bits_per_sample, codec->is_enabled, codec->disconnected);
            return ESP_CODEC_DEV_READ_FAIL;
        }
        data += bytes_read;
        size -= bytes_read;
    }

    return codec->disconnected ? ESP_CODEC_DEV_NOT_FOUND : ESP_CODEC_DEV_OK;
}

static int uac_data_write(const audio_codec_data_if_t *h, uint8_t *data, int size)
{
    uac_codec_data_t *data_if = (uac_codec_data_t *)h;

    if (data_if == NULL || data_if->codec == NULL || data == NULL || size <= 0) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }

    uac_codec_t *codec = data_if->codec;
    if (codec->config.is_input || codec->dev_handle == NULL || !codec->is_enabled || codec->disconnected) {
        return ESP_CODEC_DEV_WRONG_STATE;
    }

    int timeout_ms = 20;
    if (codec->size_per_second > 0) {
        timeout_ms = (int)(((int64_t)size * 1000 * 8 + codec->size_per_second - 1) / codec->size_per_second);
        if (timeout_ms < 20) {
            timeout_ms = 20;
        }
    }

    esp_err_t ret = uac_host_device_write(codec->dev_handle, data, size, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UAC speaker write failed: err=%s req=%d timeout=%dms bps=%d fmt=%" PRIu32 "/%u/%u enabled=%d disconnected=%d",
                 esp_err_to_name(ret), size, timeout_ms, codec->size_per_second, codec->fs.sample_rate, codec->fs.channel, codec->fs.bits_per_sample,
                 codec->is_enabled, codec->disconnected);
        return ESP_CODEC_DEV_WRITE_FAIL;
    }
    return ESP_CODEC_DEV_OK;
}

static int uac_data_close(const audio_codec_data_if_t *h)
{
    uac_codec_data_t *data_if = (uac_codec_data_t *)h;

    if (data_if == NULL || data_if->codec == NULL) {
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    data_if->codec->is_data_enabled = false;
    return uac_codec_try_enable(data_if->codec, false);
}

static const audio_codec_if_t *uac_codec_new_codec_if(const uac_codec_config_t *config)
{
    uac_codec_t *codec = calloc(1, sizeof(*codec));
    if (codec == NULL) {
        return NULL;
    }

    codec->base.open = uac_codec_open;
    codec->base.is_open = uac_codec_is_open;
    codec->base.enable = uac_codec_enable;
    codec->base.set_fs = uac_codec_set_fs;
    codec->base.mute = uac_codec_mute;
    codec->base.set_vol = uac_codec_set_vol;
    codec->base.set_mic_gain = uac_codec_set_mic_gain;
    codec->base.set_mic_channel_gain = uac_codec_set_mic_channel_gain;
    codec->base.mute_mic = uac_codec_mute_mic;
    codec->base.set_reg = uac_codec_set_reg;
    codec->base.get_reg = uac_codec_get_reg;
    codec->base.dump_reg = uac_codec_dump_reg;
    codec->base.close = uac_codec_close;
    codec->config = *config;
    codec->dev_type = config->is_input ? ESP_CODEC_DEV_TYPE_IN : ESP_CODEC_DEV_TYPE_OUT;
    codec->base.open(&codec->base, NULL, 0);

    return &codec->base;
}

static const audio_codec_data_if_t *uac_codec_new_data_if(const audio_codec_if_t *codec_if)
{
    uac_codec_data_t *data_if = calloc(1, sizeof(*data_if));
    if (data_if == NULL) {
        return NULL;
    }

    data_if->base.open = uac_data_open;
    data_if->base.is_open = uac_data_is_open;
    data_if->base.enable = uac_data_enable;
    data_if->base.set_fmt = uac_data_set_fmt;
    data_if->base.read = uac_data_read;
    data_if->base.write = uac_data_write;
    data_if->base.close = uac_data_close;
    data_if->codec = (uac_codec_t *)codec_if;

    return &data_if->base;
}

dev_audio_codec_handles_t *uac_codec_new_handle(const uac_codec_config_t *config)
{
    if (config == NULL) {
        return NULL;
    }

    dev_audio_codec_handles_t *codec_handles = calloc(1, sizeof(*codec_handles));
    if (codec_handles == NULL) {
        return NULL;
    }
    codec_handles->tx_aux_out_io = -1;

    codec_handles->codec_if = uac_codec_new_codec_if(config);
    if (codec_handles->codec_if == NULL) {
        free(codec_handles);
        return NULL;
    }

    codec_handles->data_if = uac_codec_new_data_if(codec_handles->codec_if);
    if (codec_handles->data_if == NULL) {
        audio_codec_delete_codec_if(codec_handles->codec_if);
        free(codec_handles);
        return NULL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_handles->codec_if,
        .data_if = codec_handles->data_if,
        .dev_type = config->is_input ? ESP_CODEC_DEV_TYPE_IN : ESP_CODEC_DEV_TYPE_OUT,
    };
    codec_handles->codec_dev = esp_codec_dev_new(&dev_cfg);
    if (codec_handles->codec_dev == NULL) {
        audio_codec_delete_data_if(codec_handles->data_if);
        audio_codec_delete_codec_if(codec_handles->codec_if);
        free(codec_handles);
        return NULL;
    }

    return codec_handles;
}

void uac_codec_delete_handle(dev_audio_codec_handles_t *codec_handles)
{
    if (codec_handles == NULL) {
        return;
    }
    uac_codec_t *codec = (uac_codec_t *)codec_handles->codec_if;
    if (codec_handles->codec_dev) {
        esp_codec_dev_delete(codec_handles->codec_dev);
        codec_handles->codec_dev = NULL;
    }
    uac_codec_close_device(codec);
    if (codec_handles->data_if) {
        audio_codec_delete_data_if(codec_handles->data_if);
        codec_handles->data_if = NULL;
    }
    if (codec_handles->codec_if) {
        audio_codec_delete_codec_if(codec_handles->codec_if);
        codec_handles->codec_if = NULL;
    }
    free(codec_handles);
}
