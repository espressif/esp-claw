/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file audio_memory.h
 * @brief PSRAM-first allocation helpers for audio service buffers.
 */

#pragma once

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

/**
 * @brief Allocates an aligned audio buffer in PSRAM, then internal DRAM.
 *
 * @param alignment Required alignment in bytes.
 * @param size Buffer size in bytes.
 * @return Allocated buffer, or NULL when both heaps are exhausted.
 */
void *audio_mem_aligned_alloc(size_t alignment, size_t size);

/**
 * @brief Frees a buffer returned by audio_mem_aligned_alloc().
 *
 * @param ptr Buffer to free; NULL is allowed.
 */
void audio_mem_free(void *ptr);

/**
 * @brief Creates a stream buffer in PSRAM, then internal DRAM.
 *
 * @param size Usable stream buffer capacity in bytes.
 * @param trigger_level Minimum bytes required to unblock a reader.
 * @return Stream buffer handle, or NULL when both heaps are exhausted.
 */
StreamBufferHandle_t audio_mem_stream_buffer_create(size_t size, size_t trigger_level);

/**
 * @brief Deletes a stream buffer created by audio_mem_stream_buffer_create().
 *
 * @param buffer Stream buffer handle; NULL is allowed.
 */
void audio_mem_stream_buffer_delete(StreamBufferHandle_t buffer);
