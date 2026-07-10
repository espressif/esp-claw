/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

bool audio_device_acquire(audio_device_t *dev)
{
    if (!dev || dev->closed || dev->active) {
        return false;
    }
    dev->active = true;
    return true;
}

void audio_device_release(audio_device_t *dev)
{
    if (dev) {
        dev->active = false;
    }
}

static void audio_device_refresh_actual_format(audio_device_t *dev)
{
    int magic = 0;
    int rate = 0;
    int channels = 0;
    int bits = 0;
    audio_format_t actual = {0};

    if (esp_codec_dev_read_reg(dev->codec_dev, AUDIO_CODEC_VREG_FORMAT_MAGIC, &magic) != ESP_CODEC_DEV_OK || magic != AUDIO_CODEC_VREG_FORMAT_MAGIC_VALUE) {
        return;
    }
    if (esp_codec_dev_read_reg(dev->codec_dev, AUDIO_CODEC_VREG_FORMAT_SAMPLE_RATE, &rate) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_read_reg(dev->codec_dev, AUDIO_CODEC_VREG_FORMAT_CHANNELS, &channels) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_read_reg(dev->codec_dev, AUDIO_CODEC_VREG_FORMAT_BITS, &bits) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Codec actual format read failed");
        return;
    }

    actual.sample_rate = (uint32_t)rate;
    actual.channels = (uint8_t)channels;
    actual.bits = (uint8_t)bits;
    if (audio_format_complete(&actual) != ESP_OK) {
        ESP_LOGE(TAG, "Codec actual format invalid: rate=%d ch=%d bits=%d", rate, channels, bits);
        return;
    }
    if (!audio_format_equal(&dev->fmt, &actual)) {
        ESP_LOGI(TAG, "Codec actual format: role=%s requested=%" PRIu32 "/%u/%u actual=%" PRIu32 "/%u/%u",
                 dev->kind == AUDIO_DEVICE_OUTPUT ? "output" : "input", dev->fmt.sample_rate, dev->fmt.channels, dev->fmt.bits,
                 actual.sample_rate, actual.channels, actual.bits);
        dev->fmt = actual;
    }
}

static float audio_input_volume_to_db(int volume)
{
    return ((float)volume * AUDIO_INPUT_GAIN_DB_MAX) / 100.0f;
}

static esp_err_t audio_device_open(audio_device_t *dev)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = dev->fmt.sample_rate,
        .channel = dev->fmt.channels,
        .bits_per_sample = dev->fmt.bits,
    };
    int ret = esp_codec_dev_open(dev->codec_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Codec open failed: role=%s rate=%" PRIu32 " ch=%u bits=%u ret=%d",
                 dev->kind == AUDIO_DEVICE_OUTPUT ? "output" : "input", dev->fmt.sample_rate, dev->fmt.channels, dev->fmt.bits, ret);
        return ESP_FAIL;
    }
    audio_device_refresh_actual_format(dev);
    if (dev->kind == AUDIO_DEVICE_OUTPUT) {
        ret = esp_codec_dev_set_out_vol(dev->codec_dev, dev->volume);
    } else {
        ret = esp_codec_dev_set_in_gain(dev->codec_dev, audio_input_volume_to_db(dev->volume));
    }
    if (ret != ESP_CODEC_DEV_OK && ret != ESP_CODEC_DEV_NOT_SUPPORT) {
        ESP_LOGE(TAG, "Codec level setup failed: role=%s ret=%d", dev->kind == AUDIO_DEVICE_OUTPUT ? "output" : "input", ret);
        esp_codec_dev_close(dev->codec_dev);
        return ESP_FAIL;
    }
    dev->closed = false;
    return ESP_OK;
}

static int lua_audio_new_device(lua_State *L, audio_device_kind_t kind)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    audio_device_t *dev = (audio_device_t *)lua_newuserdata(L, sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    dev->kind = kind;
    dev->codec_dev = (esp_codec_dev_handle_t)lua_audio_get_codec_field(L, 1);
    dev->volume = lua_audio_get_int_field(L, 1, "volume", AUDIO_DEFAULT_VOL);

    if (!dev->codec_dev) {
        return lua_audio_push_error(L, "audio device: codec is required");
    }
    dev->fmt.sample_rate = lua_audio_get_u32_field(L, 1, "sample_rate", 2, 0);
    dev->fmt.channels = lua_audio_get_u8_field(L, 1, "channels", 3, 0);
    dev->fmt.bits = lua_audio_get_u8_field(L, 1, "bits", 4, 0);
    if (audio_format_complete(&dev->fmt) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid device format: rate=%" PRIu32 " ch=%u bits=%u", dev->fmt.sample_rate, dev->fmt.channels, dev->fmt.bits);
        return lua_audio_push_error(L, "audio device: invalid format");
    }
    if (dev->volume < 0 || dev->volume > 100) {
        return lua_audio_push_error(L, kind == AUDIO_DEVICE_OUTPUT ? "audio output: volume must be 0..100" : "audio input: volume must be 0..100");
    }
    if (audio_device_open(dev) != ESP_OK) {
        return lua_audio_push_error(L, "audio device: failed to open codec");
    }

    luaL_getmetatable(L, kind == AUDIO_DEVICE_OUTPUT ? AUDIO_DEVICE_OUTPUT_META : AUDIO_DEVICE_INPUT_META);
    lua_setmetatable(L, -2);
    return 1;
}

