/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/queue.h"

#define SAMPLE_PLAYER_SAMPLES 64
#define SAMPLE_PLAYER_VOICES 16
#define SAMPLE_PLAYER_COMMANDS 32
#define SAMPLE_PLAYER_BLOCK_MS 20
#define SAMPLE_PLAYER_TASK_PRIORITY 9
#define SAMPLE_PLAYER_TASK_STACK 4096

typedef struct audio_sample_player *audio_sample_player_handle_t;
typedef struct {
    audio_mixer_track_handle_t track;
    uint32_t source_rate;
    uint8_t max_voices;
    size_t sample_budget_bytes;
    uint32_t attack_ms;
    uint32_t release_ms;
    float limit_threshold;
} audio_sample_player_config_t;
typedef struct {
    uint32_t active_voices;
    uint32_t loaded_samples;
    size_t sample_bytes;
    size_t sample_budget_bytes;
    uint32_t render_max_us;
    uint32_t rendered_blocks;
    esp_err_t error;
} audio_sample_player_status_t;

typedef struct {
    int32_t id;
    int16_t *pcm;
    uint32_t frames;
} sample_player_sample_t;
typedef struct {
    const sample_player_sample_t *sample;
    uint32_t position;
    uint64_t phase;
    uint32_t step;
    uint32_t end_frame;
    uint32_t pending;
    uint64_t sequence;
    float gain;
} sample_player_voice_t;
typedef struct {
    const sample_player_sample_t *sample;
    float gain;
    uint32_t frames;
    uint32_t step;
    uint32_t pending;
    uint64_t sequence;
} sample_player_command_t;

struct audio_sample_player {
    audio_sample_player_config_t config;
    sample_player_sample_t samples[SAMPLE_PLAYER_SAMPLES];
    sample_player_voice_t voices[SAMPLE_PLAYER_VOICES];
    uint32_t sample_count;
    size_t sample_bytes;
    uint32_t frames;
    uint32_t attack_frames;
    uint32_t release_frames;
    uint64_t sequence;
    size_t low_water_bytes;
    float *acc;
    int16_t *pcm;
    audio_converter_t converter;
    QueueHandle_t commands;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    bool started;
    bool stop;
    audio_sample_player_status_t status;
};

typedef struct {
    audio_sample_player_handle_t handle;
    audio_device_t *output;
    int output_ref;
    bool acquired;
} lua_sample_player_t;

static void sample_player_add_voice(audio_sample_player_handle_t s, const sample_player_command_t *command)
{
    /* A zero ID clears all voices; other IDs stop only that playback. */
    if (!command->sample) {
        for (uint32_t i = 0; i < s->config.max_voices; i++) {
            if (!command->sequence || s->voices[i].sequence == command->sequence) s->voices[i].sample = NULL;
        }
        return;
    }
    sample_player_voice_t *slot = &s->voices[0];
    for (uint32_t i = 0; i < s->config.max_voices; i++) {
        sample_player_voice_t *v = &s->voices[i];
        if (!v->sample) { slot = v; break; }
        if (v->sequence < slot->sequence) slot = v;
    }
    *slot = (sample_player_voice_t){.sample = command->sample, .end_frame = command->frames, .step = command->step, .pending = command->pending, .gain = command->gain, .sequence = command->sequence};
}

static uint32_t sample_player_render(audio_sample_player_handle_t s)
{
    memset(s->acc, 0, s->frames * sizeof(float));
    uint32_t active = 0;
    for (uint32_t i = 0; i < s->config.max_voices; i++) {
        sample_player_voice_t *v = &s->voices[i];
        if (!v->sample) continue;
        uint32_t first = v->pending < s->frames ? v->pending : s->frames;
        v->pending -= first;
        for (uint32_t f = first; f < s->frames && v->position < v->end_frame; f++, v->position++) {
            float envelope = 1.0f;
            if (v->position < s->attack_frames) envelope = (float)(v->position + 1) / s->attack_frames;
            uint32_t remaining = v->end_frame - v->position;
            if (remaining < s->release_frames) envelope = fminf(envelope, (float)remaining / s->release_frames);
            /* Q16 phase keeps pitch stable without accumulating float error. */
            uint32_t index = (uint32_t)(v->phase >> 16);
            uint32_t next = index + 1 < v->sample->frames ? index + 1 : index;
            float fraction = (float)(v->phase & 65535) / 65536.0f;
            float value = v->sample->pcm[index] + (v->sample->pcm[next] - v->sample->pcm[index]) * fraction;
            s->acc[f] += value * (v->gain / 32768.0f) * envelope;
            v->phase += v->step;
        }
        if (v->position == v->end_frame) v->sample = NULL;
        else active++;
    }
    float threshold = s->config.limit_threshold;
    for (uint32_t i = 0; i < s->frames; i++) {
        float value = s->acc[i];
        float magnitude = fabsf(value);
        if (threshold > 0 && magnitude > threshold) {
            float over = (magnitude - threshold) / (1.0f - threshold);
            value = copysignf(threshold + (1.0f - threshold) * over / (1.0f + over), value);
        }
        s->pcm[i] = (int16_t)lrintf(fmaxf(-1.0f, fminf(1.0f, value)) * 32767.0f);
    }
    return active;
}

