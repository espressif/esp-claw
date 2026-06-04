/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wave_rover_hal.h"
#include "wave_rover_hal_internal.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "wr_hal";

i2c_master_bus_handle_t g_wr_i2c_bus = NULL;
bool g_wr_dry_run = true;

esp_err_t wr_hal_init(bool dry_run)
{
    g_wr_dry_run = dry_run;

    if (dry_run) {
        ESP_LOGI(TAG, "dry-run mode: no hardware access");
        ESP_RETURN_ON_ERROR(wr_motor_worker_start(), TAG, "motor worker start");
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = WR_I2C_PORT,
        .sda_io_num          = WR_I2C_SDA,
        .scl_io_num          = WR_I2C_SCL,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &g_wr_i2c_bus),
                        TAG, "i2c_new_master_bus");
    ESP_LOGI(TAG, "I2C bus init: SDA=%d SCL=%d", WR_I2C_SDA, WR_I2C_SCL);

    ESP_RETURN_ON_ERROR(wr_motor_worker_start(), TAG, "motor worker start");
    return ESP_OK;
}
