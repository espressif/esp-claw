/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdlib.h>
#include <stdint.h>

#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DMA 0
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_DEFAULT 0

static inline void *heap_caps_malloc(size_t size, int caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, int caps)
{
    (void)caps;
    return calloc(n, size);
}

static inline void heap_caps_free(void *ptr)
{
    free(ptr);
}

static inline void *heap_caps_aligned_alloc(size_t alignment, size_t size, int caps)
{
    (void)caps;
    if (!alignment || (alignment & (alignment - 1)) || size > SIZE_MAX - (alignment - 1)) return NULL;
    return aligned_alloc(alignment, (size + alignment - 1) & ~(alignment - 1));
}
