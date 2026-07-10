/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_im_tg.h"

#include <stdio.h>

#include "argtable3/argtable3.h"
#include "cap_im_tg.h"
#include "esp_console.h"

static struct {
    struct arg_lit *set_token;
    struct arg_lit *start;
    struct arg_lit *stop;
    struct arg_str *send_text;
    struct arg_str *send_image;
    struct arg_str *send_file;
    struct arg_str *token;
    struct arg_str *text;
    struct arg_str *path;
    struct arg_str *caption;
    struct arg_end *end;
} tg_args;

static int cmd_tg_set_token(const char *token)
{
    esp_err_t err = cap_im_tg_set_token(token);

    if (err != ESP_OK) {
        printf("tg_set_token failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Telegram bot token updated\n");
    return 0;
}

static int cmd_tg_start(void)
{
    esp_err_t err = cap_im_tg_start();

    if (err != ESP_OK) {
        printf("tg_start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Telegram gateway started\n");
    return 0;
}

static int cmd_tg_stop(void)
{
    esp_err_t err = cap_im_tg_stop();

    if (err != ESP_OK) {
        printf("tg_stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Telegram gateway stopped\n");
    return 0;
}

static int cmd_tg_send_text(const char *chat_id, const char *text)
{
    esp_err_t err = cap_im_tg_send_text(chat_id, text);

    if (err != ESP_OK) {
        printf("tg_send_text failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Telegram text sent\n");
    return 0;
}

static int cmd_tg_send_image(const char *chat_id, const char *path, const char *caption)
{
    esp_err_t err = cap_im_tg_send_image(chat_id, path, caption);

    if (err != ESP_OK) {
        printf("tg_send_image failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Telegram image sent\n");
    return 0;
}

static int cmd_tg_send_file(const char *chat_id, const char *path, const char *caption)
{
    esp_err_t err = cap_im_tg_send_file(chat_id, path, caption);

    if (err != ESP_OK) {
        printf("tg_send_file failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Telegram file sent\n");
    return 0;
}

static int tg_func(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&tg_args);
    int operation_count;

    if (nerrors != 0) {
        arg_print_errors(stderr, tg_args.end, argv[0]);
        return 1;
    }

    operation_count = tg_args.set_token->count + tg_args.start->count + tg_args.stop->count +
                      tg_args.send_text->count + tg_args.send_image->count + tg_args.send_file->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    if (tg_args.set_token->count) {
        if (!tg_args.token->count) {
            printf("'--set-token' requires '--token'\n");
            return 1;
        }

        return cmd_tg_set_token(tg_args.token->sval[0]);
    }

    if (tg_args.start->count) {
        return cmd_tg_start();
    }

    if (tg_args.stop->count) {
        return cmd_tg_stop();
    }

    if (tg_args.send_text->count) {
        if (!tg_args.text->count) {
            printf("'--send-text' requires '--text'\n");
            return 1;
        }

        return cmd_tg_send_text(tg_args.send_text->sval[0], tg_args.text->sval[0]);
    }

    if (!tg_args.path->count) {
        printf("'--send-image' and '--send-file' require '--path'\n");
        return 1;
    }

    if (tg_args.send_image->count) {
        return cmd_tg_send_image(tg_args.send_image->sval[0],
                                 tg_args.path->sval[0],
                                 tg_args.caption->count ? tg_args.caption->sval[0] : NULL);
    }

    return cmd_tg_send_file(tg_args.send_file->sval[0],
                            tg_args.path->sval[0],
                            tg_args.caption->count ? tg_args.caption->sval[0] : NULL);
}

void register_cap_im_tg(void)
{
    tg_args.set_token = arg_lit0(NULL, "set-token", "Set Telegram bot token");
    tg_args.start = arg_lit0(NULL, "start", "Start the Telegram bot gateway");
    tg_args.stop = arg_lit0(NULL, "stop", "Stop the Telegram bot gateway");
    tg_args.send_text = arg_str0(NULL, "send-text", "<chat_id>", "Send text to a Telegram chat");
    tg_args.send_image = arg_str0(NULL, "send-image", "<chat_id>", "Send an image to a Telegram chat");
    tg_args.send_file = arg_str0(NULL, "send-file", "<chat_id>", "Send a file to a Telegram chat");
    tg_args.token = arg_str0("t", "token", "<token>", "Telegram bot token");
    tg_args.text = arg_str0(NULL, "text", "<text>", "Text content");
    tg_args.path = arg_str0("p", "path", "<path>", "Local file path");
    tg_args.caption = arg_str0(NULL, "caption", "<caption>", "Optional caption");
    tg_args.end = arg_end(8);

    const esp_console_cmd_t tg_cmd = {
        .command = "tg",
        .help = "Telegram operation.\n"
        "Examples:\n"
        " tg --set-token --token 123456:abc\n"
        " tg --start\n"
        " tg --stop\n"
        " tg --send-text 123456 --text \"hello\"\n"
        " tg --send-image 123456 --path /spiffs/a.jpg --caption \"hi\"\n"
        " tg --send-file 123456 --path /spiffs/a.txt --caption \"file\"\n",
        .func = tg_func,
        .argtable = &tg_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&tg_cmd));
}