int lua_audio_new_output(lua_State *L)
{
    return lua_audio_new_device(L, AUDIO_DEVICE_OUTPUT);
}

int lua_audio_new_input(lua_State *L)
{
    return lua_audio_new_device(L, AUDIO_DEVICE_INPUT);
}

int lua_audio_device_close(lua_State *L)
{
    audio_device_t *dev = (audio_device_t *)lua_touserdata(L, 1);
    if (!dev || dev->closed) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (dev->holders > 0 || dev->active) {
        ESP_LOGW(TAG, "Device close rejected: busy holders=%u active=%d", dev->holders, dev->active);
        return lua_audio_push_error(L, "audio device: busy");
    }
    if (esp_codec_dev_close(dev->codec_dev) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Codec close returned error");
    }
    dev->codec_dev = NULL;
    dev->closed = true;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_device_gc(lua_State *L)
{
    audio_device_t *dev = (audio_device_t *)lua_touserdata(L, 1);
    if (dev && !dev->closed && dev->holders == 0 && !dev->active) {
        esp_codec_dev_close(dev->codec_dev);
        dev->codec_dev = NULL;
        dev->closed = true;
    }
    return 0;
}

int lua_audio_device_info(lua_State *L)
{
    audio_device_t *dev = luaL_testudata(L, 1, AUDIO_DEVICE_INPUT_META);
    if (!dev) {
        dev = (audio_device_t *)luaL_checkudata(L, 1, AUDIO_DEVICE_OUTPUT_META);
    }
    lua_newtable(L);
    lua_pushstring(L, dev->kind == AUDIO_DEVICE_OUTPUT ? "output" : "input");
    lua_setfield(L, -2, "role");
    lua_pushboolean(L, !dev->closed);
    lua_setfield(L, -2, "opened");
    lua_pushinteger(L, dev->fmt.sample_rate);
    lua_setfield(L, -2, "sample_rate");
    lua_pushinteger(L, dev->fmt.channels);
    lua_setfield(L, -2, "channels");
    lua_pushinteger(L, dev->fmt.bits);
    lua_setfield(L, -2, "bits");
    lua_pushinteger(L, dev->fmt.bytes_per_frame);
    lua_setfield(L, -2, "bytes_per_frame");
    return 1;
}

int lua_audio_output_set_volume(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_OUTPUT, "set_volume");
    int vol = (int)luaL_checkinteger(L, 2);
    if (vol < 0 || vol > 100) {
        return luaL_error(L, "audio set_volume: volume must be 0..100");
    }
    if (esp_codec_dev_set_out_vol(dev->codec_dev, vol) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Set output volume failed");
        return lua_audio_push_error(L, "audio output: set volume failed");
    }
    dev->volume = vol;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_output_get_volume(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_OUTPUT, "get_volume");
    int vol = 0;
    if (esp_codec_dev_get_out_vol(dev->codec_dev, &vol) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Get output volume failed");
        return lua_audio_push_error(L, "audio output: get volume failed");
    }
    lua_pushinteger(L, vol);
    return 1;
}

int lua_audio_output_set_mute(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_OUTPUT, "set_mute");
    bool mute = lua_toboolean(L, 2);
    if (esp_codec_dev_set_out_mute(dev->codec_dev, mute) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Set output mute failed");
        return lua_audio_push_error(L, "audio output: set mute failed");
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_input_set_volume(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_INPUT, "set_volume");
    int vol = (int)luaL_checkinteger(L, 2);
    if (vol < 0 || vol > 100) {
        return luaL_error(L, "audio set_volume: volume must be 0..100");
    }
    if (esp_codec_dev_set_in_gain(dev->codec_dev, audio_input_volume_to_db(vol)) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Set input volume failed");
        return lua_audio_push_error(L, "audio input: set volume failed");
    }
    dev->volume = vol;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_input_get_volume(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_INPUT, "get_volume");
    lua_pushinteger(L, dev->volume);
    return 1;
}

int lua_audio_output_write(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_OUTPUT, "write");
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (!audio_device_acquire(dev)) {
        return lua_audio_push_error(L, "audio output: busy");
    }
    int ret = esp_codec_dev_write(dev->codec_dev, (void *)data, (int)len);
    audio_device_release(dev);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Output write failed: ret=%d", ret);
        return lua_audio_push_error(L, "audio output: write failed");
    }
    lua_pushinteger(L, len);
    return 1;
}

