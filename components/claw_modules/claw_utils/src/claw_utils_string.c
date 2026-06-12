/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_utils_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *claw_utils_string_dup(const char *src)
{
    if (!src) {
        return NULL;
    }

    return strdup(src);
}

char *claw_utils_string_dup_vprintf(const char *fmt, va_list args)
{
    va_list copy;
    int needed;
    char *buf;

    if (!fmt) {
        return NULL;
    }

    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        return NULL;
    }

    buf = calloc(1, (size_t)needed + 1);
    if (!buf) {
        return NULL;
    }

    vsnprintf(buf, (size_t)needed + 1, fmt, args);
    return buf;
}

char *claw_utils_string_dup_printf(const char *fmt, ...)
{
    va_list args;
    char *buf;

    va_start(args, fmt);
    buf = claw_utils_string_dup_vprintf(fmt, args);
    va_end(args);
    return buf;
}

size_t claw_utils_utf8_prefix_len(const char *text, size_t max_bytes)
{
    const unsigned char *p = (const unsigned char *)text;
    size_t off = 0;

    if (!text) {
        return 0;
    }

    while (p[off] && off < max_bytes) {
        unsigned char ch = p[off];
        size_t char_len;

        if ((ch & 0x80) == 0) {
            char_len = 1;
        } else if ((ch & 0xE0) == 0xC0) {
            char_len = 2;
        } else if ((ch & 0xF0) == 0xE0) {
            char_len = 3;
        } else if ((ch & 0xF8) == 0xF0) {
            char_len = 4;
        } else {
            break;
        }

        if (off + char_len > max_bytes) {
            break;
        }
        for (size_t i = 1; i < char_len; i++) {
            if ((p[off + i] & 0xC0) != 0x80) {
                return off;
            }
        }
        off += char_len;
    }

    return off;
}
