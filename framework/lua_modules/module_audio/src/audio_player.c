/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio_private.h"

static esp_err_t audio_player_create_converter(audio_player_t *player, const audio_format_t *src)
{
    audio_converter_t next = {0};
    if (audio_converter_create(&next, src, &player->output->fmt) != ESP_OK) {
        return ESP_FAIL;
    }
    audio_converter_destroy(&player->converter);
    player->converter = next;
    player->converter_ready = true;
    return ESP_OK;
}

static bool audio_player_uri_is_https(const char *uri)
{
    static const char https_prefix[] = "https://";
    if (!uri) {
        return false;
    }
    for (size_t i = 0; i < sizeof(https_prefix) - 1; i++) {
        char c = uri[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != https_prefix[i]) {
            return false;
        }
    }
    return true;
}

static int audio_player_prev_cb(esp_asp_handle_t *handle, void *ctx)
{
    audio_player_t *player = (audio_player_t *)ctx;
    esp_gmf_pipeline_handle_t pipe = NULL;
    esp_gmf_io_handle_t in_io = NULL;
    esp_gmf_err_t ret;

    if (!handle || !player) {
        ESP_LOGE(TAG, "Player previous callback received invalid context");
        return ESP_GMF_ERR_INVALID_ARG;
    }
    if (!player->current_uri_https) {
        return ESP_GMF_ERR_OK;
    }

    /* Configure the cloned HTTP IO before pipeline open, otherwise ESP-TLS refuses HTTPS without server verification. */
    ret = esp_audio_simple_player_get_pipeline((esp_asp_handle_t)handle, &pipe);
    if (ret != ESP_GMF_ERR_OK || !pipe) {
        ESP_LOGE(TAG, "Player failed to get pipeline for HTTPS setup: ret=%d", ret);
        return ret == ESP_GMF_ERR_OK ? ESP_GMF_ERR_FAIL : ret;
    }
    ret = esp_gmf_pipeline_get_in(pipe, &in_io);
    if (ret != ESP_GMF_ERR_OK || !in_io) {
        ESP_LOGE(TAG, "Player failed to get input IO for HTTPS setup: ret=%d", ret);
        return ret == ESP_GMF_ERR_OK ? ESP_GMF_ERR_FAIL : ret;
    }
    if (strcmp(OBJ_GET_TAG(in_io), "io_http") != 0) {
        ESP_LOGE(TAG, "Player HTTPS URI resolved to unexpected IO: %s", OBJ_GET_TAG(in_io));
        return ESP_GMF_ERR_NOT_SUPPORT;
    }

#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    http_io_cfg_t *http_cfg = (http_io_cfg_t *)OBJ_GET_CFG(in_io);
    if (!http_cfg) {
        ESP_LOGE(TAG, "Player HTTP IO config missing for HTTPS setup");
        return ESP_GMF_ERR_FAIL;
    }
    http_cfg->crt_bundle_attach = esp_crt_bundle_attach;
    return ESP_GMF_ERR_OK;
#else
    ESP_LOGE(TAG, "HTTPS playback requires CONFIG_MBEDTLS_CERTIFICATE_BUNDLE");
    return ESP_GMF_ERR_NOT_SUPPORT;
#endif
}

static int audio_player_out_cb(uint8_t *data, int data_size, void *ctx)
{
    audio_player_t *player = (audio_player_t *)ctx;
    uint8_t *out = NULL;
    uint32_t out_bytes = 0;
    int ret = 0;

    if (!player || player->closed || !player->output || player->output->closed) {
        ESP_LOGE(TAG, "Player output callback on invalid device");
        return 0;
    }

    xSemaphoreTake(player->lock, portMAX_DELAY);
    if (!player->converter_ready) {
        /* Some streams may output data before reporting info; assume codec-ready format and let ASRC bypass. */
        if (audio_player_create_converter(player, &player->output->fmt) != ESP_OK) {
            xSemaphoreGive(player->lock);
            return 0;
        }
    }
    if (audio_converter_process(&player->converter, data, (uint32_t)data_size, &out, &out_bytes) == ESP_OK) {
        ret = (esp_codec_dev_write(player->output->codec_dev, out, (int)out_bytes) == ESP_CODEC_DEV_OK) ? data_size : 0;
    }
    xSemaphoreGive(player->lock);
    if (ret == 0) {
        ESP_LOGE(TAG, "Player output callback failed");
    }
    return ret;
}