int lua_audio_input_read(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_INPUT, "read");
    uint32_t bytes = 0;
    uint8_t *buf = NULL;

    if (lua_istable(L, 2)) {
        bytes = lua_audio_get_u32_field(L, 2, "bytes", 0, 0);
    } else {
        bytes = (uint32_t)luaL_checkinteger(L, 2);
    }
    if (!audio_device_acquire(dev)) {
        return lua_audio_push_error(L, "audio input: busy");
    }
    buf = malloc(bytes);
    if (!buf) {
        audio_device_release(dev);
        ESP_LOGE(TAG, "Input read buffer alloc failed: %" PRIu32 " bytes", bytes);
        return lua_audio_push_error(L, "audio input: out of memory");
    }
    int ret = esp_codec_dev_read(dev->codec_dev, buf, (int)bytes);
    audio_device_release(dev);
    if (ret != ESP_CODEC_DEV_OK) {
        free(buf);
        ESP_LOGE(TAG, "Input read failed: ret=%d", ret);
        return lua_audio_push_error(L, "audio input: read failed");
    }
    lua_pushlstring(L, (const char *)buf, bytes);
    free(buf);
    return 1;
}

int lua_audio_output_play_tone(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_OUTPUT, "play_tone");
    uint32_t freq_hz = (uint32_t)luaL_checkinteger(L, 2);
    uint32_t duration_ms = (uint32_t)luaL_checkinteger(L, 3);
    uint32_t chunk_frames;
    uint32_t total_frames;
    uint32_t frames_written = 0;
    float amplitude;
    float phase = 0.0f;
    float phase_step;
    uint8_t *buf = NULL;

    if (duration_ms == 0 || freq_hz == 0) {
        return luaL_error(L, "audio play_tone: invalid frequency or duration");
    }
    if (dev->fmt.bits != 16 && dev->fmt.bits != 32) {
        return luaL_error(L, "audio play_tone: only 16-bit or 32-bit PCM output is supported");
    }
    if (freq_hz >= dev->fmt.sample_rate / 2) {
        return luaL_error(L, "audio play_tone: freq_hz must be less than half of sample_rate");
    }
    if (!audio_device_acquire(dev)) {
        return lua_audio_push_error(L, "audio output: busy");
    }

    chunk_frames = AUDIO_CHUNK_BYTES / dev->fmt.bytes_per_frame;
    if (chunk_frames == 0) {
        audio_device_release(dev);
        return lua_audio_push_error(L, "audio play_tone: invalid frame size");
    }
    total_frames = (uint32_t)(((uint64_t)dev->fmt.sample_rate * duration_ms) / 1000);
    /* Keep tone amplitude below full scale; device volume controls perceived loudness. */
    amplitude = 32767.0f * 0.55f;
    phase_step = 2.0f * (float)M_PI * (float)freq_hz / (float)dev->fmt.sample_rate;
    buf = malloc(chunk_frames * dev->fmt.bytes_per_frame);
    if (!buf) {
        audio_device_release(dev);
        ESP_LOGE(TAG, "Tone buffer alloc failed");
        return lua_audio_push_error(L, "audio play_tone: out of memory");
    }

    while (frames_written < total_frames) {
        uint32_t frames_this = total_frames - frames_written;
        if (frames_this > chunk_frames) {
            frames_this = chunk_frames;
        }
        for (uint32_t i = 0; i < frames_this; i++) {
            int16_t sample16 = (int16_t)(sinf(phase) * amplitude);
            if (dev->fmt.bits == 16) {
                int16_t *p = (int16_t *)buf + (size_t)i * dev->fmt.channels;
                for (uint8_t ch = 0; ch < dev->fmt.channels; ch++) {
                    p[ch] = sample16;
                }
            } else {
                int32_t sample32 = (int32_t)sample16 << 16;
                int32_t *p = (int32_t *)buf + (size_t)i * dev->fmt.channels;
                for (uint8_t ch = 0; ch < dev->fmt.channels; ch++) {
                    p[ch] = sample32;
                }
            }
            phase += phase_step;
            if (phase >= 2.0f * (float)M_PI) {
                phase -= 2.0f * (float)M_PI;
            }
        }
        int bytes = (int)(frames_this * dev->fmt.bytes_per_frame);
        if (esp_codec_dev_write(dev->codec_dev, buf, bytes) != ESP_CODEC_DEV_OK) {
            free(buf);
            audio_device_release(dev);
            ESP_LOGE(TAG, "Tone output write failed");
            return lua_audio_push_error(L, "audio play_tone: write failed");
        }
        frames_written += frames_this;
    }
    free(buf);
    audio_device_release(dev);
    lua_pushboolean(L, 1);
    return 1;
}
