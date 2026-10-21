/**
 * @file cap_react_agent.c
 * @brief react_agent Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_react_agent.h"
#include "esp_log.h"
static const char *TAG = "cap_react_agent";
esp_err_t cap_react_agent_init(void) {
    ESP_LOGI(TAG, "react_agent initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_react_agent_deinit(void) {
    ESP_LOGI(TAG, "react_agent deinitialized");
    return ESP_OK;
}
