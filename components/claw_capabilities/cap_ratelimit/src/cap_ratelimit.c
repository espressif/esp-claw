/**
 * @file cap_ratelimit.c
 * @brief ratelimit Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_ratelimit.h"
#include "esp_log.h"
static const char *TAG = "cap_ratelimit";
esp_err_t cap_ratelimit_init(void) {
    ESP_LOGI(TAG, "ratelimit initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_ratelimit_deinit(void) {
    ESP_LOGI(TAG, "ratelimit deinitialized");
    return ESP_OK;
}