static int audio_player_event_cb(esp_asp_event_pkt_t *pkt, void *ctx)
{
    audio_player_t *player = (audio_player_t *)ctx;
    if (!player || !pkt) {
        return 0;
    }

    xSemaphoreTake(player->lock, portMAX_DELAY);
    if (pkt->type == ESP_ASP_EVENT_TYPE_STATE && pkt->payload_size == sizeof(esp_asp_state_t)) {
        player->state = *(esp_asp_state_t *)pkt->payload;
        if (player->state == ESP_ASP_STATE_STOPPED || player->state == ESP_ASP_STATE_FINISHED || player->state == ESP_ASP_STATE_ERROR) {
            player->running = false;
            audio_device_release(player->output);
        }
    } else if (pkt->type == ESP_ASP_EVENT_TYPE_MUSIC_INFO && pkt->payload_size == sizeof(esp_asp_music_info_t)) {
        audio_format_t src = {0};
        player->music_info = *(esp_asp_music_info_t *)pkt->payload;
        player->has_music_info = true;
        src.sample_rate = (uint32_t)player->music_info.sample_rate;
        src.channels = (uint8_t)player->music_info.channels;
        src.bits = (uint8_t)player->music_info.bits;
        if (audio_format_complete(&src) == ESP_OK && audio_player_create_converter(player, &src) != ESP_OK) {
            ESP_LOGE(TAG, "Player failed to create ASRC converter from music info");
        }
    }
    xSemaphoreGive(player->lock);
    return 0;
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
    player->state = ESP_ASP_STATE_NONE;
    player->lock = xSemaphoreCreateMutex();
    if (!player->lock) {
        ESP_LOGE(TAG, "Player mutex create failed");
        return lua_audio_push_error(L, "audio player: out of memory");
    }

    esp_asp_cfg_t cfg = {
        .out = {
            .cb = audio_player_out_cb,
            .user_ctx = player,
        },
        .task_prio = 0,
        .task_stack = 0,
        .task_core = 0,
        .task_stack_in_ext = false,
        .prev = audio_player_prev_cb,
        .prev_ctx = player,
    };
    if (esp_audio_simple_player_new(&cfg, &player->asp) != ESP_GMF_ERR_OK) {
        vSemaphoreDelete(player->lock);
        ESP_LOGE(TAG, "Create audio simple player failed");
        return lua_audio_push_error(L, "audio player: create failed");
    }
    esp_audio_simple_player_set_event(player->asp, audio_player_event_cb, player);

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
    if (player->running && player->asp) {
        esp_audio_simple_player_stop(player->asp);
        audio_device_release(player->output);
        player->running = false;
    }
    if (player->asp) {
        esp_audio_simple_player_destroy(player->asp);
        player->asp = NULL;
    }
    audio_converter_destroy(&player->converter);
    if (player->output) {
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
    player->closed = true;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_gc(lua_State *L)
{
    return lua_audio_player_close(L);
}

static void lua_audio_parse_music_info(lua_State *L, int idx, esp_asp_music_info_t *info, bool *has_info)
{
    *has_info = false;
    memset(info, 0, sizeof(*info));
    if (lua_isnoneornil(L, idx)) {
        return;
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    info->sample_rate = (int)lua_audio_get_u32_field(L, idx, "sample_rate", 0, 0);
    info->channels = lua_audio_get_u8_field(L, idx, "channels", 0, 0);
    info->bits = lua_audio_get_u8_field(L, idx, "bits", 0, 0);
    lua_getfield(L, idx, "bitrate");
    info->bitrate = lua_isnil(L, -1) ? 0 : (int)luaL_checkinteger(L, -1);
    lua_pop(L, 1);
    if (info->sample_rate > 0 && info->channels > 0 && info->bits > 0) {
        *has_info = true;
    }
}

int lua_audio_player_play(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "play");
    const char *path = luaL_checkstring(L, 2);
    bool wait_done = false;
    bool has_music_info = false;
    esp_asp_music_info_t music_info = {0};
    char *uri = NULL;
    esp_gmf_err_t ret;

    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_getfield(L, 3, "wait");
        wait_done = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 3, "music_info");
        lua_audio_parse_music_info(L, lua_gettop(L), &music_info, &has_music_info);
        lua_pop(L, 1);
    }

    if (!audio_device_acquire(player->output)) {
        return lua_audio_push_error(L, "audio player: output busy");
    }
    uri = audio_uri_from_path(path);
    if (!uri || !uri[0]) {
        audio_device_release(player->output);
        free(uri);
        return lua_audio_push_error(L, "audio player: invalid uri");
    }

    xSemaphoreTake(player->lock, portMAX_DELAY);
    player->running = true;
    player->current_uri_https = audio_player_uri_is_https(uri);
    player->state = ESP_ASP_STATE_NONE;
    player->has_music_info = false;
    player->converter_ready = false;
    audio_converter_destroy(&player->converter);
    xSemaphoreGive(player->lock);

    ret = wait_done ? esp_audio_simple_player_run_to_end(player->asp, uri, has_music_info ? &music_info : NULL) :
                      esp_audio_simple_player_run(player->asp, uri, has_music_info ? &music_info : NULL);
    free(uri);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Player run failed: ret=%d", ret);
        audio_device_release(player->output);
        player->running = false;
        return lua_audio_push_errorf(L, "audio player: play failed (%d)", ret);
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
    esp_gmf_err_t ret = esp_audio_simple_player_stop(player->asp);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Player stop failed: ret=%d", ret);
        return lua_audio_push_errorf(L, "audio player: stop failed (%d)", ret);
    }
    audio_device_release(player->output);
    player->running = false;
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_pause(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "pause");
    esp_gmf_err_t ret = esp_audio_simple_player_pause(player->asp);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Player pause failed: ret=%d", ret);
        return lua_audio_push_errorf(L, "audio player: pause failed (%d)", ret);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_resume(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "resume");
    esp_gmf_err_t ret = esp_audio_simple_player_resume(player->asp);
    if (ret != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "Player resume failed: ret=%d", ret);
        return lua_audio_push_errorf(L, "audio player: resume failed (%d)", ret);
    }
    lua_pushboolean(L, 1);
    return 1;
}

int lua_audio_player_poll(lua_State *L)
{
    audio_player_t *player = lua_audio_check_player(L, 1, "poll");
    xSemaphoreTake(player->lock, portMAX_DELAY);
    lua_newtable(L);
    lua_pushstring(L, esp_audio_simple_player_state_to_str(player->state));
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
