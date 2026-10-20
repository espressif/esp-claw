/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

static const char *audio_player_state_name(audio_playback_state_t state)
{
    switch (state) {
    case AUDIO_PLAYER_PLAYING: return "playing";
    case AUDIO_PLAYER_PAUSED: return "paused";
    case AUDIO_PLAYER_STOPPED: return "stopped";
    case AUDIO_PLAYER_FINISHED: return "finished";
    case AUDIO_PLAYER_ERROR: return "error";
    default: return "idle";
    }
}

static esp_err_t audio_player_write_pcm(const void *data, size_t bytes, void *ctx)
{
    audio_player_t *player = (audio_player_t *)ctx;
    if (!player || player->closed || !player->output || player->output->closed) {
        ESP_LOGE(TAG, "Player write callback received invalid output");
        return ESP_ERR_INVALID_STATE;
    }
    return audio_device_write(player->output, data, bytes);
}

static void audio_player_event_cb(const audio_playback_status_t *status, void *ctx)
{
    audio_player_t *player = (audio_player_t *)ctx;
    if (!player || !status || !player->lock) {
        return;
    }

    bool release_output = false;
    xSemaphoreTake(player->lock, portMAX_DELAY);
    if (!player->closed) {
        player->state = status->state;
        player->music_info.sample_rate = (int)status->source_format.sample_rate;
        player->music_info.channels = status->source_format.channels;
        player->music_info.bits = status->source_format.bits;
        player->music_info.bitrate = (int)status->bitrate;
        player->has_music_info = status->source_format.sample_rate > 0 && status->source_format.channels > 0 && status->source_format.bits > 0;
        if (status->state == AUDIO_PLAYER_STOPPED || status->state == AUDIO_PLAYER_FINISHED || status->state == AUDIO_PLAYER_ERROR) {
            player->running = false;
            release_output = true;
        }
    }
    xSemaphoreGive(player->lock);
    if (release_output) {
        audio_device_release(player->output);
    }
}

int lua_audio_player_new(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "output");
    audio_device_t *output = lua_audio_check_device(L, -1, AUDIO_DEVICE_OUTPUT, "player");
    int output_idx = lua_gettop(L);

    audio_player_t *player = (audio_player_t *)lua_newuserdata(L, sizeof(*player));
    memset(player, 0, sizeof(*player));
    player->output = output;
    player->state = AUDIO_PLAYER_IDLE;
    player->lock = xSemaphoreCreateMutex();
    if (!player->lock) {
        ESP_LOGE(TAG, "Player mutex create failed");
        return lua_audio_push_error(L, "audio player: out of memory");
    }

    const audio_playback_config_t config = {
        .output_format = {.sample_rate = output->fmt.sample_rate, .channels = output->fmt.channels, .bits = output->fmt.bits},
        .write = audio_player_write_pcm,
        .write_ctx = player,
        .event = audio_player_event_cb,
        .event_ctx = player,
    };
    esp_err_t err = audio_playback_create(&config, &player->service);
    if (err != ESP_OK) {
        vSemaphoreDelete(player->lock);
        player->lock = NULL;
        ESP_LOGE(TAG, "Create audio player service failed: %s", esp_err_to_name(err));
        return lua_audio_push_errorf(L, "audio player: create failed (%s)", esp_err_to_name(err));
    }

    lua_pushvalue(L, output_idx);
    player->output_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    output->holders++;
    luaL_getmetatable(L, AUDIO_PLAYER_META);
    lua_setmetatable(L, -2);
    lua_remove(L, output_idx);
    return 1;
}