static void sample_player_task(void *arg)
{
    audio_sample_player_handle_t s = arg;
    esp_err_t err = ESP_OK;
    for (;;) {
        xSemaphoreTake(s->lock, portMAX_DELAY);
        bool stop = s->stop;
        xSemaphoreGive(s->lock);
        if (stop) break;
        audio_mixer_track_status_t track_status;
        err = audio_mixer_track_get_status(s->config.track, &track_status);
        if (err != ESP_OK) break;
        /* Keep interactive audio shallow; recheck shutdown after each yield. */
        if (track_status.queued_bytes > s->low_water_bytes) { vTaskDelay(1); continue; }
        sample_player_command_t command;
        /* Bound command processing so a busy producer cannot starve PCM. */
        for (int i = 0; i < SAMPLE_PLAYER_COMMANDS && xQueueReceive(s->commands, &command, 0) == pdTRUE; i++) sample_player_add_voice(s, &command);
        int64_t started = esp_timer_get_time();
        uint32_t active = sample_player_render(s);
        uint8_t *pcm = NULL;
        uint32_t bytes = 0;
        err = audio_converter_process(&s->converter, (const uint8_t *)s->pcm, s->frames * sizeof(int16_t), &pcm, &bytes);
        uint32_t elapsed = (uint32_t)(esp_timer_get_time() - started);
        if (err != ESP_OK) break;
        /* Submit a whole block atomically when space is available. */
        if (audio_mixer_track_write(s->config.track, pcm, bytes) != bytes) { err = ESP_FAIL; break; }
        xSemaphoreTake(s->lock, portMAX_DELAY);
        s->status.active_voices = active;
        s->status.rendered_blocks++;
        if (elapsed > s->status.render_max_us) s->status.render_max_us = elapsed;
        xSemaphoreGive(s->lock);
    }
    xSemaphoreTake(s->lock, portMAX_DELAY);
    s->status.error = err;
    xSemaphoreGive(s->lock);
    if (err != ESP_OK) ESP_LOGE(TAG, "Sample player output failed: %s", esp_err_to_name(err));
    xSemaphoreGive(s->done);
    vTaskDelete(NULL);
}

static void audio_sample_player_delete(audio_sample_player_handle_t s)
{
    if (!s) return;
    if (s->started) {
        xSemaphoreTake(s->lock, portMAX_DELAY);
        s->stop = true;
        xSemaphoreGive(s->lock);
        xSemaphoreTake(s->done, portMAX_DELAY);
        audio_mixer_track_stop(s->config.track);
    }
    for (uint32_t i = 0; i < s->sample_count; i++) free(s->samples[i].pcm);
    audio_converter_destroy(&s->converter);
    if (s->commands) vQueueDelete(s->commands);
    if (s->done) vSemaphoreDelete(s->done);
    if (s->lock) vSemaphoreDelete(s->lock);
    free(s->acc);
    free(s->pcm);
    free(s);
}

