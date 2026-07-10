/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Logical power-controlled features on the M5Stack StickS3.
 *
 * The on-board M5PM1 PMIC exposes 5 GPIO-style channels (PYG0..PYG4).
 * Only the rails that gate independent peripherals are enumerated here.
 */
typedef enum {
    M5_STICKS3_PWR_LCD,        /*!< PYG2 - LCD/L3B power enable        */
    M5_STICKS3_PWR_SPEAKER,    /*!< PYG3 - Speaker amplifier enable    */
} m5_sticks3_power_feature_t;

/**
 * @brief Enable or disable a power feature on the StickS3 PMIC.
 *
 * @param feature  Feature to control.
 * @param enable   true to enable, false to disable.
 *
 * @return ESP_OK on success, otherwise an esp_err_t error code.
 */
esp_err_t m5_sticks3_power_enable(m5_sticks3_power_feature_t feature, bool enable);

#ifdef __cplusplus
}
#endif
