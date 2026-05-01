/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cap_voice.h"
#include "cap_voice_priv.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "esp_board_manager.h"
#include "esp_codec_dev.h"
#include "app_config.h"
#include "claw_core_llm.h"

static const char *TAG = "cap_voice";

/* Configuration */
#define VOICE_BTN_GPIO          GPIO_NUM_0
#define VOICE_SAMPLE_RATE       16000
#define VOICE_CHANNELS          1
#define VOICE_BITS              16
#define VOICE_MAX_RECORD_SEC    10
#define VOICE_RECORD_BUF_SIZE   (VOICE_SAMPLE_RATE * (VOICE_BITS / 8) * VOICE_CHANNELS * VOICE_MAX_RECORD_SEC)
#define VOICE_CHUNK_BYTES       512
#define VOICE_TASK_STACK        (8 * 1024)
#define VOICE_TASK_PRIO         5
#define VOICE_DEBOUNCE_MS       50
#define WAV_HEADER_SIZE         44

/* SiliconFlow API key for STT (SenseVoice) - separate from Minimax LLM key */
#ifndef SILICONFLOW_API_KEY
#define SILICONFLOW_API_KEY     "sk-ksekhezlziqinpeshbzxnbvjxozehrtlotlhvtyxodlsxplg"
#endif

/* Voice state */
typedef enum {
    VOICE_STATE_IDLE = 0,
    VOICE_STATE_RECORDING,
    VOICE_STATE_PROCESSING,
} voice_state_t;

/* Events sent to voice task */
typedef enum {
    VOICE_EVT_BTN_PRESS = 0,
    VOICE_EVT_BTN_RELEASE,
} voice_evt_t;

static struct {
    esp_codec_dev_handle_t dac_dev;
    esp_codec_dev_handle_t adc_dev;
    QueueHandle_t evt_queue;
    TaskHandle_t task_handle;
    voice_state_t state;
    uint8_t *record_buf;       /* PSRAM buffer for PCM data */
    size_t record_len;         /* Bytes recorded so far */
    bool running;
    int64_t last_isr_time;
} s_voice;