static esp_err_t audio_sample_player_create(const audio_sample_player_config_t *config, audio_sample_player_handle_t *out)
{
    if (!config || !out || !config->track || config->sample_budget_bytes < 2) return ESP_ERR_INVALID_ARG;
    if (config->source_rate < 8000 || config->source_rate > 48000 || config->max_voices < 1 || config->max_voices > SAMPLE_PLAYER_VOICES) return ESP_ERR_INVALID_ARG;
    if (config->attack_ms > 1000 || config->release_ms > 1000 || !isfinite(config->limit_threshold) || config->limit_threshold < 0 || config->limit_threshold >= 1) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    audio_sample_player_handle_t s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->config = *config;
    s->frames = config->source_rate * SAMPLE_PLAYER_BLOCK_MS / 1000;
    s->attack_frames = config->source_rate * config->attack_ms / 1000;
    s->release_frames = config->source_rate * config->release_ms / 1000;
    s->acc = calloc(s->frames, sizeof(float));
    s->pcm = calloc(s->frames, sizeof(int16_t));
    s->lock = xSemaphoreCreateMutex();
    s->done = xSemaphoreCreateBinary();
    s->commands = xQueueCreate(SAMPLE_PLAYER_COMMANDS, sizeof(sample_player_command_t));
    esp_err_t err = ESP_ERR_NO_MEM;
    if (!s->acc || !s->pcm || !s->lock || !s->done || !s->commands) goto fail;
    audio_format_t dst = {0};
    err = audio_mixer_track_info(config->track, &dst.sample_rate, &dst.channels, &dst.bits);
    if (err != ESP_OK) goto fail;
    err = audio_format_complete(&dst);
    if (err != ESP_OK) goto fail;
    s->low_water_bytes = (size_t)dst.sample_rate * SAMPLE_PLAYER_BLOCK_MS / 1000 * dst.bytes_per_frame;
    audio_format_t src = {.sample_rate = config->source_rate, .channels = 1, .bits = 16, .bytes_per_frame = 2};
    err = audio_converter_create(&s->converter, &src, &dst);
    if (err != ESP_OK) goto fail;
    err = audio_converter_prepare(&s->converter, s->frames * sizeof(int16_t));
    if (err != ESP_OK) goto fail;
    *out = s;
    return ESP_OK;
fail:
    ESP_LOGE(TAG, "Sample player create failed: %s", esp_err_to_name(err));
    audio_sample_player_delete(s);
    return err;
}

