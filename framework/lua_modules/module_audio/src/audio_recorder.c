/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

typedef enum {
    AUDIO_RECORD_FORMAT_WAV = 0,
    AUDIO_RECORD_FORMAT_AAC,
    AUDIO_RECORD_FORMAT_UNKNOWN,
} audio_record_format_t;

static bool audio_recorder_str_eq_ci(const char *a, const char *b)
{
    if (!a || !b) {
        return false;
    }
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static bool audio_recorder_path_has_ext(const char *path, const char *ext)
{
    size_t path_len = path ? strlen(path) : 0;
    size_t ext_len = ext ? strlen(ext) : 0;
    if (path_len < ext_len || ext_len == 0) {
        return false;
    }
    return audio_recorder_str_eq_ci(path + path_len - ext_len, ext);
}

static audio_record_format_t audio_recorder_format_from_path(const char *path)
{
    if (audio_recorder_path_has_ext(path, ".wav")) {
        return AUDIO_RECORD_FORMAT_WAV;
    }
    if (audio_recorder_path_has_ext(path, ".aac")) {
        return AUDIO_RECORD_FORMAT_AAC;
    }
    return AUDIO_RECORD_FORMAT_UNKNOWN;
}

static bool audio_recorder_aac_rate_supported(uint32_t sample_rate)
{
    switch (sample_rate) {
    case 8000:
    case 11025:
    case 12000:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
    case 64000:
    case 88200:
    case 96000:
        return true;
    default:
        return false;
    }
}

static uint32_t audio_recorder_aac_default_bitrate(const audio_format_t *fmt)
{
    uint32_t bitrate = 64000;
    if (fmt->sample_rate <= 12000) {
        bitrate = 32000;
    } else if (fmt->sample_rate <= 16000) {
        bitrate = 48000;
    } else if (fmt->sample_rate >= 44100) {
        bitrate = 96000;
    }
    return bitrate * fmt->channels;
}

static void wav_write_u16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wav_write_u32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)((v >> 8) & 0xFF);
    dst[2] = (uint8_t)((v >> 16) & 0xFF);
    dst[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void wav_build_header(uint8_t *hdr, uint32_t data_size, const audio_format_t *fmt)
{
    uint32_t byte_rate = fmt->sample_rate * fmt->channels * (fmt->bits / 8);
    uint16_t block_align = fmt->channels * (fmt->bits / 8);

    memcpy(hdr + 0, "RIFF", 4);
    wav_write_u32(hdr + 4, data_size + 36);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    wav_write_u32(hdr + 16, 16);
    wav_write_u16(hdr + 20, 1);
    wav_write_u16(hdr + 22, fmt->channels);
    wav_write_u32(hdr + 24, fmt->sample_rate);
    wav_write_u32(hdr + 28, byte_rate);
    wav_write_u16(hdr + 32, block_align);
    wav_write_u16(hdr + 34, fmt->bits);
    memcpy(hdr + 36, "data", 4);
    wav_write_u32(hdr + 40, data_size);
}

int lua_audio_recorder_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "input");
    audio_device_t *input = lua_audio_check_device(L, -1, AUDIO_DEVICE_INPUT, "recorder");
    int input_idx = lua_gettop(L);

    audio_recorder_t *rec = (audio_recorder_t *)lua_newuserdata(L, sizeof(*rec));
    memset(rec, 0, sizeof(*rec));
    rec->input = input;
    lua_pushvalue(L, input_idx);
    rec->input_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    input->holders++;
    luaL_getmetatable(L, AUDIO_RECORDER_META);
    lua_setmetatable(L, -2);
    lua_remove(L, input_idx);
    return 1;
}

int lua_audio_recorder_close(lua_State *L)
{
    audio_recorder_t *rec = (audio_recorder_t *)luaL_checkudata(L, 1, AUDIO_RECORDER_META);
    if (!rec || rec->closed) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (rec->input && rec->input->holders > 0) {
        rec->input->holders--;
    }
    rec->input = NULL;
    if (rec->input_ref != LUA_NOREF && rec->input_ref != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, rec->input_ref);
        rec->input_ref = LUA_NOREF;
    }
    rec->closed = true;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_recorder_gc(lua_State *L)
{
    return lua_audio_recorder_close(L);
}

