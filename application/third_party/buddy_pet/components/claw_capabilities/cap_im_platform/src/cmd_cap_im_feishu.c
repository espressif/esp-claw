/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_im_feishu.h"

#include <stdio.h>

#include "argtable3/argtable3.h"
#include "cap_im_feishu.h"
#include "esp_console.h"

static struct {
    struct arg_lit *set_credentials;
    struct arg_lit *start;
    struct arg_lit *stop;
    struct arg_str *send_text;
    struct arg_str *send_image;
    struct arg_str *send_file;
    struct arg_str *app_id;
    struct arg_str *app_secret;
    struct arg_str *text;
    struct arg_str *path;
    struct arg_str *caption;
    struct arg_end *end;
} feishu_args;

static int cmd_feishu_set_credentials(const char *app_id, const char *app_secret)
{
    esp_err_t err = cap_im_feishu_set_credentials(app_id, app_secret);

    if (err != ESP_OK) {
        printf("feishu_set_credentials failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Feishu credentials updated\n");
    return 0;
}

static int cmd_feishu_start(void)
{
    esp_err_t err = cap_im_feishu_start();

    if (err != ESP_OK) {
        printf("feishu_start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Feishu gateway started\n");
    return 0;
}

static int cmd_feishu_stop(void)
{
    esp_err_t err = cap_im_feishu_stop();

    if (err != ESP_OK) {
        printf("feishu_stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Feishu gateway stopped\n");
    return 0;
}

static int cmd_feishu_send_text(const char *chat_id, const char *text)
{
    esp_err_t err = cap_im_feishu_send_text(chat_id, text);

    if (err != ESP_OK) {
        printf("feishu_send_text failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Feishu text sent\n");
    return 0;
}

static int cmd_feishu_send_image(const char *chat_id, const char *path, const char *caption)
{
    esp_err_t err = cap_im_feishu_send_image(chat_id, path, caption);

    if (err != ESP_OK) {
        printf("feishu_send_image failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Feishu image sent\n");
    return 0;
}

static int cmd_feishu_send_file(const char *chat_id, const char *path, const char *caption)
{
    esp_err_t err = cap_im_feishu_send_file(chat_id, path, caption);

    if (err != ESP_OK) {
        printf("feishu_send_file failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Feishu file sent\n");
    return 0;
}

static int feishu_func(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&feishu_args);
    int operation_count;

    if (nerrors != 0) {
        arg_print_errors(stderr, feishu_args.end, argv[0]);
        return 1;
    }

    operation_count = feishu_args.set_credentials->count +
                      feishu_args.start->count +
                      feishu_args.stop->count +
                      feishu_args.send_text->count +
                      feishu_args.send_image->count +
                      feishu_args.send_file->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    if (feishu_args.set_credentials->count) {
        if (!feishu_args.app_id->count || !feishu_args.app_secret->count) {
            printf("'--set-credentials' requires '--app-id' and '--app-secret'\n");
            return 1;
        }
        return cmd_feishu_set_credentials(feishu_args.app_id->sval[0], feishu_args.app_secret->sval[0]);
    }

    if (feishu_args.start->count) {
        return cmd_feishu_start();
    }

    if (feishu_args.stop->count) {
        return cmd_feishu_stop();
    }

    if (feishu_args.send_text->count) {
        if (!feishu_args.text->count) {
            printf("'--send-text' requires '--text'\n");
            return 1;
        }
        return cmd_feishu_send_text(feishu_args.send_text->sval[0], feishu_args.text->sval[0]);
    }

    if (!feishu_args.path->count) {
        printf("'--send-image' and '--send-file' require '--path'\n");
        return 1;
    }

    if (feishu_args.send_image->count) {
        return cmd_feishu_send_image(feishu_args.send_image->sval[0],
                                     feishu_args.path->sval[0],
                                     feishu_args.caption->count ? feishu_args.caption->sval[0] : NULL);
    }

    return cmd_feishu_send_file(feishu_args.send_file->sval[0],
                                feishu_args.path->sval[0],
                                feishu_args.caption->count ? feishu_args.caption->sval[0] : NULL);
}

void register_cap_im_feishu(void)
{
    feishu_args.set_credentials = arg_lit0(NULL, "set-credentials", "Set Feishu app credentials");
    feishu_args.start = arg_lit0(NULL, "start", "Start the Feishu gateway");
    feishu_args.stop = arg_lit0(NULL, "stop", "Stop the Feishu gateway");
    feishu_args.send_text = arg_str0(NULL, "send-text", "<chat_id>", "Send text to a Feishu chat");
    feishu_args.send_image = arg_str0(NULL, "send-image", "<chat_id>", "Send an image to a Feishu chat");
    feishu_args.send_file = arg_str0(NULL, "send-file", "<chat_id>", "Send a file to a Feishu chat");
    feishu_args.app_id = arg_str0(NULL, "app-id", "<app_id>", "Feishu app ID");
    feishu_args.app_secret = arg_str0(NULL, "app-secret", "<app_secret>", "Feishu app secret");
    feishu_args.text = arg_str0(NULL, "text", "<text>", "Text content");
    feishu_args.path = arg_str0(NULL, "path", "<path>", "Local file path");
    feishu_args.caption = arg_str0(NULL, "caption", "<caption>", "Optional caption sent as text");
    feishu_args.end = arg_end(11);

    const esp_console_cmd_t feishu_cmd = {
        .command = "feishu",
        .help = "Feishu operation.\n"
        "Examples:\n"
        " feishu --set-credentials --app-id cli_xxx --app-secret sec_xxx\n"
        " feishu --start\n"
        " feishu --stop\n"
        " feishu --send-text ou_xxx --text \"hello\"\n"
        " feishu --send-image ou_xxx --path <storage_root>/inbox/pic.jpg --caption \"look\"\n"
        " feishu --send-file ou_xxx --path <storage_root>/docs/readme.txt\n",
        .func = feishu_func,
        .argtable = &feishu_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&feishu_cmd));
}
