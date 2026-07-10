/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

static bool s_fft_ready = false;
static int s_fft_init_size = 0;

int lua_audio_analyzer_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "input");
    audio_device_t *input = lua_audio_check_device(L, -1, AUDIO_DEVICE_INPUT, "analyzer");
    int input_idx = lua_gettop(L);

    audio_analyzer_t *analyzer = (audio_analyzer_t *)lua_newuserdata(L, sizeof(*analyzer));
    memset(analyzer, 0, sizeof(*analyzer));
    analyzer->input = input;
    lua_pushvalue(L, input_idx);
    analyzer->input_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    input->holders++;
    luaL_getmetatable(L, AUDIO_ANALYZER_META);
    lua_setmetatable(L, -2);
    lua_remove(L, input_idx);
    return 1;
}

int lua_audio_analyzer_close(lua_State *L)
{
    audio_analyzer_t *analyzer = (audio_analyzer_t *)luaL_checkudata(L, 1, AUDIO_ANALYZER_META);
    if (!analyzer || analyzer->closed) {
        lua_pushboolean(L, 1);
        return 1;
    }
    if (analyzer->input && analyzer->input->holders > 0) {
        analyzer->input->holders--;
    }
    analyzer->input = NULL;
    if (analyzer->input_ref != LUA_NOREF && analyzer->input_ref != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, analyzer->input_ref);
        analyzer->input_ref = LUA_NOREF;
    }
    analyzer->closed = true;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_analyzer_gc(lua_State *L)
{
    return lua_audio_analyzer_close(L);
}

static bool audio_is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1U)) == 0;
}

static esp_err_t audio_fft_ensure_ready(int fft_size)
{
    if (s_fft_ready && s_fft_init_size >= fft_size) {
        return ESP_OK;
    }
    if (s_fft_ready) {
        dsps_fft2r_deinit_fc32();
        s_fft_ready = false;
        s_fft_init_size = 0;
    }
    esp_err_t err = dsps_fft2r_init_fc32(NULL, fft_size);
    if (err == ESP_OK) {
        s_fft_ready = true;
        s_fft_init_size = fft_size;
    }
    return err;
}

static uint8_t audio_spectrum_db_to_level(float db)
{
    if (db <= AUDIO_SPECTRUM_DB_MIN) {
        return 0;
    }
    if (db >= AUDIO_SPECTRUM_DB_MAX) {
        return 255;
    }
    float scaled = (db - AUDIO_SPECTRUM_DB_MIN) * 255.0f / (AUDIO_SPECTRUM_DB_MAX - AUDIO_SPECTRUM_DB_MIN);
    if (scaled < 0.0f) {
        scaled = 0.0f;
    } else if (scaled > 255.0f) {
        scaled = 255.0f;
    }
    return (uint8_t)(scaled + 0.5f);
}

static uint32_t audio_spectrum_log_bin_start(uint32_t band, uint32_t band_count, uint32_t half_bins)
{
    double min_bin = 1.0;
    double max_bin = (double)(half_bins - 1);
    double ratio;
    double pos;

    if (half_bins <= 1 || band_count == 0) {
        return 1;
    }
    if (band == 0) {
        return 1;
    }
    ratio = pow(max_bin / min_bin, 1.0 / (double)band_count);
    pos = min_bin * pow(ratio, (double)band);
    if (pos < 1.0) {
        pos = 1.0;
    }
    if (pos > max_bin) {
        pos = max_bin;
    }
    return (uint32_t)pos;
}

