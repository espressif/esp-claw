/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cheap, allocation-free estimate of the number of LLM tokens a UTF-8 text
 * occupies. CJK characters (3-byte sequences, the vast majority of which are
 * Han ideographs / full-width punctuation) count as 1 token per character;
 * everything else counts as 1 token per 4 bytes/characters. This mirrors how
 * common tokenizers behave on mixed Chinese/English text and is deliberately
 * a heuristic for context budgeting, not a tokenizer.
 */
size_t claw_memory_estimate_tokens(const char *text);

#ifdef __cplusplus
}
#endif
