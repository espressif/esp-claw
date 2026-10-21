/**
 * @file cap_prompt_template.c
 * @brief prompt_template Capability Stub for Neon-Claw
 * @author maruf009sultan
 */
#include "cap_prompt_template.h"
#include "esp_log.h"
static const char *TAG = "cap_prompt_template";
esp_err_t cap_prompt_template_init(void) {
    ESP_LOGI(TAG, "prompt_template initialized (stub)");
    return ESP_OK;
}
esp_err_t cap_prompt_template_deinit(void) {
    ESP_LOGI(TAG, "prompt_template deinitialized");
    return ESP_OK;
}
