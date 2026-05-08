/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_rover.h"
#include "cap_rover_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include "argtable3/argtable3.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "cmd_cap_rover";

static struct {
    struct arg_int *x;
    struct arg_int *y;
    struct arg_int *z;
    struct arg_int *duration_ms;
    struct arg_end *end;
} s_move_args;

static int cmd_rover_move(int argc, char **argv)
{
    int errs = arg_parse(argc, argv, (void **)&s_move_args);
    if (errs > 0) {
        arg_print_errors(stderr, s_move_args.end, argv[0]);
        return 1;
    }

    int x = s_move_args.x->count ? s_move_args.x->ival[0] : 0;
    int y = s_move_args.y->count ? s_move_args.y->ival[0] : 60;
    int z = s_move_args.z->count ? s_move_args.z->ival[0] : 0;
    int dur = s_move_args.duration_ms->count ? s_move_args.duration_ms->ival[0] : 1500;
    rover_action_req_t req = {
        .kind = ROVER_ACTION_MOVE,
        .x = (int8_t)x,
        .y = (int8_t)y,
        .z = (int8_t)z,
        .duration_ms = (uint16_t)dur,
    };
    rover_action_result_t r = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(dur + 1000), &r);
    printf("rover_move: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_rover_stop(int argc, char **argv)
{
    rover_action_req_t req = { .kind = ROVER_ACTION_STOP };
    rover_action_result_t r = {0};
    (void)argc;
    (void)argv;
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(1000), &r);
    printf("rover_stop: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_rover_gripper(rover_action_kind_t kind, const char *label)
{
    rover_action_req_t req = { .kind = kind };
    rover_action_result_t r = {0};
    esp_err_t err = cap_rover_submit_and_wait(&req, pdMS_TO_TICKS(1000), &r);
    printf("%s: %s\n", label, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_rover_open(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return cmd_rover_gripper(ROVER_ACTION_GRIPPER_OPEN, "rover_open");
}

static int cmd_rover_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return cmd_rover_gripper(ROVER_ACTION_GRIPPER_CLOSE, "rover_close");
}

void cap_rover_register_cli(void)
{
    s_move_args.x = arg_int0("x", "x", "<int>", "lateral speed -100..100");
    s_move_args.y = arg_int0("y", "y", "<int>", "forward speed -100..100");
    s_move_args.z = arg_int0("z", "z", "<int>", "rotation speed -100..100");
    s_move_args.duration_ms = arg_int0("d", "duration", "<ms>", "duration in ms");
    s_move_args.end = arg_end(2);

    const esp_console_cmd_t move_cmd = {
        .command = "rover_move",
        .help = "Move rover with velocity vector for a duration",
        .func = cmd_rover_move,
        .argtable = &s_move_args,
    };
    const esp_console_cmd_t stop_cmd = {
        .command = "rover_stop",
        .help = "Stop rover immediately",
        .func = cmd_rover_stop,
    };
    const esp_console_cmd_t open_cmd = {
        .command = "rover_open",
        .help = "Open gripper",
        .func = cmd_rover_open,
    };
    const esp_console_cmd_t close_cmd = {
        .command = "rover_close",
        .help = "Close gripper",
        .func = cmd_rover_close,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&move_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&stop_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&open_cmd));
    ESP_ERROR_CHECK(esp_console_cmd_register(&close_cmd));
    ESP_LOGI(TAG, "CLI commands registered");
}
