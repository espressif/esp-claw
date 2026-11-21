/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Explicit PSRAM-first allocation helpers for audio_hub. */

#include "audio_memory.h"

#include <stdint.h>

#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define AUDIO_MEM_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define AUDIO_MEM_DRAM_CAPS  (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static const char *TAG = "audio_memory";

void *audio_mem_aligned_alloc(size_t alignment, size_t size)
{
    void *ptr = heap_caps_aligned_alloc(alignment, size, AUDIO_MEM_PSRAM_CAPS);
    if (ptr) return ptr;
    ptr = heap_caps_aligned_alloc(alignment, size, AUDIO_MEM_DRAM_CAPS);
    if (ptr == NULL) ESP_LOGE(TAG, "aligned alloc failed size=%u", (unsigned)size);
    return ptr;
}

void audio_mem_free(void *ptr)
{
    heap_caps_free(ptr);
}

StreamBufferHandle_t audio_mem_stream_buffer_create(size_t size, size_t trigger_level)
{
    if (size == SIZE_MAX) {
        ESP_LOGE(TAG, "stream buffer size overflow");
        return NULL;
    }
    size_t storage_size = size + 1; /* StreamBuffer reserves one byte for full/empty state. */
    StreamBufferHandle_t buffer = xStreamBufferCreateWithCaps(storage_size, trigger_level, AUDIO_MEM_PSRAM_CAPS);
    if (buffer) return buffer;
    buffer = xStreamBufferCreateWithCaps(storage_size, trigger_level, AUDIO_MEM_DRAM_CAPS);
    if (buffer == NULL) ESP_LOGE(TAG, "stream buffer alloc failed size=%u", (unsigned)size);
    return buffer;
}

void audio_mem_stream_buffer_delete(StreamBufferHandle_t buffer)
{
    if (buffer) vStreamBufferDeleteWithCaps(buffer);
}