static int audio_recorder_record_aac(lua_State *L, audio_recorder_t *rec, const char *path, uint32_t duration_ms,
                                     const audio_format_t *enc_fmt, uint32_t bitrate)
{
    if (enc_fmt->bits != 16 || enc_fmt->channels == 0 || enc_fmt->channels > 2 || !audio_recorder_aac_rate_supported(enc_fmt->sample_rate)) {
        ESP_LOGE(TAG, "AAC recorder unsupported format: rate=%" PRIu32 " ch=%u bits=%u", enc_fmt->sample_rate, enc_fmt->channels, enc_fmt->bits);
        return lua_audio_push_error(L, "audio recorder: AAC requires 8k..96kHz, 1..2 channels, 16-bit PCM");
    }

    esp_aac_enc_config_t aac_cfg = {
        .sample_rate = (int)enc_fmt->sample_rate,
        .channel = enc_fmt->channels,
        .bits_per_sample = enc_fmt->bits,
        .bitrate = (int)(bitrate > 0 ? bitrate : audio_recorder_aac_default_bitrate(enc_fmt)),
        .adts_used = true,
    };
    void *encoder = NULL;
    int pcm_size = 0;
    int aac_size = 0;
    uint8_t *read_buf = NULL;
    uint8_t *pending_buf = NULL;
    uint8_t *aac_buf = NULL;
    uint32_t pending_len = 0;
    uint32_t pending_cap = 0;
    FILE *f = NULL;
    audio_converter_t converter = {0};
    uint32_t total_in_frames = (uint32_t)(((uint64_t)rec->input->fmt.sample_rate * duration_ms) / 1000);
    uint32_t done_frames = 0;
    uint32_t total_encoded_bytes = 0;
    int ret = ESP_CODEC_DEV_OK;

    if (!audio_device_acquire(rec->input)) {
        return lua_audio_push_error(L, "audio recorder: input busy");
    }
    if (audio_converter_create(&converter, &rec->input->fmt, enc_fmt) != ESP_OK) {
        audio_device_release(rec->input);
        return lua_audio_push_error(L, "audio recorder: converter create failed");
    }
    if (esp_aac_enc_open(&aac_cfg, sizeof(aac_cfg), &encoder) != ESP_AUDIO_ERR_OK || !encoder) {
        ESP_LOGE(TAG, "AAC encoder open failed");
        audio_converter_destroy(&converter);
        audio_device_release(rec->input);
        return lua_audio_push_error(L, "audio recorder: AAC encoder open failed");
    }
    if (esp_aac_enc_get_frame_size(encoder, &pcm_size, &aac_size) != ESP_AUDIO_ERR_OK || pcm_size <= 0 || aac_size <= 0) {
        ESP_LOGE(TAG, "AAC encoder frame size query failed");
        esp_aac_enc_close(encoder);
        audio_converter_destroy(&converter);
        audio_device_release(rec->input);
        return lua_audio_push_error(L, "audio recorder: AAC frame size query failed");
    }

    read_buf = malloc(AUDIO_CHUNK_BYTES);
    pending_cap = (uint32_t)pcm_size + AUDIO_CHUNK_BYTES * 4;
    pending_buf = malloc(pending_cap);
    aac_buf = malloc(aac_size);
    if (!read_buf || !pending_buf || !aac_buf) {
        ESP_LOGE(TAG, "AAC recorder buffer alloc failed");
        free(read_buf);
        free(pending_buf);
        free(aac_buf);
        esp_aac_enc_close(encoder);
        audio_converter_destroy(&converter);
        audio_device_release(rec->input);
        return lua_audio_push_error(L, "audio recorder: out of memory");
    }

    remove(path);
    f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "AAC recorder open failed: %s", path);
        free(read_buf);
        free(pending_buf);
        free(aac_buf);
        esp_aac_enc_close(encoder);
        audio_converter_destroy(&converter);
        audio_device_release(rec->input);
        return lua_audio_push_error(L, "audio recorder: cannot open file");
    }

    uint32_t chunk_frames = AUDIO_CHUNK_BYTES / rec->input->fmt.bytes_per_frame;
    if (chunk_frames == 0) {
        chunk_frames = 1;
    }
    while (done_frames < total_in_frames || pending_len >= (uint32_t)pcm_size) {
        while (pending_len < (uint32_t)pcm_size && done_frames < total_in_frames) {
            uint32_t frames = total_in_frames - done_frames;
            if (frames > chunk_frames) {
                frames = chunk_frames;
            }
            uint32_t in_bytes = frames * rec->input->fmt.bytes_per_frame;
            if (esp_codec_dev_read(rec->input->codec_dev, read_buf, (int)in_bytes) != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "AAC recorder codec read failed");
                ret = ESP_CODEC_DEV_READ_FAIL;
                break;
            }
            uint8_t *converted = NULL;
            uint32_t converted_bytes = 0;
            if (audio_converter_process(&converter, read_buf, in_bytes, &converted, &converted_bytes) != ESP_OK) {
                ESP_LOGE(TAG, "AAC recorder converter process failed");
                ret = ESP_CODEC_DEV_WRITE_FAIL;
                break;
            }
            if (pending_len + converted_bytes > pending_cap) {
                uint32_t new_cap = pending_len + converted_bytes + (uint32_t)pcm_size;
                uint8_t *new_buf = realloc(pending_buf, new_cap);
                if (!new_buf) {
                    ESP_LOGE(TAG, "AAC recorder pending buffer realloc failed");
                    ret = ESP_CODEC_DEV_NO_MEM;
                    break;
                }
                pending_buf = new_buf;
                pending_cap = new_cap;
            }
            memcpy(pending_buf + pending_len, converted, converted_bytes);
            pending_len += converted_bytes;
            done_frames += frames;
        }
        if (ret != ESP_CODEC_DEV_OK || pending_len < (uint32_t)pcm_size) {
            break;
        }

        esp_audio_enc_in_frame_t in_frame = {
            .buffer = pending_buf,
            .len = (uint32_t)pcm_size,
        };
        esp_audio_enc_out_frame_t out_frame = {
            .buffer = aac_buf,
            .len = (uint32_t)aac_size,
        };
        if (esp_aac_enc_process(encoder, &in_frame, &out_frame) != ESP_AUDIO_ERR_OK ||
            fwrite(aac_buf, 1, out_frame.encoded_bytes, f) != out_frame.encoded_bytes) {
            ESP_LOGE(TAG, "AAC recorder encode/write failed");
            ret = ESP_CODEC_DEV_WRITE_FAIL;
            break;
        }
        total_encoded_bytes += out_frame.encoded_bytes;
        pending_len -= (uint32_t)pcm_size;
        if (pending_len > 0) {
            memmove(pending_buf, pending_buf + pcm_size, pending_len);
        }
    }

    fclose(f);
    if (ret != ESP_CODEC_DEV_OK) {
        remove(path);
    }
    free(read_buf);
    free(pending_buf);
    free(aac_buf);
    esp_aac_enc_close(encoder);
    audio_converter_destroy(&converter);
    audio_device_release(rec->input);
    if (ret != ESP_CODEC_DEV_OK) {
        return lua_audio_push_error(L, "audio recorder: AAC record failed");
    }

    lua_newtable(L);
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, duration_ms);
    lua_setfield(L, -2, "duration_ms");
    lua_pushinteger(L, total_encoded_bytes);
    lua_setfield(L, -2, "bytes");
    lua_pushstring(L, "aac");
    lua_setfield(L, -2, "encoding");
    lua_audio_push_format(L, enc_fmt);
    lua_setfield(L, -2, "format");
    return 1;
}

