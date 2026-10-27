/**
 * @file cap_watchdog.c
 * @brief watchdog Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_watchdog.h"
#include "esp_log.h"
static const char *TAG = "cap_watchdog";
esp_err_t cap_watchdog_init(void) {
    ESP_LOGI(TAG, "watchdog initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_watchdog_deinit(void) {
    ESP_LOGI(TAG, "watchdog deinitialized");
    return ESP_OK;
}