int lua_audio_player_close(lua_State *L)
{
    audio_player_t *player = (audio_player_t *)luaL_checkudata(L, 1, AUDIO_PLAYER_META);
    if (!player || player->closed) {
        lua_pushboolean(L, 1);
        return 1;
    }
    player->closed = true;
    if (player->service) {
        audio_playback_delete(player->service);
        player->service = NULL;
    }
    if (player->output) {
        audio_device_release(player->output);
        if (player->output->holders > 0) {
            player->output->holders--;
        }
        player->output = NULL;
    }
    if (player->output_ref != LUA_NOREF && player->output_ref != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, player->output_ref);
        player->output_ref = LUA_NOREF;
    }
    if (player->lock) {
        vSemaphoreDelete(player->lock);
        player->lock = NULL;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_gc(lua_State *L)
{
    return lua_audio_player_close(L);
}

int lua_audio_player_play(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "play");
    const char *path = luaL_checkstring(L, 2);
    bool wait_done = false;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "wait");
        wait_done = lua_toboolean(L, -1);
        lua_pop(L, 1);
    }

    if (!audio_device_acquire(player->output)) {
        return lua_audio_push_error(L, "audio player: output busy");
    }
    char *uri = audio_uri_from_path(path);
    if (!uri || !uri[0]) {
        audio_device_release(player->output);
        free(uri);
        return lua_audio_push_error(L, "audio player: invalid uri");
    }

    xSemaphoreTake(player->lock, portMAX_DELAY);
    player->running = true;
    player->state = AUDIO_PLAYER_IDLE;
    player->has_music_info = false;
    xSemaphoreGive(player->lock);

    esp_err_t err = audio_playback_play(player->service, uri, 0, wait_done);
    free(uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Player run failed: %s", esp_err_to_name(err));
        audio_device_release(player->output);
        player->running = false;
        return lua_audio_push_errorf(L, "audio player: play failed (%s)", esp_err_to_name(err));
    }
    if (wait_done) {
        audio_device_release(player->output);
        player->running = false;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_stop(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "stop");
    esp_err_t err = audio_playback_stop(player->service);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Player stop failed: %s", esp_err_to_name(err));
        return lua_audio_push_errorf(L, "audio player: stop failed (%s)", esp_err_to_name(err));
    }
    audio_device_release(player->output);
    player->running = false;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_pause(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "pause");
    esp_err_t err = audio_playback_pause(player->service);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Player pause failed: %s", esp_err_to_name(err));
        return lua_audio_push_errorf(L, "audio player: pause failed (%s)", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_resume(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "resume");
    esp_err_t err = audio_playback_resume(player->service);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Player resume failed: %s", esp_err_to_name(err));
        return lua_audio_push_errorf(L, "audio player: resume failed (%s)", esp_err_to_name(err));
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_poll(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "poll");
    audio_playback_status_t status = {0};
    esp_err_t err = audio_playback_get_status(player->service, &status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Player status refresh failed: %s", esp_err_to_name(err));
    } else {
        xSemaphoreTake(player->lock, portMAX_DELAY);
        player->state = status.state;
        player->music_info.sample_rate = (int)status.source_format.sample_rate;
        player->music_info.channels = status.source_format.channels;
        player->music_info.bits = status.source_format.bits;
        player->music_info.bitrate = (int)status.bitrate;
        player->has_music_info = status.source_format.sample_rate > 0 && status.source_format.channels > 0 && status.source_format.bits > 0;
        player->running = status.state == AUDIO_PLAYER_PLAYING || status.state == AUDIO_PLAYER_PAUSED;
        xSemaphoreGive(player->lock);
    }

    xSemaphoreTake(player->lock, portMAX_DELAY);
    lua_newtable(L);
    lua_pushstring(L, audio_player_state_name(player->state));
    lua_setfield(L, -2, "state");
    lua_pushboolean(L, player->running);
    lua_setfield(L, -2, "running");
    if (player->has_music_info) {
        lua_newtable(L);
        lua_pushinteger(L, player->music_info.sample_rate);
        lua_setfield(L, -2, "sample_rate");
        lua_pushinteger(L, player->music_info.channels);
        lua_setfield(L, -2, "channels");
        lua_pushinteger(L, player->music_info.bits);
        lua_setfield(L, -2, "bits");
        lua_pushinteger(L, player->music_info.bitrate);
        lua_setfield(L, -2, "bitrate");
        lua_setfield(L, -2, "music_info");
    }
    xSemaphoreGive(player->lock);
    return 1;
}
