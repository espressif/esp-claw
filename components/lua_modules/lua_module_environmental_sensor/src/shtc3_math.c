/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "shtc3_math.h"

#define SHTC3_CRC8_INIT       0xff
#define SHTC3_CRC8_POLYNOMIAL 0x31
#define SHTC3_RAW_MAX         65535.0f

uint8_t shtc3_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = SHTC3_CRC8_INIT;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x80) != 0) {
                crc = (uint8_t)((crc << 1) ^ SHTC3_CRC8_POLYNOMIAL);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }

    return crc;
}

float shtc3_raw_to_celsius(uint16_t raw)
{
    return -45.0f + (175.0f * (float)raw / SHTC3_RAW_MAX);
}

float shtc3_raw_to_humidity(uint16_t raw)
{
    return 100.0f * (float)raw / SHTC3_RAW_MAX;
}