static esp_err_t audio_analyzer_capture_i16(audio_device_t *input, uint32_t frames, int16_t **ret_samples, uint32_t *ret_frames)
{
    audio_format_t dst = {
        .sample_rate = input->fmt.sample_rate,
        .channels = 1,
        .bits = 16,
    };
    audio_format_complete(&dst);
    audio_converter_t converter = {0};
    uint32_t in_bytes = frames * input->fmt.bytes_per_frame;
    uint8_t *in_buf = malloc(in_bytes);
    if (!in_buf) {
        ESP_LOGE(TAG, "Analyzer input buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    if (audio_converter_create(&converter, &input->fmt, &dst) != ESP_OK) {
        free(in_buf);
        return ESP_FAIL;
    }
    if (esp_codec_dev_read(input->codec_dev, in_buf, (int)in_bytes) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "Analyzer codec read failed");
        audio_converter_destroy(&converter);
        free(in_buf);
        return ESP_FAIL;
    }
    uint8_t *out = NULL;
    uint32_t out_bytes = 0;
    if (audio_converter_process(&converter, in_buf, in_bytes, &out, &out_bytes) != ESP_OK) {
        audio_converter_destroy(&converter);
        free(in_buf);
        return ESP_FAIL;
    }
    int16_t *samples = malloc(out_bytes);
    if (!samples) {
        ESP_LOGE(TAG, "Analyzer output buffer alloc failed");
        audio_converter_destroy(&converter);
        free(in_buf);
        return ESP_ERR_NO_MEM;
    }
    memcpy(samples, out, out_bytes);
    *ret_samples = samples;
    *ret_frames = out_bytes / sizeof(int16_t);
    audio_converter_destroy(&converter);
    free(in_buf);
    return ESP_OK;
}

int lua_audio_analyzer_read_level(lua_State *L)
{
    audio_analyzer_t *analyzer = lua_audio_check_analyzer(L, 1, "read_level");
    uint32_t duration_ms = 100;
    int16_t *samples = NULL;
    uint32_t frames = 0;
    int64_t sum_sq = 0;
    int32_t peak = 0;

    if (!lua_isnoneornil(L, 2)) {
        if (lua_istable(L, 2)) {
            duration_ms = lua_audio_get_u32_field(L, 2, "duration_ms", 0, 100);
        } else {
            duration_ms = (uint32_t)luaL_checkinteger(L, 2);
        }
    }
    if (!audio_device_acquire(analyzer->input)) {
        return lua_audio_push_error(L, "audio analyzer: input busy");
    }
    uint32_t target_frames = (uint32_t)(((uint64_t)analyzer->input->fmt.sample_rate * duration_ms) / 1000);
    esp_err_t err = audio_analyzer_capture_i16(analyzer->input, target_frames, &samples, &frames);
    audio_device_release(analyzer->input);
    if (err != ESP_OK) {
        free(samples);
        return lua_audio_push_error(L, "audio analyzer: capture failed");
    }
    for (uint32_t i = 0; i < frames; i++) {
        int32_t s = samples[i];
        sum_sq += (int64_t)s * s;
        if (s < 0) {
            s = -s;
        }
        if (s > peak) {
            peak = s;
        }
    }
    int32_t rms = (frames > 0) ? (int32_t)sqrt((double)sum_sq / frames) : 0;
    free(samples);
    lua_newtable(L);
    lua_pushinteger(L, rms);
    lua_setfield(L, -2, "rms");
    lua_pushinteger(L, peak);
    lua_setfield(L, -2, "peak");
    lua_pushinteger(L, duration_ms);
    lua_setfield(L, -2, "duration_ms");
    return 1;
}

