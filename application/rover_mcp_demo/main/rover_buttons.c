/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_buttons.h"

#include <stdlib.h>
#include <string.h>

#include "cap_rover.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rover_display.h"
#include "setup_device.h"

static const char *TAG = "rover_buttons";

#define BTN_TASK_PERIOD_MS  20
#define BTN_LONG_PRESS_MS   3000

static rover_buttons_config_t s_cfg;

static void inject_demo_event(void)
{
    if (!s_cfg.on_btn_a_short) {
        return;
    }

    claw_event_t ev = {0};
    strlcpy(ev.event_id, "btn_a_demo", sizeof(ev.event_id));
    strlcpy(ev.source_cap, "rover_buttons", sizeof(ev.source_cap));
    strlcpy(ev.event_type, "message", sizeof(ev.event_type));
    strlcpy(ev.source_channel, "btn_a", sizeof(ev.source_channel));
    strlcpy(ev.chat_id, "local", sizeof(ev.chat_id));
    strlcpy(ev.sender_id, "local", sizeof(ev.sender_id));
    strlcpy(ev.content_type, "text", sizeof(ev.content_type));
    ev.text = strdup("demo");
    if (ev.text) {
        (void)s_cfg.on_btn_a_short(&ev, s_cfg.on_btn_a_short_ctx);
        free(ev.text);
    }
}

static void buttons_task(void *arg)
{
    TickType_t btn_a_pressed_at = 0;
    bool btn_a_was_down = false;
    (void)arg;

    while (1) {
        rover_board_m5_update();

        if (rover_board_btn_b_pressed()) {
            cap_rover_emergency_stop_set();
        }

        bool down = rover_board_btn_a_pressed();
        TickType_t now = xTaskGetTickCount();
        if (down && !btn_a_was_down) {
            btn_a_pressed_at = now;
        }
        if (!down && btn_a_was_down) {
            TickType_t held = now - btn_a_pressed_at;
            if (held >= pdMS_TO_TICKS(BTN_LONG_PRESS_MS)) {
                ESP_LOGI(TAG, "BtnA long press");
                if (s_cfg.on_btn_a_long) {
                    s_cfg.on_btn_a_long(s_cfg.on_btn_a_long_ctx);
                }
            } else {
                ESP_LOGI(TAG, "BtnA short press");
                inject_demo_event();
            }
        }
        btn_a_was_down = down;

        rover_display_refresh();
        vTaskDelay(pdMS_TO_TICKS(BTN_TASK_PERIOD_MS));
    }
}

esp_err_t rover_buttons_init(const rover_buttons_config_t *config)
{
    if (config) {
        s_cfg = *config;
    }
    BaseType_t ok = xTaskCreate(buttons_task, "rover_btn", 4096, NULL, 4, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
