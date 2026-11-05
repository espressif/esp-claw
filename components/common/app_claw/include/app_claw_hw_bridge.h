/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "claw_hw_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_CLAW_HW_GPIO,
    APP_CLAW_HW_I2C_BUS,
    APP_CLAW_HW_I2C_ADDR,
    APP_CLAW_HW_I2S_BUS,
    APP_CLAW_HW_SPI_BUS,
    APP_CLAW_HW_UART_BUS,
    APP_CLAW_HW_ADC_UNIT,
    APP_CLAW_HW_ADC_CHANNEL,
    APP_CLAW_HW_LEDC_TIMER,
    APP_CLAW_HW_LEDC_CHANNEL,
    APP_CLAW_HW_SDMMC_SLOT,
    APP_CLAW_HW_DSI_BUS,
    APP_CLAW_HW_LDO_CHANNEL,
} app_claw_hw_resource_kind_t;

typedef struct {
    app_claw_hw_resource_kind_t kind;
    int a;
    int b; /* Address/channel/timer, SDMMC bus width, or zero. */
} app_claw_hw_resource_t;

typedef struct {
    const char *name;
    bool device;
    size_t group; /* Codec/I2S aliases share one physical claim. */
    const app_claw_hw_resource_t *resources;
    size_t resource_count;
    bool board_owned;
} app_claw_hw_group_t;

typedef struct {
    const app_claw_hw_group_t *groups;
    size_t group_count;
} app_claw_hw_board_config_t;

extern const app_claw_hw_board_config_t app_claw_hw_generated_board;
/* Register the catalog before board initialization; no resources are claimed here. */
esp_err_t app_claw_hw_bridge_init(claw_hw_registry_handle_t registry);

#ifdef __cplusplus
}
#endif
