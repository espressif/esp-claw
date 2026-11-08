/**
 * @file cap_vector_store.c
 * @brief vector_store Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_vector_store.h"
#include "esp_log.h"
static const char *TAG = "cap_vector_store";
esp_err_t cap_vector_store_init(void) {
    ESP_LOGI(TAG, "vector_store initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_vector_store_deinit(void) {
    ESP_LOGI(TAG, "vector_store deinitialized");
    return ESP_OK;
}
