/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_utils_string.h"

#include <stdint.h>
#include <string.h>

size_t claw_utils_utf8_prefix_len(const char *text, size_t max_bytes)
{
    size_t keep;

    if (!text || max_bytes == 0) {
        return 0;
    }

    keep = strnlen(text, max_bytes);
    if (keep < max_bytes || text[keep] == '\0') {
        return keep;
    }

    while (keep > 0 && (((uint8_t)text[keep]) & 0xC0U) == 0x80U) {
        keep--;
    }

    return keep;
}
