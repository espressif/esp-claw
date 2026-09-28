/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "claw_memory_token.h"

size_t claw_memory_estimate_tokens(const char *text)
{
    size_t cjk_chars = 0;
    size_t other_chars = 0;
    const unsigned char *p = (const unsigned char *)text;

    if (!text) {
        return 0;
    }

    while (*p) {
        unsigned char c = *p;

        if (c < 0x80) {
            /* ASCII / 1-byte: roughly 4 characters per token. */
            other_chars++;
            p++;
        } else if ((c & 0xE0) == 0xC0) {
            /* 2-byte sequence (Latin-1 supplement, Cyrillic, etc.). */
            other_chars++;
            p += 2;
        } else if ((c & 0xF0) == 0xE0) {
            /* 3-byte sequence: almost exclusively CJK ideographs and
             * full-width punctuation, which map ~1:1 to tokens. */
            cjk_chars++;
            p += 3;
        } else {
            /* 4-byte sequence (emoji, rare scripts). */
            other_chars++;
            p += 4;
        }
    }

    return cjk_chars + (other_chars + 3) / 4;
}