/* WAV header builder */
static void voice_build_wav_header(uint8_t *hdr, uint32_t pcm_data_size)
{
    uint32_t sample_rate = VOICE_SAMPLE_RATE;
    uint16_t channels = VOICE_CHANNELS;
    uint16_t bits = VOICE_BITS;
    uint16_t bytes_per_sample = bits / 8;
    uint32_t byte_rate = sample_rate * channels * bytes_per_sample;
    uint16_t block_align = channels * bytes_per_sample;

    memcpy(hdr + 0,  "RIFF", 4);
    hdr[4] = (pcm_data_size + 36) & 0xFF;
    hdr[5] = ((pcm_data_size + 36) >> 8) & 0xFF;
    hdr[6] = ((pcm_data_size + 36) >> 16) & 0xFF;
    hdr[7] = ((pcm_data_size + 36) >> 24) & 0xFF;
    memcpy(hdr + 8,  "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0; /* fmt chunk size */
    hdr[20] = 1; hdr[21] = 0; /* PCM format */
    hdr[22] = channels & 0xFF; hdr[23] = (channels >> 8) & 0xFF;
    hdr[24] = sample_rate & 0xFF; hdr[25] = (sample_rate >> 8) & 0xFF;
    hdr[26] = (sample_rate >> 16) & 0xFF; hdr[27] = (sample_rate >> 24) & 0xFF;
    hdr[28] = byte_rate & 0xFF; hdr[29] = (byte_rate >> 8) & 0xFF;
    hdr[30] = (byte_rate >> 16) & 0xFF; hdr[31] = (byte_rate >> 24) & 0xFF;
    hdr[32] = block_align & 0xFF; hdr[33] = (block_align >> 8) & 0xFF;
    hdr[34] = bits & 0xFF; hdr[35] = (bits >> 8) & 0xFF;
    memcpy(hdr + 36, "data", 4);
    hdr[40] = pcm_data_size & 0xFF;
    hdr[41] = (pcm_data_size >> 8) & 0xFF;
    hdr[42] = (pcm_data_size >> 16) & 0xFF;
    hdr[43] = (pcm_data_size >> 24) & 0xFF;
}

/* Open both codec devices once (called from init).
 * Tolerates "already open" (error 262) since board manager or
 * lua_module_audio may have opened the device earlier. */
static esp_err_t voice_open_codecs(void)
{
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = VOICE_SAMPLE_RATE,
        .channel = VOICE_CHANNELS,
        .bits_per_sample = VOICE_BITS,
    };
    int ret;

    ret = esp_codec_dev_open(s_voice.dac_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        if (ret == 262) {
            ESP_LOGW(TAG, "DAC already open (ret=%d), reusing", ret);
        } else {
            ESP_LOGE(TAG, "Failed to open DAC: %d", ret);
            return ESP_FAIL;
        }
    }
    esp_codec_dev_set_out_vol(s_voice.dac_dev, 80);

    ret = esp_codec_dev_open(s_voice.adc_dev, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        if (ret == 262) {
            ESP_LOGW(TAG, "ADC already open (ret=%d), reusing", ret);
        } else {
            ESP_LOGE(TAG, "Failed to open ADC: %d", ret);
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Audio codecs ready (DAC + ADC, 16kHz/16bit/mono)");
    return ESP_OK;
}

/* Play a short beep tone through the speaker (codec already open) */
static void voice_play_tone(uint16_t freq_hz, uint32_t duration_ms)
{
    uint32_t total_samples = (VOICE_SAMPLE_RATE * duration_ms) / 1000;
    int16_t tone_buf[256];
    uint32_t samples_written = 0;

    while (samples_written < total_samples) {
        uint32_t chunk = total_samples - samples_written;
        if (chunk > 256) chunk = 256;
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t period = VOICE_SAMPLE_RATE / freq_hz;
            tone_buf[i] = ((samples_written + i) % period < period / 2) ? 4000 : -4000;
        }
        esp_codec_dev_write(s_voice.dac_dev, tone_buf, chunk * sizeof(int16_t));
        samples_written += chunk;
    }
}

/* Start recording from microphone (codec already open) */
static esp_err_t voice_start_recording(void)
{
    s_voice.record_len = 0;
    ESP_LOGI(TAG, "Recording started...");
    s_voice.state = VOICE_STATE_RECORDING;
    return ESP_OK;
}

/* Stop recording */
static void voice_stop_recording(void)
{
    s_voice.state = VOICE_STATE_PROCESSING;
    ESP_LOGI(TAG, "Recording stopped. %zu bytes captured (%.1f sec)",
             s_voice.record_len,
             (float)s_voice.record_len / (VOICE_SAMPLE_RATE * VOICE_CHANNELS * (VOICE_BITS / 8)));
}

/* Read audio data during recording */
static void voice_record_chunk(void)
{
    if (s_voice.record_len + VOICE_CHUNK_BYTES > VOICE_RECORD_BUF_SIZE) {
        ESP_LOGW(TAG, "Record buffer full, stopping");
        voice_stop_recording();
        return;
    }
    int ret = esp_codec_dev_read(s_voice.adc_dev,
                                 s_voice.record_buf + s_voice.record_len,
                                 VOICE_CHUNK_BYTES);
    if (ret == ESP_CODEC_DEV_OK) {
        s_voice.record_len += VOICE_CHUNK_BYTES;
    }
}

/* Play raw PCM data through speaker (codec already open) */
static void voice_play_pcm(const uint8_t *pcm, size_t len)
{
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > 1024) chunk = 1024;
        esp_codec_dev_write(s_voice.dac_dev, (void *)(pcm + offset), chunk);
        offset += chunk;
    }
}

