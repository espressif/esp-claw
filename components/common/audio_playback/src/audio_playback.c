/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */

#include "audio_playback.h"

#include <stdlib.h>

#include "esp_audio_dec_default.h"
#include "esp_audio_render.h"
#include "esp_extractor_defaults.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_log.h"
#include "esp_player.h"
#include "media_lib_adapter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "audio_player_svc";

struct audio_playback_t {
    esp_player_handle_t player;
    esp_audio_render_handle_t render;
    esp_audio_render_stream_handle_t render_stream;
    esp_gmf_pool_handle_t render_pool;
    SemaphoreHandle_t lock;
    audio_playback_config_t config;
    audio_playback_status_t status;
    bool deleting;
};

static bool s_extractors_registered;
static bool s_audio_decoders_registered;
static StaticSemaphore_t s_global_init_lock_storage;
static SemaphoreHandle_t s_global_init_lock;
static portMUX_TYPE s_global_init_guard = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t global_init_lock_get(void)
{
    taskENTER_CRITICAL(&s_global_init_guard);
    if (!s_global_init_lock) {
        s_global_init_lock = xSemaphoreCreateMutexStatic(&s_global_init_lock_storage);
    }
    SemaphoreHandle_t lock = s_global_init_lock;
    taskEXIT_CRITICAL(&s_global_init_guard);
    return lock;
}

static esp_err_t ensure_global_media_defaults(void)
{
    SemaphoreHandle_t lock = global_init_lock_get();
    if (!lock) {
        ESP_LOGE(TAG, "Create media defaults lock failed");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(lock, portMAX_DELAY);

    esp_err_t ret = ESP_OK;
    if (media_lib_add_default_adapter() != ESP_OK) {
        ESP_LOGE(TAG, "Register media adapters failed");
        ret = ESP_FAIL;
        goto done;
    }
    if (!s_extractors_registered) {
        const esp_extractor_err_t err = esp_extractor_register_default();
        if (err != ESP_EXTRACTOR_ERR_OK && err != ESP_EXTRACTOR_ERR_ALREADY_EXIST) {
            ESP_LOGE(TAG, "Register extractors failed: %d", err);
            ret = ESP_FAIL;
            goto done;
        }
        s_extractors_registered = true;
    }
    if (!s_audio_decoders_registered) {
        const esp_audio_err_t err = esp_audio_dec_register_default();
        if (err != ESP_AUDIO_ERR_OK && err != ESP_AUDIO_ERR_ALREADY_EXIST) {
            ESP_LOGE(TAG, "Register audio decoders failed: %d", err);
            ret = ESP_FAIL;
            goto done;
        }
        s_audio_decoders_registered = true;
    }

done:
    xSemaphoreGive(lock);
    return ret;
}

static bool format_valid(const audio_playback_format_t *format)
{
    return format && format->sample_rate && format->channels && format->bits &&
        (format->bits % 8U) == 0;
}

static esp_err_t player_err_to_esp(esp_player_err_t err)
{
    switch (err) {
    case ESP_PLAYER_ERR_OK: return ESP_OK;
    case ESP_PLAYER_ERR_INVALID_ARG: return ESP_ERR_INVALID_ARG;
    case ESP_PLAYER_ERR_NO_MEM: return ESP_ERR_NO_MEM;
    case ESP_PLAYER_ERR_TIMEOUT: return ESP_ERR_TIMEOUT;
    case ESP_PLAYER_ERR_NOT_SUPPORT: return ESP_ERR_NOT_SUPPORTED;
    case ESP_PLAYER_ERR_INVALID_STATE: return ESP_ERR_INVALID_STATE;
    default: return ESP_FAIL;
    }
}

static audio_playback_state_t translate_state(esp_player_state_t state)
{
    switch (state) {
    case ESP_PLAYER_STATE_PREPARING:
    case ESP_PLAYER_STATE_PLAYING: return AUDIO_PLAYER_PLAYING;
    case ESP_PLAYER_STATE_PAUSED: return AUDIO_PLAYER_PAUSED;
    case ESP_PLAYER_STATE_STOPPED: return AUDIO_PLAYER_STOPPED;
    case ESP_PLAYER_STATE_FINISHED: return AUDIO_PLAYER_FINISHED;
    case ESP_PLAYER_STATE_ERROR: return AUDIO_PLAYER_ERROR;
    default: return AUDIO_PLAYER_IDLE;
    }
}

static int render_writer(uint8_t *pcm, uint32_t bytes, void *ctx)
{
    audio_playback_handle_t service = ctx;
    if (!service || !pcm || !bytes) {
        return -1;
    }
    return service->config.write(pcm, bytes, service->config.write_ctx) ==
            ESP_OK ? 0 : -1;
}

static esp_err_t create_render_pool(esp_gmf_pool_handle_t *ret_pool)
{
    if (esp_gmf_pool_init(ret_pool) != ESP_GMF_ERR_OK || !*ret_pool) {
        return ESP_ERR_NO_MEM;
    }

    esp_gmf_element_handle_t element = NULL;
    esp_ae_ch_cvt_cfg_t channel = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&channel, &element) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_register_element(*ret_pool, element, NULL) !=
            ESP_GMF_ERR_OK) {
        goto fail;
    }
    esp_ae_bit_cvt_cfg_t bits = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    if (esp_gmf_bit_cvt_init(&bits, &element) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_register_element(*ret_pool, element, NULL) !=
            ESP_GMF_ERR_OK) {
        goto fail;
    }
    esp_ae_rate_cvt_cfg_t rate = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    if (esp_gmf_rate_cvt_init(&rate, &element) != ESP_GMF_ERR_OK ||
        esp_gmf_pool_register_element(*ret_pool, element, NULL) !=
            ESP_GMF_ERR_OK) {
        goto fail;
    }
    return ESP_OK;

