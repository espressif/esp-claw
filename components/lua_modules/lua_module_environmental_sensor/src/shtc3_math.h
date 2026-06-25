/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint8_t shtc3_crc8(const uint8_t *data, size_t len);
float shtc3_raw_to_celsius(uint16_t raw);
float shtc3_raw_to_humidity(uint16_t raw);

#ifdef __cplusplus
}
#endif