/* Process recorded audio: STT → LLM → TTS → Play */
static void voice_process_recording(void)
{
    if (s_voice.record_len < VOICE_CHUNK_BYTES * 4) {
        ESP_LOGW(TAG, "Recording too short, ignoring");
        s_voice.state = VOICE_STATE_IDLE;
        return;
    }

    /* Build WAV in memory */
    size_t wav_size = WAV_HEADER_SIZE + s_voice.record_len;
    uint8_t *wav_buf = heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
    if (!wav_buf) {
        ESP_LOGE(TAG, "Failed to alloc WAV buffer");
        s_voice.state = VOICE_STATE_IDLE;
        return;
    }
    voice_build_wav_header(wav_buf, s_voice.record_len);
    memcpy(wav_buf + WAV_HEADER_SIZE, s_voice.record_buf, s_voice.record_len);

    /* Get API key from config */
    app_config_t config;
    app_config_load(&config);
    const char *api_key = config.llm_api_key;
    const char *base_url = config.llm_base_url;

    /* Step 1: STT (uses SiliconFlow API key, not Minimax) */
    ESP_LOGI(TAG, "Step 1/3: Speech-to-Text...");
    char *stt_text = NULL;
    const char *stt_key = SILICONFLOW_API_KEY;
    if (!stt_key[0]) {
        ESP_LOGE(TAG, "SiliconFlow API key not configured!");
        free(wav_buf);
        voice_play_tone(200, 300);
        s_voice.state = VOICE_STATE_IDLE;
        return;
    }
    esp_err_t err = cap_voice_stt_transcribe(wav_buf, wav_size, stt_key, NULL, &stt_text);
    free(wav_buf);

    if (err != ESP_OK || !stt_text || stt_text[0] == '\0') {
        ESP_LOGE(TAG, "STT failed: %s", esp_err_to_name(err));
        voice_play_tone(200, 300); /* Error tone */
        free(stt_text);
        s_voice.state = VOICE_STATE_IDLE;
        return;
    }
    ESP_LOGI(TAG, "STT result: \"%s\"", stt_text);

    /* Step 2: LLM */
    ESP_LOGI(TAG, "Step 2/3: LLM thinking...");
    char *llm_response = NULL;
    char *llm_error = NULL;
    err = claw_core_llm_chat(
        "你是一个友好的语音助手，请用简洁的中文回答，回答尽量控制在100字以内。",
        stt_text, &llm_response, &llm_error);
    free(stt_text);

    if (err != ESP_OK || !llm_response) {
        ESP_LOGE(TAG, "LLM failed: %s", llm_error ? llm_error : esp_err_to_name(err));
        voice_play_tone(200, 300);
        free(llm_response);
        free(llm_error);
        s_voice.state = VOICE_STATE_IDLE;
        return;
    }
    free(llm_error);
    ESP_LOGI(TAG, "LLM response: \"%s\"", llm_response);

    /* Step 3: TTS */
    ESP_LOGI(TAG, "Step 3/3: Text-to-Speech...");
    uint8_t *tts_pcm = NULL;
    size_t tts_pcm_len = 0;
    err = cap_voice_tts_synthesize(llm_response, api_key, base_url, &tts_pcm, &tts_pcm_len);
    free(llm_response);

    if (err != ESP_OK || !tts_pcm || tts_pcm_len == 0) {
        ESP_LOGE(TAG, "TTS failed: %s", esp_err_to_name(err));
        voice_play_tone(200, 300);
        free(tts_pcm);
        s_voice.state = VOICE_STATE_IDLE;
        return;
    }

    /* Step 4: Play audio */
    ESP_LOGI(TAG, "Playing TTS audio (%zu bytes, %.1f sec)...",
             tts_pcm_len, (float)tts_pcm_len / (VOICE_SAMPLE_RATE * 2));
    voice_play_pcm(tts_pcm, tts_pcm_len);
    free(tts_pcm);

    ESP_LOGI(TAG, "Voice interaction complete");
    s_voice.state = VOICE_STATE_IDLE;
}

