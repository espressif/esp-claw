/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_hub.h"
#include "audio_private.h"
#include "cap_lua.h"

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

static const char *audio_device_owner_tag(void)
{
    /* Prefer the current Lua job tag so mixer/capture logs correlate with
     * the script that opened the track. */
    const char *tag = cap_lua_current_owner_tag();
    return tag ? tag : "lua/module/audio";
}

static esp_err_t audio_device_open_output(audio_device_t *dev)
{
    audio_mixer_handle_t mixer = NULL;
    if (audio_hub_get_mixer(&mixer) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    audio_mixer_track_handle_t track = NULL;
    esp_err_t err = audio_mixer_open_track(mixer,
                                           AUDIO_MIXER_TRACK_APP,
                                           audio_device_owner_tag(),
                                           &track);
    if (err != ESP_OK) {
        return err;
    }
    /* Adopt the mixer's negotiated format; the mixer does not resample per
     * track, so producers must submit PCM in this format. */
    uint32_t rate = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    (void)audio_mixer_track_info(track, &rate, &channels, &bits);
    dev->fmt.sample_rate = rate;
    dev->fmt.channels    = channels;
    dev->fmt.bits        = bits;
    if (audio_format_complete(&dev->fmt) != ESP_OK) {
        ESP_LOGE(TAG, "mixer reported invalid format: rate=%" PRIu32 " ch=%u bits=%u",
                 rate, channels, bits);
        audio_mixer_close_track(track);
        return ESP_ERR_INVALID_STATE;
    }
    dev->sink_handle = track;
    dev->closed = false;
    return ESP_OK;
}

static esp_err_t audio_device_open_input(audio_device_t *dev, const audio_format_t *req_fmt)
{
    audio_capture_handle_t capture = NULL;
    if (audio_hub_get_capture(&capture) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    const audio_capture_sub_format_t fmt_req = {
        .sample_rate = req_fmt->sample_rate,
        .channels    = req_fmt->channels,
        .bits        = req_fmt->bits,
    };
    audio_capture_sub_handle_t sub = NULL;
    esp_err_t err = audio_capture_open_subscriber(capture,
                                                  AUDIO_CAPTURE_SUB_APP,
                                                  &fmt_req,
                                                  audio_device_owner_tag(),
                                                  &sub);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t rate = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    (void)audio_capture_sub_info(sub, &rate, &channels, &bits);
    dev->fmt.sample_rate = rate;
    dev->fmt.channels    = channels;
    dev->fmt.bits        = bits;
    if (audio_format_complete(&dev->fmt) != ESP_OK) {
        ESP_LOGE(TAG, "capture reported invalid format: rate=%" PRIu32 " ch=%u bits=%u",
                 rate, channels, bits);
        audio_capture_close_subscriber(sub);
        return ESP_ERR_INVALID_STATE;
    }
    dev->sink_handle = sub;
    dev->closed = false;
    return ESP_OK;
}

esp_err_t audio_device_write(audio_device_t *dev, const void *buf, size_t bytes)
{
    if (!dev || dev->closed || dev->kind != AUDIO_DEVICE_OUTPUT || !dev->sink_handle || !buf) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bytes == 0) {
        return ESP_OK;
    }
    audio_mixer_track_handle_t track = (audio_mixer_track_handle_t)dev->sink_handle;
    const uint8_t *p = (const uint8_t *)buf;
    size_t remaining = bytes;

    /* Bounded-blocking mixer writes naturally pace this loop; a short return
     * means the mixer task stalled — treat it as a fatal write error. */
    while (remaining > 0) {
        size_t chunk = remaining > AUDIO_CHUNK_BYTES ? AUDIO_CHUNK_BYTES : remaining;
        size_t written = audio_mixer_track_write(track, p, chunk);
        if (written == 0) {
            return ESP_FAIL;
        }
        p += written;
        remaining -= written;
    }
    return ESP_OK;
}

esp_err_t audio_device_read(audio_device_t *dev, void *buf, size_t bytes, uint32_t timeout_ms)
{
    if (!dev || dev->closed || dev->kind != AUDIO_DEVICE_INPUT || !dev->sink_handle || !buf) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bytes == 0) {
        return ESP_OK;
    }
    if (timeout_ms == 0) {
        timeout_ms = AUDIO_INPUT_READ_TIMEOUT_MS;
    }
    audio_capture_sub_handle_t sub = (audio_capture_sub_handle_t)dev->sink_handle;
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = bytes;
    uint32_t deadline_left = timeout_ms;
    while (remaining > 0) {
        uint32_t slice = deadline_left > 200 ? 200 : deadline_left;
        size_t got = audio_capture_sub_read(sub, p, remaining, slice);
        if (got == 0) {
            if (slice >= deadline_left) {
                return ESP_ERR_TIMEOUT;
            }
            deadline_left -= slice;
            continue;
        }
        p += got;
        remaining -= got;
    }
    return ESP_OK;
}