fail:
    esp_gmf_pool_deinit(*ret_pool);
    *ret_pool = NULL;
    return ESP_FAIL;
}

static void refresh_status(audio_playback_handle_t service)
{
    esp_player_state_t state = ESP_PLAYER_STATE_IDLE;
    uint64_t position = 0;
    uint64_t duration = 0;
    esp_player_track_info_t track = {0};
    const bool have_state = esp_player_get_state(service->player, &state) ==
        ESP_PLAYER_ERR_OK;
    const bool have_position = esp_player_get_play_time(
        service->player, &position) == ESP_PLAYER_ERR_OK;
    const bool have_duration = esp_player_get_duration(
        service->player, &duration) == ESP_PLAYER_ERR_OK;
    const bool have_track = esp_player_get_track_info(service->player,
        ESP_PLAYER_TRACK_TYPE_AUDIO, 0, &track) == ESP_PLAYER_ERR_OK;

    xSemaphoreTake(service->lock, portMAX_DELAY);
    if (have_state) {
        service->status.state = translate_state(state);
    }
    if (have_position) {
        service->status.position_ms = position > INT64_MAX
            ? INT64_MAX : (int64_t)position;
    }
    if (have_duration) {
        service->status.duration_ms = duration > INT64_MAX
            ? INT64_MAX : (int64_t)duration;
    }
    if (have_track && track.track_type == ESP_PLAYER_TRACK_TYPE_AUDIO) {
        service->status.source_format = (audio_playback_format_t) {
            .sample_rate = track.audio_info.sample_rate,
            .channels = track.audio_info.channels,
            .bits = track.audio_info.bits_per_sample,
        };
        service->status.bitrate = track.audio_info.bitrate;
    }
    xSemaphoreGive(service->lock);
}

static esp_player_err_t on_player_event(esp_player_event_msg_t *message,
    void *ctx)
{
    audio_playback_handle_t service = ctx;
    if (!service || !message) {
        return ESP_PLAYER_ERR_INVALID_ARG;
    }

    bool changed = true;
    xSemaphoreTake(service->lock, portMAX_DELAY);
    switch (message->event_type) {
    case ESP_PLAYER_EVENT_PLAYED:
        service->status.state = AUDIO_PLAYER_PLAYING;
        break;
    case ESP_PLAYER_EVENT_PAUSED:
        service->status.state = AUDIO_PLAYER_PAUSED;
        break;
    case ESP_PLAYER_EVENT_STOPPED:
        service->status.state = AUDIO_PLAYER_STOPPED;
        break;
    case ESP_PLAYER_EVENT_FINISHED:
        service->status.state = AUDIO_PLAYER_FINISHED;
        break;
    case ESP_PLAYER_EVENT_ERROR:
        service->status.state = AUDIO_PLAYER_ERROR;
        service->status.last_error = ESP_FAIL;
        break;
    default:
        changed = false;
        break;
    }
    const audio_playback_status_t status = service->status;
    audio_playback_event_cb_t event =
        changed && !service->deleting ? service->config.event : NULL;
    void *event_ctx = service->config.event_ctx;
    xSemaphoreGive(service->lock);

    if (event) {
        event(&status, event_ctx);
    }
    return ESP_PLAYER_ERR_OK;
}

static void destroy_service(audio_playback_handle_t service)
{
    if (service->player) {
        esp_player_deinit(service->player);
    }
    if (service->render) {
        esp_audio_render_destroy(service->render);
    }
    if (service->render_pool) {
        esp_gmf_pool_deinit(service->render_pool);
    }
    if (service->lock) {
        vSemaphoreDelete(service->lock);
    }
    free(service);
}