int lua_audio_recorder_record(lua_State *L)
{
    audio_recorder_t *rec = lua_audio_check_recorder(L, 1, "record");
    const char *path_arg = luaL_checkstring(L, 2);
    uint32_t duration_ms = 0;
    uint32_t bitrate = 0;
    int bitrate_opt = 0;
    audio_format_t out_fmt = rec->input->fmt;
    audio_record_format_t record_format = AUDIO_RECORD_FORMAT_UNKNOWN;
    char *path = NULL;
    FILE *f = NULL;
    uint8_t *in_buf = NULL;
    audio_converter_t converter = {0};
    uint32_t total_out_bytes = 0;
    uint8_t wav_hdr[44] = {0};

    if (lua_isnoneornil(L, 3)) {
        return luaL_error(L, "audio recorder: opts.duration_ms is required");
    }
    luaL_checktype(L, 3, LUA_TTABLE);
    duration_ms = lua_audio_get_u32_field(L, 3, "duration_ms", 0, 0);
    out_fmt.sample_rate = lua_audio_get_u32_field(L, 3, "sample_rate", 0, out_fmt.sample_rate);
    out_fmt.channels = lua_audio_get_u8_field(L, 3, "channels", 0, out_fmt.channels);
    out_fmt.bits = lua_audio_get_u8_field(L, 3, "bits", 0, out_fmt.bits);
    bitrate_opt = lua_audio_get_int_field(L, 3, "bitrate", 0);
    if (bitrate_opt < 0) {
        return luaL_error(L, "audio recorder: bitrate must be non-negative");
    }
    bitrate = (uint32_t)bitrate_opt;
    if (audio_format_complete(&out_fmt) != ESP_OK) {
        ESP_LOGE(TAG, "Recorder output format invalid: rate=%" PRIu32 " ch=%u bits=%u", out_fmt.sample_rate, out_fmt.channels, out_fmt.bits);
        return lua_audio_push_error(L, "audio recorder: invalid output format");
    }
    if (duration_ms == 0) {
        return luaL_error(L, "audio recorder: duration_ms must be positive");
    }
    path = audio_path_from_file_arg(path_arg);
    record_format = audio_recorder_format_from_path(path);
    if (record_format == AUDIO_RECORD_FORMAT_UNKNOWN) {
        free(path);
        return luaL_error(L, "audio recorder: unsupported file extension");
    }
    if (record_format == AUDIO_RECORD_FORMAT_AAC) {
        int result;
        if (!audio_path_valid(path, ".aac")) {
            free(path);
            return luaL_error(L, "audio recorder: AAC output path must be a .aac file and must not contain '..'");
        }
        result = audio_recorder_record_aac(L, rec, path, duration_ms, &out_fmt, bitrate);
        free(path);
        return result;
    }
    if (!audio_path_valid(path, ".wav")) {
        free(path);
        return luaL_error(L, "audio recorder: WAV output path must be a .wav file and must not contain '..'");
    }
    if (!audio_device_acquire(rec->input)) {
        free(path);
        return lua_audio_push_error(L, "audio recorder: input busy");
    }
    if (audio_converter_create(&converter, &rec->input->fmt, &out_fmt) != ESP_OK) {
        audio_device_release(rec->input);
        free(path);
        return lua_audio_push_error(L, "audio recorder: converter create failed");
    }
    in_buf = malloc(AUDIO_CHUNK_BYTES);
    if (!in_buf) {
        ESP_LOGE(TAG, "Recorder input buffer alloc failed");
        audio_converter_destroy(&converter);
        audio_device_release(rec->input);
        free(path);
        return lua_audio_push_error(L, "audio recorder: out of memory");
    }

    remove(path);
    f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Recorder open failed: %s", path);
        free(in_buf);
        audio_converter_destroy(&converter);
        audio_device_release(rec->input);
        free(path);
        return lua_audio_push_error(L, "audio recorder: cannot open file");
    }
    fwrite(wav_hdr, 1, sizeof(wav_hdr), f);

    uint32_t total_in_frames = (uint32_t)(((uint64_t)rec->input->fmt.sample_rate * duration_ms) / 1000);
    uint32_t done_frames = 0;
    uint32_t chunk_frames = AUDIO_CHUNK_BYTES / rec->input->fmt.bytes_per_frame;
    if (chunk_frames == 0) {
        chunk_frames = 1;
    }
    while (done_frames < total_in_frames) {
        uint32_t frames = total_in_frames - done_frames;
        if (frames > chunk_frames) {
            frames = chunk_frames;
        }
        uint32_t in_bytes = frames * rec->input->fmt.bytes_per_frame;
        if (esp_codec_dev_read(rec->input->codec_dev, in_buf, (int)in_bytes) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "Recorder codec read failed");
            fclose(f);
            remove(path);
            free(in_buf);
            audio_converter_destroy(&converter);
            audio_device_release(rec->input);
            free(path);
            return lua_audio_push_error(L, "audio recorder: read failed");
        }
        uint8_t *out = NULL;
        uint32_t out_bytes = 0;
        if (audio_converter_process(&converter, in_buf, in_bytes, &out, &out_bytes) != ESP_OK || fwrite(out, 1, out_bytes, f) != out_bytes) {
            ESP_LOGE(TAG, "Recorder convert/write failed");
            fclose(f);
            remove(path);
            free(in_buf);
            audio_converter_destroy(&converter);
            audio_device_release(rec->input);
            free(path);
            return lua_audio_push_error(L, "audio recorder: write failed");
        }
        total_out_bytes += out_bytes;
        done_frames += frames;
    }

    wav_build_header(wav_hdr, total_out_bytes, &out_fmt);
    fseek(f, 0, SEEK_SET);
    if (fwrite(wav_hdr, 1, sizeof(wav_hdr), f) != sizeof(wav_hdr)) {
        ESP_LOGE(TAG, "Recorder WAV header finalize failed");
        fclose(f);
        remove(path);
        free(in_buf);
        audio_converter_destroy(&converter);
        audio_device_release(rec->input);
        free(path);
        return lua_audio_push_error(L, "audio recorder: header finalize failed");
    }
    fclose(f);
    free(in_buf);
    audio_converter_destroy(&converter);
    audio_device_release(rec->input);

    lua_newtable(L);
    lua_pushstring(L, path);
    lua_setfield(L, -2, "path");
    lua_pushinteger(L, duration_ms);
    lua_setfield(L, -2, "duration_ms");
    lua_pushinteger(L, total_out_bytes);
    lua_setfield(L, -2, "bytes");
    lua_audio_push_format(L, &out_fmt);
    lua_setfield(L, -2, "format");
    free(path);
    return 1;
}
