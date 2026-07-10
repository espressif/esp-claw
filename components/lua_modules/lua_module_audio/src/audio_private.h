/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_asrc.h"
#include "esp_asrc_types.h"
#include "esp_aac_enc.h"
#include "esp_audio_simple_player_advance.h"
#include "esp_audio_simple_player.h"
#include "esp_codec_dev.h"
#include "esp_crt_bundle.h"
#include "esp_dsp.h"
#include "esp_err.h"
#include "esp_gmf_io_http.h"
#include "esp_gmf_obj.h"
#include "esp_gmf_pipeline.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lauxlib.h"
#include "lua.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define AUDIO_TAG                     "lua_audio"
#define TAG                           AUDIO_TAG
#define AUDIO_CHUNK_BYTES             512
#define AUDIO_DEFAULT_VOL             80
#define AUDIO_INPUT_GAIN_DB_MAX       30.0f
#define AUDIO_DEVICE_INPUT_META       "lua_audio_input"
#define AUDIO_DEVICE_OUTPUT_META      "lua_audio_output"
#define AUDIO_PLAYER_META             "lua_audio_player"
#define AUDIO_RECORDER_META           "lua_audio_recorder"
#define AUDIO_ANALYZER_META           "lua_audio_analyzer"
#define AUDIO_SPECTRUM_MIN_FFT        64
#define AUDIO_SPECTRUM_MAX_FFT        4096
#define AUDIO_SPECTRUM_DEF_FFT        512
#define AUDIO_SPECTRUM_DEF_BANDS      16
#define AUDIO_SPECTRUM_DB_MIN         (-90.0f)
#define AUDIO_SPECTRUM_DB_MAX         (-20.0f)
#define AUDIO_ASRC_COMPLEXITY         3
#define AUDIO_ASRC_TIMEOUT_MS         1000
#define AUDIO_CODEC_VREG_FORMAT_MAGIC        0x7AC0
#define AUDIO_CODEC_VREG_FORMAT_SAMPLE_RATE  0x7AC1
#define AUDIO_CODEC_VREG_FORMAT_CHANNELS     0x7AC2
#define AUDIO_CODEC_VREG_FORMAT_BITS         0x7AC3
#define AUDIO_CODEC_VREG_FORMAT_MAGIC_VALUE  0x55414346

typedef enum {
    AUDIO_DEVICE_INPUT = 0,
    AUDIO_DEVICE_OUTPUT,
} audio_device_kind_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
    uint8_t bytes_per_frame;
} audio_format_t;

typedef struct audio_device_t {
    audio_device_kind_t kind;
    esp_codec_dev_handle_t codec_dev;
    audio_format_t fmt;
    int volume;
    bool closed;
    uint8_t holders;
    bool active;
} audio_device_t;

typedef struct {
    bool bypass;
    esp_asrc_handle_t handle;
    audio_format_t src;
    audio_format_t dst;
    uint16_t in_frame_bytes;
    uint16_t out_frame_bytes;
    uint8_t *in_buf;
    uint32_t in_buf_size;
    uint8_t *out_buf;
    uint32_t out_buf_size;
    esp_asrc_buffer_alignment_t align;
} audio_converter_t;

typedef struct {
    esp_asp_handle_t asp;
    audio_device_t *output;
    int output_ref;
    SemaphoreHandle_t lock;
    audio_converter_t converter;
    bool converter_ready;
    bool closed;
    bool running;
    bool current_uri_https;
    esp_asp_state_t state;
    esp_asp_music_info_t music_info;
    bool has_music_info;
} audio_player_t;

typedef struct {
    audio_device_t *input;
    int input_ref;
    bool closed;
} audio_recorder_t;

typedef struct {
    audio_device_t *input;
    int input_ref;
    bool closed;
} audio_analyzer_t;

char *audio_strdup(const char *s);
bool audio_format_equal(const audio_format_t *a, const audio_format_t *b);
esp_err_t audio_format_complete(audio_format_t *fmt);
void audio_format_log(char *buf, size_t len, const audio_format_t *fmt);
uint8_t lua_audio_get_u8_field(lua_State *L, int idx, const char *name, int raw_index, uint8_t def);
uint32_t lua_audio_get_u32_field(lua_State *L, int idx, const char *name, int raw_index, uint32_t def);
bool lua_audio_parse_format_table(lua_State *L, int idx, audio_format_t *fmt, bool allow_missing);
void *lua_audio_get_codec_field(lua_State *L, int idx);
int lua_audio_get_int_field(lua_State *L, int idx, const char *name, int def);
void lua_audio_push_format(lua_State *L, const audio_format_t *fmt);
int lua_audio_push_error(lua_State *L, const char *msg);
int lua_audio_push_errorf(lua_State *L, const char *fmt, ...);

audio_device_t *lua_audio_check_device(lua_State *L, int idx, audio_device_kind_t kind, const char *what);
audio_player_t *lua_audio_check_player(lua_State *L, int idx, const char *what);
audio_recorder_t *lua_audio_check_recorder(lua_State *L, int idx, const char *what);
audio_analyzer_t *lua_audio_check_analyzer(lua_State *L, int idx, const char *what);

bool audio_device_acquire(audio_device_t *dev);
void audio_device_release(audio_device_t *dev);

char *audio_uri_from_path(const char *path);
char *audio_path_from_file_arg(const char *path_or_uri);
bool audio_path_valid(const char *path, const char *ext);

void audio_converter_destroy(audio_converter_t *converter);
esp_err_t audio_converter_create(audio_converter_t *converter, const audio_format_t *src, const audio_format_t *dst);
esp_err_t audio_converter_process(audio_converter_t *converter, const uint8_t *in, uint32_t in_bytes, uint8_t **out, uint32_t *out_bytes);

int lua_audio_new_output(lua_State *L);
int lua_audio_new_input(lua_State *L);
int lua_audio_device_close(lua_State *L);
int lua_audio_device_gc(lua_State *L);
int lua_audio_device_info(lua_State *L);
int lua_audio_output_set_volume(lua_State *L);
int lua_audio_output_get_volume(lua_State *L);
int lua_audio_output_set_mute(lua_State *L);
int lua_audio_input_set_volume(lua_State *L);
int lua_audio_input_get_volume(lua_State *L);
int lua_audio_output_write(lua_State *L);
int lua_audio_input_read(lua_State *L);
int lua_audio_output_play_tone(lua_State *L);

int lua_audio_player_new(lua_State *L);
int lua_audio_player_close(lua_State *L);
int lua_audio_player_gc(lua_State *L);
int lua_audio_player_play(lua_State *L);
int lua_audio_player_stop(lua_State *L);
int lua_audio_player_pause(lua_State *L);
int lua_audio_player_resume(lua_State *L);
int lua_audio_player_poll(lua_State *L);

int lua_audio_recorder_new(lua_State *L);
int lua_audio_recorder_close(lua_State *L);
int lua_audio_recorder_gc(lua_State *L);
int lua_audio_recorder_record(lua_State *L);

int lua_audio_analyzer_new(lua_State *L);
int lua_audio_analyzer_close(lua_State *L);
int lua_audio_analyzer_gc(lua_State *L);
int lua_audio_analyzer_read_level(lua_State *L);
int lua_audio_analyzer_read_spectrum(lua_State *L);
