/**
 * @file cap_context_engine.c
 * @brief context_engine Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_context_engine.h"
#include "esp_log.h"
static const char *TAG = "cap_context_engine";
esp_err_t cap_context_engine_init(void) {
    ESP_LOGI(TAG, "context_engine initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_context_engine_deinit(void) {
    ESP_LOGI(TAG, "context_engine deinitialized");
    return ESP_OK;
}
