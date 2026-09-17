/**
 * @file cap_heartbeat.c
 * @brief heartbeat Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_heartbeat.h"
#include "esp_log.h"
static const char *TAG = "cap_heartbeat";
esp_err_t cap_heartbeat_init(void) {
    ESP_LOGI(TAG, "heartbeat initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_heartbeat_deinit(void) {
    ESP_LOGI(TAG, "heartbeat deinitialized");
    return ESP_OK;
}
