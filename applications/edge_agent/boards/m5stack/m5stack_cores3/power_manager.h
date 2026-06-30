/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: LicenseRef-Espressif-Modified-MIT
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif  /* __cplusplus */

/**
 * @brief  M5Stack CoreS3 feature enumeration
 */
typedef enum {
    CORES3_POWER_MANAGER_FEATURE_LCD,      /*!< LCD display feature */
    CORES3_POWER_MANAGER_FEATURE_TOUCH,    /*!< Touch screen feature */
    CORES3_POWER_MANAGER_FEATURE_5V,       /*!< 5V feature */
    CORES3_POWER_MANAGER_FEATURE_SD,       /*!< SD card feature */
    CORES3_POWER_MANAGER_FEATURE_SPEAKER,  /*!< Speaker feature */
    CORES3_POWER_MANAGER_FEATURE_CAMERA,   /*!< Camera feature */
} cores3_power_manager_feature_t;

/**
 * @brief  Power manager handle structure
 */
typedef struct {
    i2c_master_dev_handle_t pm_handle;  /*!< I2C device handle for AXP2101 PMU */
} cores3_power_manager_handle_t;

int cores3_power_manager_init(void *config, int cfg_size, void **device_handle);
int cores3_power_manager_deinit(void *device_handle);
esp_err_t cores3_power_manager_enable(void *device_handle, cores3_power_manager_feature_t feature);

#ifdef __cplusplus
}
#endif  /* __cplusplus */
