/**
 * @file cap_event_bus.c
 * @brief event_bus Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_event_bus.h"
#include "esp_log.h"
static const char *TAG = "cap_event_bus";
esp_err_t cap_event_bus_init(void) {
    ESP_LOGI(TAG, "event_bus initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_event_bus_deinit(void) {
    ESP_LOGI(TAG, "event_bus deinitialized");
    return ESP_OK;
}