int lua_audio_analyzer_read_spectrum(lua_State *L)
{
    audio_analyzer_t *analyzer = lua_audio_check_analyzer(L, 1, "read_spectrum");
    uint32_t fft_size = AUDIO_SPECTRUM_DEF_FFT;
    uint32_t band_count = AUDIO_SPECTRUM_DEF_BANDS;
    int16_t *samples = NULL;
    uint32_t frames = 0;
    float *window = NULL;
    float *fft_buf = NULL;

    if (!lua_isnoneornil(L, 2)) {
        if (lua_istable(L, 2)) {
            fft_size = lua_audio_get_u32_field(L, 2, "fft_size", 0, AUDIO_SPECTRUM_DEF_FFT);
            band_count = lua_audio_get_u32_field(L, 2, "bands", 0, AUDIO_SPECTRUM_DEF_BANDS);
        } else {
            fft_size = (uint32_t)luaL_checkinteger(L, 2);
            band_count = (uint32_t)luaL_optinteger(L, 3, AUDIO_SPECTRUM_DEF_BANDS);
        }
    }
    if (fft_size < AUDIO_SPECTRUM_MIN_FFT || fft_size > AUDIO_SPECTRUM_MAX_FFT || !audio_is_power_of_two(fft_size)) {
        return luaL_error(L, "audio read_spectrum: fft_size must be a power of two in range 64..4096");
    }
    uint32_t half_bins = fft_size / 2;
    if (band_count == 0 || band_count > half_bins) {
        return luaL_error(L, "audio read_spectrum: bands must be in range 1..fft_size/2");
    }
    if (audio_fft_ensure_ready((int)fft_size) != ESP_OK) {
        return lua_audio_push_error(L, "audio analyzer: FFT init failed");
    }
    if (!audio_device_acquire(analyzer->input)) {
        return lua_audio_push_error(L, "audio analyzer: input busy");
    }
    esp_err_t err = audio_analyzer_capture_i16(analyzer->input, fft_size, &samples, &frames);
    audio_device_release(analyzer->input);
    if (err != ESP_OK || frames < fft_size) {
        free(samples);
        return lua_audio_push_error(L, "audio analyzer: capture failed");
    }

    window = (float *)malloc(sizeof(float) * fft_size);
    fft_buf = (float *)calloc((size_t)fft_size * 2U, sizeof(float));
    if (!window || !fft_buf) {
        free(samples);
        free(window);
        free(fft_buf);
        ESP_LOGE(TAG, "Spectrum buffer alloc failed");
        return lua_audio_push_error(L, "audio analyzer: out of memory");
    }
    dsps_wind_hann_f32(window, (int)fft_size);

    int64_t sum_sq = 0;
    for (uint32_t i = 0; i < fft_size; i++) {
        int32_t s = samples[i];
        sum_sq += (int64_t)s * s;
        fft_buf[2 * i] = ((float)s / 32768.0f) * window[i];
        fft_buf[2 * i + 1] = 0.0f;
    }
    dsps_fft2r_fc32(fft_buf, (int)fft_size);
    dsps_bit_rev_fc32(fft_buf, (int)fft_size);

    lua_newtable(L);
    lua_newtable(L);
    uint32_t peak_bin = 0;
    float peak_mag = 0.0f;
    for (uint32_t band = 0; band < band_count; band++) {
        uint32_t start_bin = audio_spectrum_log_bin_start(band, band_count, half_bins);
        uint32_t end_bin = audio_spectrum_log_bin_start(band + 1, band_count, half_bins);
        if (end_bin <= start_bin) {
            end_bin = start_bin + 1;
        }
        if (end_bin > half_bins) {
            end_bin = half_bins;
        }
        float band_peak = 0.0f;
        for (uint32_t bin = start_bin; bin < end_bin; bin++) {
            float real = fft_buf[2 * bin];
            float imag = fft_buf[2 * bin + 1];
            float mag = sqrtf(real * real + imag * imag);
            if (mag > band_peak) {
                band_peak = mag;
            }
            if (mag > peak_mag) {
                peak_mag = mag;
                peak_bin = bin;
            }
        }
        float band_db = 20.0f * log10f((band_peak + 1e-9f) / (float)fft_size);
        lua_pushinteger(L, (lua_Integer)audio_spectrum_db_to_level(band_db));
        lua_rawseti(L, -2, (lua_Integer)band + 1);
    }
    lua_setfield(L, -2, "bands");

    float peak_db = 20.0f * log10f((peak_mag + 1e-9f) / (float)fft_size);
    int32_t rms = (int32_t)sqrt((double)sum_sq / fft_size);
    lua_pushnumber(L, ((lua_Number)peak_bin * (lua_Number)analyzer->input->fmt.sample_rate) / (lua_Number)fft_size);
    lua_setfield(L, -2, "peak_freq_hz");
    lua_pushnumber(L, peak_db);
    lua_setfield(L, -2, "peak_db");
    lua_pushinteger(L, rms);
    lua_setfield(L, -2, "rms");
    lua_pushinteger(L, (lua_Integer)fft_size);
    lua_setfield(L, -2, "fft_size");
    lua_pushinteger(L, (lua_Integer)band_count);
    lua_setfield(L, -2, "band_count");
    lua_pushinteger(L, (lua_Integer)analyzer->input->fmt.sample_rate);
    lua_setfield(L, -2, "sample_rate");

    free(samples);
    free(window);
    free(fft_buf);
    return 1;
}