esp_err_t audio_device_flush_input(audio_device_t *dev)
{
    if (!dev || dev->closed || dev->kind != AUDIO_DEVICE_INPUT || !dev->sink_handle) return ESP_ERR_INVALID_STATE;
    return audio_capture_sub_flush((audio_capture_sub_handle_t)dev->sink_handle);
}

static int lua_audio_new_device(lua_State *L, audio_device_kind_t kind)
{
    /* Accepts either no arg or an opts table
     * `{ volume=..., sample_rate=..., channels=..., bits=... }`. Only the
     * "app" mixer/capture slot is exposed to Lua; there is no role field. */
    int volume = AUDIO_DEFAULT_VOL;
    audio_format_t req_fmt = {0};
    if (lua_istable(L, 1)) {
        volume = lua_audio_get_int_field(L, 1, "volume", AUDIO_DEFAULT_VOL);
        (void)lua_audio_get_codec_field(L, 1);
        req_fmt.sample_rate = lua_audio_get_u32_field(L, 1, "sample_rate", 2, 0);
        req_fmt.channels    = lua_audio_get_u8_field(L, 1, "channels", 3, 0);
        req_fmt.bits        = lua_audio_get_u8_field(L, 1, "bits", 4, 0);
    } else if (!lua_isnoneornil(L, 1)) {
        return lua_audio_push_error(L,
            "audio.open_output/open_input: expected opts table or no argument");
    }

    audio_device_t *dev = (audio_device_t *)lua_newuserdata(L, sizeof(*dev));
    memset(dev, 0, sizeof(*dev));
    dev->kind = kind;
    dev->closed = true; /* flipped in audio_device_open_* on success */
    dev->volume = volume;

    if (dev->volume < 0 || dev->volume > 100) {
        return lua_audio_push_error(L, kind == AUDIO_DEVICE_OUTPUT ? "audio output: volume must be 0..100" : "audio input: volume must be 0..100");
    }

    esp_err_t err;
    if (kind == AUDIO_DEVICE_OUTPUT) {
        (void)audio_format_complete(&req_fmt);
        err = audio_device_open_output(dev);
    } else {
        err = audio_device_open_input(dev, &req_fmt);
    }

    if (err == ESP_ERR_NOT_FOUND) {
        return lua_audio_push_error(L, "audio codec not available");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return lua_audio_push_error(L,
                                    kind == AUDIO_DEVICE_OUTPUT
                                        ? "audio output: app track already open"
                                        : "audio input: app subscriber already open");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio %s open failed: %s",
                 kind == AUDIO_DEVICE_OUTPUT ? "output" : "input", esp_err_to_name(err));
        return lua_audio_push_error(L, kind == AUDIO_DEVICE_OUTPUT ? "audio output: open failed" : "audio input: open failed");
    }

    luaL_getmetatable(L, kind == AUDIO_DEVICE_OUTPUT ? AUDIO_DEVICE_OUTPUT_META : AUDIO_DEVICE_INPUT_META);
    lua_setmetatable(L, -2);
    return 1;
}

int lua_audio_open_output(lua_State *L)
{
    return lua_audio_new_device(L, AUDIO_DEVICE_OUTPUT);
}

