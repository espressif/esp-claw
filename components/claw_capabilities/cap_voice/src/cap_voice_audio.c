/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_voice_audio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "esp_heap_caps.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "cap_voice_audio";

static i2s_chan_handle_t s_rx_chan;
static i2s_chan_handle_t s_tx_chan;
static esp_codec_dev_handle_t s_spk_dev;
static esp_codec_dev_handle_t s_mic_dev;
static i2c_master_bus_handle_t s_i2c_bus;

static esp_err_t init_i2c(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CAP_VOICE_I2C_PORT,
        .sda_io_num = CAP_VOICE_I2C_SDA,
        .scl_io_num = CAP_VOICE_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

static esp_err_t init_i2s(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(
        CAP_VOICE_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan),
                        TAG, "i2s_new_channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CAP_VOICE_SAMPLE_RATE_REC),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        CAP_VOICE_BITS, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = CAP_VOICE_I2S_MCLK,
            .bclk = CAP_VOICE_I2S_BCLK,
            .ws   = CAP_VOICE_I2S_WS,
            .dout = CAP_VOICE_I2S_DOUT,
            .din  = CAP_VOICE_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg),
                        TAG, "i2s tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg),
                        TAG, "i2s rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "i2s rx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "i2s tx enable failed");
    return ESP_OK;
}

static esp_err_t init_codec(void)
{
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port       = CAP_VOICE_I2C_PORT,
        .addr       = CAP_VOICE_ES8311_ADDR,
        .bus_handle = s_i2c_bus,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!i2c_ctrl_if) {
        ESP_LOGE(TAG, "failed to create I2C control interface");
        return ESP_FAIL;
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port      = CAP_VOICE_I2S_PORT,
        .rx_handle = s_rx_chan,
        .tx_handle = s_tx_chan,
    };
    const audio_codec_data_if_t *i2s_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!i2s_if) {
        ESP_LOGE(TAG, "failed to create I2S data interface");
        return ESP_FAIL;
    }

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if     = i2c_ctrl_if,
        .gpio_if     = NULL,
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin      = -1,
        .master_mode = false,
        .use_mclk    = true,
    };
    const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
    if (!es8311_if) {
        ESP_LOGE(TAG, "failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    s_mic_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es8311_if,
        .data_if  = i2s_if,
    });
    s_spk_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_if,
        .data_if  = i2s_if,
    });

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = CAP_VOICE_SAMPLE_RATE_REC,
        .channel         = CAP_VOICE_CHANNELS,
        .bits_per_sample = CAP_VOICE_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic_dev, &fs),
                        TAG, "mic codec open failed");
    esp_codec_dev_set_in_gain(s_mic_dev, 30.0f);
    ESP_LOGI(TAG, "ES8311 codec init OK");
    return ESP_OK;
}

esp_err_t cap_voice_audio_init(void)
{
    ESP_RETURN_ON_ERROR(init_i2c(),   TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(init_i2s(),   TAG, "I2S init failed");
    ESP_RETURN_ON_ERROR(init_codec(), TAG, "codec init failed");
    return ESP_OK;
}

esp_err_t cap_voice_audio_deinit(void)
{
    if (s_mic_dev)  esp_codec_dev_close(s_mic_dev);
    if (s_spk_dev)  esp_codec_dev_close(s_spk_dev);
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
    }
    if (s_tx_chan) {
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
    }
    if (s_i2c_bus) {
        i2c_del_master_bus(s_i2c_bus);
    }
    return ESP_OK;
}

esp_err_t cap_voice_audio_read(int16_t *buf, size_t samples, size_t *out_read)
{
    int bytes = esp_codec_dev_read(s_mic_dev, buf,
                                    (int)(samples * sizeof(int16_t)));
    if (bytes < 0) {
        return ESP_FAIL;
    }
    if (out_read) {
        *out_read = (size_t)bytes / sizeof(int16_t);
    }
    return ESP_OK;
}

esp_err_t cap_voice_audio_play(const int16_t *buf, size_t samples)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = CAP_VOICE_SAMPLE_RATE_PLAY,
        .channel         = CAP_VOICE_CHANNELS,
        .bits_per_sample = CAP_VOICE_BITS,
    };

    esp_codec_dev_close(s_spk_dev);
    esp_err_t err = esp_codec_dev_open(s_spk_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spk open for playback failed: %s", esp_err_to_name(err));
        return err;
    }

    int written = esp_codec_dev_write(s_spk_dev, (void *)buf,
                                       (int)(samples * sizeof(int16_t)));

    esp_codec_dev_close(s_spk_dev);
    fs.sample_rate = CAP_VOICE_SAMPLE_RATE_REC;
    esp_codec_dev_open(s_mic_dev, &fs);
    return (written >= 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t cap_voice_audio_play_tone(int freq_hz, int duration_ms)
{
    const int sample_rate = CAP_VOICE_SAMPLE_RATE_PLAY;
    const int num_samples = sample_rate * duration_ms / 1000;
    int16_t *buf = heap_caps_malloc(num_samples * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return ESP_ERR_NO_MEM;

    for (int i = 0; i < num_samples; i++) {
        buf[i] = (int16_t)(8000.0f *
                    sinf(2.0f * M_PI * freq_hz * i / sample_rate));
    }
    esp_err_t err = cap_voice_audio_play(buf, num_samples);
    free(buf);
    return err;
}
