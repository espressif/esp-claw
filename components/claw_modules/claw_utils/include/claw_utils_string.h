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

size_t claw_utils_utf8_prefix_len(const char *text, size_t max_bytes);

#ifdef __cplusplus
}
#endif