esp_err_t audio_playback_create(
    const audio_playback_config_t *config,
    audio_playback_handle_t *ret_service)
{
    if (!config || !ret_service || !config->write ||
        !format_valid(&config->output_format)) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_service = NULL;
    audio_playback_handle_t service = calloc(1, sizeof(*service));
    if (!service) {
        return ESP_ERR_NO_MEM;
    }
    service->config = *config;
    service->lock = xSemaphoreCreateMutex();
    if (!service->lock) {
        destroy_service(service);
        return ESP_ERR_NO_MEM;
    }

    if (ensure_global_media_defaults() != ESP_OK) {
        destroy_service(service);
        return ESP_FAIL;
    }
    if (create_render_pool(&service->render_pool) != ESP_OK) {
        destroy_service(service);
        return ESP_FAIL;
    }

    esp_audio_render_cfg_t render_config = {
        .max_stream_num = 1,
        .out_writer = render_writer,
        .out_ctx = service,
        .out_sample_info = {
            .sample_rate = config->output_format.sample_rate,
            .bits_per_sample = config->output_format.bits,
            .channel = config->output_format.channels,
        },
        .pool = service->render_pool,
        .process_period = 20,
    };
    if (esp_audio_render_create(&render_config, &service->render) !=
            ESP_AUDIO_RENDER_ERR_OK ||
        esp_audio_render_stream_get(service->render,
            ESP_AUDIO_RENDER_FIRST_STREAM, &service->render_stream) !=
            ESP_AUDIO_RENDER_ERR_OK) {
        destroy_service(service);
        return ESP_FAIL;
    }

    esp_player_config_t player_config = ESP_PLAYER_CONFIG_DEFAULT();
    player_config.audio_render_hd = service->render_stream;
    if (esp_player_init(&player_config, &service->player) !=
            ESP_PLAYER_ERR_OK ||
        esp_player_set_event_cb(service->player, on_player_event, service) !=
            ESP_PLAYER_ERR_OK) {
        destroy_service(service);
        return ESP_FAIL;
    }
    *ret_service = service;
    return ESP_OK;
}

void audio_playback_delete(audio_playback_handle_t service)
{
    if (!service) {
        return;
    }
    xSemaphoreTake(service->lock, portMAX_DELAY);
    service->deleting = true;
    xSemaphoreGive(service->lock);
    (void)esp_player_stop(service->player);
    destroy_service(service);
}

esp_err_t audio_playback_play(audio_playback_handle_t service,
    const char *uri, uint64_t total_bytes, bool wait_until_finished)
{
    (void)total_bytes;
    if (!service || !uri || !uri[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_state_t state = ESP_PLAYER_STATE_IDLE;
    if (esp_player_get_state(service->player, &state) == ESP_PLAYER_ERR_OK &&
        (state == ESP_PLAYER_STATE_PREPARING ||
         state == ESP_PLAYER_STATE_PLAYING ||
         state == ESP_PLAYER_STATE_PAUSED)) {
        esp_player_err_t stop_err = esp_player_stop(service->player);
        if (stop_err != ESP_PLAYER_ERR_OK) {
            return player_err_to_esp(stop_err);
        }
    }

    xSemaphoreTake(service->lock, portMAX_DELAY);
    service->status = (audio_playback_status_t) {
        .state = AUDIO_PLAYER_IDLE,
        .last_error = ESP_OK,
    };
    xSemaphoreGive(service->lock);

    esp_player_data_src_t source = ESP_PLAYER_DATA_SRC(
        uri, ESP_PLAYER_MASK_AUDIO);
    esp_player_err_t err = esp_player_set_data_src(service->player, &source);
    if (err == ESP_PLAYER_ERR_OK) {
        err = wait_until_finished ? esp_player_run_to_end(service->player)
                                  : esp_player_run(service->player);
    }
    if (err != ESP_PLAYER_ERR_OK) {
        xSemaphoreTake(service->lock, portMAX_DELAY);
        service->status.state = AUDIO_PLAYER_ERROR;
        service->status.last_error = player_err_to_esp(err);
        xSemaphoreGive(service->lock);
    }
    return player_err_to_esp(err);
}

esp_err_t audio_playback_stop(audio_playback_handle_t service)
{
    return service ? player_err_to_esp(esp_player_stop(service->player))
                   : ESP_ERR_INVALID_ARG;
}

esp_err_t audio_playback_pause(audio_playback_handle_t service)
{
    return service ? player_err_to_esp(esp_player_pause(service->player))
                   : ESP_ERR_INVALID_ARG;
}

esp_err_t audio_playback_resume(audio_playback_handle_t service)
{
    return service ? player_err_to_esp(esp_player_resume(service->player))
                   : ESP_ERR_INVALID_ARG;
}

esp_err_t audio_playback_get_status(
    audio_playback_handle_t service,
    audio_playback_status_t *out_status)
{
    if (!service || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    refresh_status(service);
    xSemaphoreTake(service->lock, portMAX_DELAY);
    *out_status = service->status;
    xSemaphoreGive(service->lock);
    return ESP_OK;
}

const char *audio_playback_state_name(audio_playback_state_t state)
{
    static const char *const names[] = {
        "idle", "running", "paused", "stopped", "finished", "error",
    };
    return (unsigned)state < sizeof(names) / sizeof(names[0])
        ? names[state] : "unknown";
}