static esp_err_t audio_sample_player_load(audio_sample_player_handle_t s, int32_t id, const uint8_t *pcm, size_t bytes)
{
    if (!s || !pcm || bytes < 2 || bytes % 2 != 0 || bytes > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    if (s->started) return ESP_ERR_INVALID_STATE;
    if (s->sample_count == SAMPLE_PLAYER_SAMPLES) return ESP_ERR_NO_MEM;
    for (uint32_t i = 0; i < s->sample_count; i++) if (s->samples[i].id == id) return ESP_ERR_INVALID_ARG;
    if (bytes > s->config.sample_budget_bytes - s->sample_bytes) {
        ESP_LOGW(TAG, "Sample budget exceeded: used=%u request=%u limit=%u", (unsigned)s->sample_bytes, (unsigned)bytes, (unsigned)s->config.sample_budget_bytes);
        return ESP_ERR_NO_MEM;
    }
    int16_t *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = malloc(bytes);
    if (!copy) { ESP_LOGE(TAG, "Sample allocation failed: bytes=%u", (unsigned)bytes); return ESP_ERR_NO_MEM; }
    for (size_t i = 0; i < bytes / 2; i++) copy[i] = (int16_t)((uint16_t)pcm[i * 2] | ((uint16_t)pcm[i * 2 + 1] << 8));
    s->samples[s->sample_count++] = (sample_player_sample_t){.id = id, .pcm = copy, .frames = bytes / 2};
    s->sample_bytes += bytes;
    return ESP_OK;
}

static esp_err_t audio_sample_player_start(audio_sample_player_handle_t s)
{
    if (!s || s->started || !s->sample_count) return ESP_ERR_INVALID_STATE;
    esp_err_t err = audio_mixer_track_stop(s->config.track);
    if (err != ESP_OK) return err;
    if (xTaskCreate(sample_player_task, "audio_sample_player", SAMPLE_PLAYER_TASK_STACK, s, SAMPLE_PLAYER_TASK_PRIORITY, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    s->started = true;
    return ESP_OK;
}

static esp_err_t audio_sample_player_get_status(audio_sample_player_handle_t s, audio_sample_player_status_t *status)
{
    if (!s || !status) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    *status = s->status;
    status->loaded_samples = s->sample_count;
    status->sample_bytes = s->sample_bytes;
    status->sample_budget_bytes = s->config.sample_budget_bytes;
    xSemaphoreGive(s->lock);
    return ESP_OK;
}

static esp_err_t audio_sample_player_trigger(audio_sample_player_handle_t s, int32_t id, float gain, uint32_t delay_ms, uint32_t duration_ms, float rate, uint64_t *voice_id)
{
    if (!s || !voice_id || !isfinite(gain) || gain < 0 || gain > 1 || delay_ms > 1000 || duration_ms > 60000 || !isfinite(rate) || rate < 0.25f || rate > 4.0f) return ESP_ERR_INVALID_ARG;
    if (!s->started) return ESP_ERR_INVALID_STATE;
    audio_sample_player_status_t status;
    audio_sample_player_get_status(s, &status);
    if (status.error != ESP_OK) return status.error;
    const sample_player_sample_t *sample = NULL;
    for (uint32_t i = 0; i < s->sample_count; i++) if (s->samples[i].id == id) { sample = &s->samples[i]; break; }
    if (!sample) return ESP_ERR_NOT_FOUND;
    uint32_t step = (uint32_t)lrintf(rate * 65536.0f);
    uint64_t natural = (((uint64_t)sample->frames << 16) + step - 1) / step;
    uint32_t frames = duration_ms ? duration_ms * s->config.source_rate / 1000 : (uint32_t)(natural > UINT32_MAX ? UINT32_MAX : natural);
    if (frames > natural) frames = (uint32_t)natural;
    if (s->sequence == UINT64_MAX) return ESP_ERR_INVALID_STATE;
    sample_player_command_t command = {.sequence = ++s->sequence, .sample = sample, .gain = gain, .frames = frames, .step = step, .pending = delay_ms * s->config.source_rate / 1000};
    if (xQueueSend(s->commands, &command, 0) != pdTRUE) return ESP_ERR_TIMEOUT;
    *voice_id = command.sequence;
    return ESP_OK;
}

static esp_err_t audio_sample_player_stop_voice(audio_sample_player_handle_t s, uint64_t voice_id)
{
    if (!s || !voice_id) return ESP_ERR_INVALID_ARG;
    if (!s->started) return ESP_ERR_INVALID_STATE;
    audio_sample_player_status_t status;
    audio_sample_player_get_status(s, &status);
    if (status.error != ESP_OK) return status.error;
    sample_player_command_t command = {.sequence = voice_id};
    return xQueueSend(s->commands, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static esp_err_t audio_sample_player_clear(audio_sample_player_handle_t s)
{
    if (!s || !s->started) return ESP_ERR_INVALID_STATE;
    /* Keep reset and subsequent notes ordered in the worker queue. */
    xQueueReset(s->commands);
    sample_player_command_t command = {0};
    return xQueueSend(s->commands, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static lua_sample_player_t *check_sample_player(lua_State *L)
{
    lua_sample_player_t *s = luaL_checkudata(L, 1, AUDIO_SAMPLE_PLAYER_META);
    luaL_argcheck(L, s->handle != NULL, 1, "sample player is closed");
    return s;
}

static int sample_player_result(lua_State *L, esp_err_t err)
{
    if (err != ESP_OK) return lua_audio_push_errorf(L, "audio sample player: %s", esp_err_to_name(err));
    lua_pushboolean(L, true);
    return 1;
}

int lua_audio_sample_player_close(lua_State *L)
{
    lua_sample_player_t *s = luaL_checkudata(L, 1, AUDIO_SAMPLE_PLAYER_META);
    audio_sample_player_delete(s->handle);
    s->handle = NULL;
    if (s->output) {
        if (s->acquired) audio_device_release(s->output);
        s->output->holders--;
        s->output = NULL;
    }
    if (s->output_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, s->output_ref);
    s->output_ref = LUA_NOREF;
    s->acquired = false;
    return sample_player_result(L, ESP_OK);
}

int lua_audio_sample_player_load(lua_State *L)
{
    lua_sample_player_t *s = check_sample_player(L);
    int32_t id = (int32_t)luaL_checkinteger(L, 2);
    size_t bytes;
    const char *pcm = luaL_checklstring(L, 3, &bytes);
    return sample_player_result(L, audio_sample_player_load(s->handle, id, (const uint8_t *)pcm, bytes));
}

int lua_audio_sample_player_start(lua_State *L)
{
    lua_sample_player_t *s = check_sample_player(L);
    if (!audio_device_acquire(s->output)) return lua_audio_push_error(L, "audio sample player: output busy");
    esp_err_t err = audio_sample_player_start(s->handle);
    if (err == ESP_OK) s->acquired = true;
    else audio_device_release(s->output);
    return sample_player_result(L, err);
}

int lua_audio_sample_player_trigger(lua_State *L)
{
    lua_sample_player_t *s = check_sample_player(L);
    int32_t id = (int32_t)luaL_checkinteger(L, 2);
    float gain = (float)luaL_optnumber(L, 3, 1.0);
    lua_Integer delay = luaL_optinteger(L, 4, 0);
    luaL_argcheck(L, delay >= 0 && delay <= 1000, 4, "delay must be 0..1000 ms");
    lua_Integer duration = luaL_optinteger(L, 5, 0);
    luaL_argcheck(L, duration >= 0 && duration <= 60000, 5, "duration must be 0..60000 ms");
    float rate = (float)luaL_optnumber(L, 6, 1.0);
    uint64_t *voice = lua_newuserdata(L, sizeof(*voice));
    *voice = 0;
    luaL_getmetatable(L, AUDIO_VOICE_META);
    lua_setmetatable(L, -2);
    /* Keep the owner alive and reject handles from another player. */
    lua_pushvalue(L, 1);
    lua_setuservalue(L, -2);
    esp_err_t err = audio_sample_player_trigger(s->handle, id, gain, (uint32_t)delay, (uint32_t)duration, rate, voice);
    return err == ESP_OK ? 1 : sample_player_result(L, err);
}

int lua_audio_sample_player_stop(lua_State *L)
{
    lua_sample_player_t *s = check_sample_player(L);
    uint64_t *voice = luaL_checkudata(L, 2, AUDIO_VOICE_META);
    lua_getuservalue(L, 2);
    bool owned = lua_rawequal(L, 1, -1);
    lua_pop(L, 1);
    luaL_argcheck(L, owned, 2, "voice belongs to another sample player");
    return sample_player_result(L, audio_sample_player_stop_voice(s->handle, *voice));
}

int lua_audio_sample_player_clear(lua_State *L)
{
    return sample_player_result(L, audio_sample_player_clear(check_sample_player(L)->handle));
}

static void set_number(lua_State *L, const char *key, lua_Number value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, key);
}

int lua_audio_sample_player_status(lua_State *L)
{
    lua_sample_player_t *s = check_sample_player(L);
    audio_sample_player_status_t status;
    audio_sample_player_get_status(s->handle, &status);
    lua_newtable(L);
    set_number(L, "active_voices", status.active_voices);
    set_number(L, "loaded_samples", status.loaded_samples);
    set_number(L, "sample_bytes", status.sample_bytes);
    set_number(L, "sample_budget_bytes", status.sample_budget_bytes);
    set_number(L, "render_max_us", status.render_max_us);
    set_number(L, "rendered_blocks", status.rendered_blocks);
    lua_pushstring(L, esp_err_to_name(status.error));
    lua_setfield(L, -2, "error");
    return 1;
}

static uint32_t config_integer(lua_State *L, const char *key, uint32_t default_value, uint32_t minimum, uint32_t maximum)
{
    lua_getfield(L, 1, key);
    lua_Integer value = lua_isnil(L, -1) ? default_value : luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (value < minimum || value > maximum) luaL_error(L, "audio sample player: invalid %s", key);
    return value;
}

int lua_audio_sample_player_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "output");
    audio_device_t *output = lua_audio_check_device(L, -1, AUDIO_DEVICE_OUTPUT, "sample_player");
    int output_idx = lua_gettop(L);
    if (output->holders == UINT8_MAX) return lua_audio_push_error(L, "audio sample player: too many output holders");
    audio_sample_player_config_t config = {
        .track = output->sink_handle,
        .source_rate = config_integer(L, "source_rate", 16000, 8000, 48000),
        .sample_budget_bytes = config_integer(L, "sample_budget_bytes", 2 * 1024 * 1024, 2, UINT32_MAX),
        .max_voices = config_integer(L, "max_voices", 6, 1, 16),
        .attack_ms = config_integer(L, "attack_ms", 0, 0, 1000),
        .release_ms = config_integer(L, "release_ms", 0, 0, 1000),
    };
    lua_getfield(L, 1, "limit_threshold");
    config.limit_threshold = (float)luaL_optnumber(L, -1, 0);
    lua_pop(L, 1);
    lua_sample_player_t *s = lua_newuserdata(L, sizeof(*s));
    memset(s, 0, sizeof(*s));
    s->output_ref = LUA_NOREF;
    luaL_getmetatable(L, AUDIO_SAMPLE_PLAYER_META);
    lua_setmetatable(L, -2);
    esp_err_t err = audio_sample_player_create(&config, &s->handle);
    if (err != ESP_OK) return sample_player_result(L, err);
    s->output = output;
    output->holders++;
    lua_pushvalue(L, output_idx);
    s->output_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 1;
}
