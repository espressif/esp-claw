/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>

#include "ir_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware protocol tables; all of them are decodable and the air-conditioner ones carry their capabilities in meta. */
const ir_protocol_t *const *ir_builtin_protocols(size_t *count);
const ir_protocol_t *ir_builtin_find(const char *id);

#ifdef __cplusplus
}
#endif