int lua_audio_open_input(lua_State *L)
{
    return lua_audio_new_device(L, AUDIO_DEVICE_INPUT);
}

static void audio_device_close_backend(audio_device_t *dev)
{
    if (!dev || !dev->sink_handle) {
        return;
    }
    if (dev->kind == AUDIO_DEVICE_OUTPUT) {
        audio_mixer_close_track((audio_mixer_track_handle_t)dev->sink_handle);
    } else {
        audio_capture_close_subscriber((audio_capture_sub_handle_t)dev->sink_handle);
    }
    dev->sink_handle = NULL;
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
    audio_device_close_backend(dev);
    dev->closed = true;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_device_gc(lua_State *L)
{
    audio_device_t *dev = (audio_device_t *)lua_touserdata(L, 1);
    if (dev && !dev->closed && dev->holders == 0 && !dev->active) {
        audio_device_close_backend(dev);
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
    /* The mixer/capture handle is the source of truth for the negotiated
     * format; refresh dev->fmt from it every call. */
    if (!dev->closed && dev->sink_handle) {
        uint32_t rate = 0;
        uint8_t channels = 0;
        uint8_t bits = 0;
        if (dev->kind == AUDIO_DEVICE_OUTPUT) {
            (void)audio_mixer_track_info((audio_mixer_track_handle_t)dev->sink_handle,
                                         &rate, &channels, &bits);
        } else {
            (void)audio_capture_sub_info((audio_capture_sub_handle_t)dev->sink_handle,
                                         &rate, &channels, &bits);
        }
        audio_format_t fmt = {
            .sample_rate = rate,
            .channels    = channels,
            .bits        = bits,
        };
        if (audio_format_complete(&fmt) == ESP_OK) {
            dev->fmt = fmt;
        }
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

/* Output volume routes to the mixer (physical DAC gain). Input gain is a
 * shadow-only value; the capture hub does not expose a gain knob yet. */

int lua_audio_output_set_volume(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_OUTPUT, "set_volume");
    int vol = (int)luaL_checkinteger(L, 2);
    if (vol < 0 || vol > 100) {
        return luaL_error(L, "audio set_volume: volume must be 0..100");
    }
    audio_mixer_handle_t mixer = NULL;
    if (audio_hub_get_mixer(&mixer) != ESP_OK) {
        return lua_audio_push_error(L, "audio set_volume: mixer unavailable");
    }
    esp_err_t err = audio_mixer_set_output_volume(mixer, vol);
    if (err != ESP_OK) {
        return lua_audio_push_error(L, "audio set_volume: mixer rejected");
    }
    dev->volume = vol;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_output_get_volume(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_OUTPUT, "get_volume");
    audio_mixer_handle_t mixer = NULL;
    (void)audio_hub_get_mixer(&mixer);
    int vol = dev->volume;
    if (mixer != NULL) {
        int live = 0;
        if (audio_mixer_get_output_volume(mixer, &live) == ESP_OK) {
            vol = live;
            dev->volume = vol;
        }
    }
    lua_pushinteger(L, vol);
    return 1;
}

int lua_audio_output_set_mute(lua_State *L)
{
    audio_device_t *dev = lua_audio_check_device(L, 1, AUDIO_DEVICE_OUTPUT, "set_mute");
    bool mute = lua_toboolean(L, 2);
    (void)dev;
    ESP_LOGD(TAG, "output set_mute=%d ignored: mute is owned by the mixer", (int)mute);
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
    dev->volume = vol;
    ESP_LOGD(TAG, "input set_volume=%d ignored: gain is owned by capture", vol);
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
    esp_err_t err = audio_device_write(dev, data, len);
    audio_device_release(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Output write failed: err=%s", esp_err_to_name(err));
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
    esp_err_t err = audio_device_read(dev, buf, bytes, 0);
    audio_device_release(dev);
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "Input read failed: err=%s", esp_err_to_name(err));
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
        size_t chunk_bytes = (size_t)(frames_this * dev->fmt.bytes_per_frame);
        if (audio_device_write(dev, buf, chunk_bytes) != ESP_OK) {
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
