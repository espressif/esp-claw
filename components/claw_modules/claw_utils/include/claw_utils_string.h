/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

char *claw_utils_string_dup(const char *src);
char *claw_utils_string_dup_vprintf(const char *fmt, va_list args);
char *claw_utils_string_dup_printf(const char *fmt, ...);
size_t claw_utils_utf8_prefix_len(const char *text, size_t max_bytes);

#ifdef __cplusplus
}
#endif