/* GPIO ISR handler */
static void IRAM_ATTR voice_btn_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if (now - s_voice.last_isr_time < VOICE_DEBOUNCE_MS * 1000) {
        return;
    }
    s_voice.last_isr_time = now;

    int level = gpio_get_level(VOICE_BTN_GPIO);
    voice_evt_t evt = (level == 0) ? VOICE_EVT_BTN_PRESS : VOICE_EVT_BTN_RELEASE;

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_voice.evt_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/* Main voice task */
static void voice_task(void *arg)
{
    voice_evt_t evt;

    ESP_LOGI(TAG, "Voice task started. Press GPIO0 to talk.");

    while (s_voice.running) {
        if (s_voice.state == VOICE_STATE_RECORDING) {
            /* While recording, read audio and check for button release */
            if (xQueueReceive(s_voice.evt_queue, &evt, pdMS_TO_TICKS(10)) == pdTRUE) {
                if (evt == VOICE_EVT_BTN_RELEASE) {
                    voice_stop_recording();
                    voice_play_tone(800, 100); /* Stop beep */
                    voice_process_recording();
                }
            } else {
                voice_record_chunk();
            }
        } else if (s_voice.state == VOICE_STATE_IDLE) {
            /* Wait for button press */
            if (xQueueReceive(s_voice.evt_queue, &evt, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (evt == VOICE_EVT_BTN_PRESS) {
                    voice_play_tone(1000, 100); /* Start beep */
                    if (voice_start_recording() != ESP_OK) {
                        voice_play_tone(200, 300);
                    }
                }
            }
        } else {
            /* Processing state - just wait */
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    vTaskDelete(NULL);
}

esp_err_t cap_voice_init(void)
{
    /* Get audio device handles from board manager */
    esp_err_t err;

    err = esp_board_manager_get_device_handle("audio_dac", (void **)&s_voice.dac_dev);
    if (err != ESP_OK || !s_voice.dac_dev) {
        ESP_LOGE(TAG, "Failed to get audio_dac handle");
        return ESP_FAIL;
    }

    err = esp_board_manager_get_device_handle("audio_adc", (void **)&s_voice.adc_dev);
    if (err != ESP_OK || !s_voice.adc_dev) {
        ESP_LOGE(TAG, "Failed to get audio_adc handle");
        return ESP_FAIL;
    }

    /* Open both codec devices once and keep open */
    err = voice_open_codecs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open audio codecs");
        return ESP_FAIL;
    }

    /* Allocate record buffer in PSRAM */
    s_voice.record_buf = heap_caps_malloc(VOICE_RECORD_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_voice.record_buf) {
        ESP_LOGE(TAG, "Failed to alloc record buffer (%d bytes)", VOICE_RECORD_BUF_SIZE);
        return ESP_ERR_NO_MEM;
    }

    /* Setup GPIO0 button */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << VOICE_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_conf);

    /* Create event queue */
    s_voice.evt_queue = xQueueCreate(8, sizeof(voice_evt_t));
    if (!s_voice.evt_queue) {
        return ESP_ERR_NO_MEM;
    }

    /* Install GPIO ISR */
    gpio_install_isr_service(0);
    gpio_isr_handler_add(VOICE_BTN_GPIO, voice_btn_isr, NULL);

    ESP_LOGI(TAG, "Voice assistant initialized (GPIO%d press-to-talk)", VOICE_BTN_GPIO);
    return ESP_OK;
}

esp_err_t cap_voice_start(void)
{
    s_voice.running = true;
    s_voice.state = VOICE_STATE_IDLE;

    BaseType_t ret = xTaskCreatePinnedToCore(
        voice_task, "voice", VOICE_TASK_STACK, NULL,
        VOICE_TASK_PRIO, &s_voice.task_handle, 1);

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create voice task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Voice assistant started");
    return ESP_OK;
}

esp_err_t cap_voice_stop(void)
{
    s_voice.running = false;
    if (s_voice.task_handle) {
        vTaskDelay(pdMS_TO_TICKS(200));
        s_voice.task_handle = NULL;
    }
    gpio_isr_handler_remove(VOICE_BTN_GPIO);
    ESP_LOGI(TAG, "Voice assistant stopped");
    return ESP_OK;
}
