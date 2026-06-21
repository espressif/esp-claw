/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_im_qq.h"

#include <stdio.h>
#include <string.h>

#include "argtable3/argtable3.h"
#include "cap_im_qq.h"
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
} qq_args;

static int cmd_qq_credentials(const char *app_id, const char *app_secret)
{
    esp_err_t err;

    err = cap_im_qq_set_credentials(app_id, app_secret);
    if (err != ESP_OK) {
        printf("qq_credentials failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("QQ credentials updated\n");
    return 0;
}

static int cmd_qq_start(void)
{
    esp_err_t err;

    err = cap_im_qq_start();
    if (err != ESP_OK) {
        printf("qq_start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("QQ gateway started\n");
    return 0;
}

static int cmd_qq_stop(void)
{
    esp_err_t err;

    err = cap_im_qq_stop();
    if (err != ESP_OK) {
        printf("qq_stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("QQ gateway stopped\n");
    return 0;
}

static int cmd_qq_send_text(const char *chat_id, const char *text)
{
    esp_err_t err;

    err = cap_im_qq_send_text(chat_id, text);
    if (err != ESP_OK) {
        printf("qq_send_text failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("QQ text sent\n");
    return 0;
}

static int cmd_qq_send_image(const char *chat_id, const char *path, const char *caption)
{
    esp_err_t err;

    err = cap_im_qq_send_image(chat_id, path, caption);
    if (err != ESP_OK) {
        printf("qq_send_image failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("QQ image sent\n");
    return 0;
}

static int cmd_qq_send_file(const char *chat_id, const char *path, const char *caption)
{
    esp_err_t err;

    err = cap_im_qq_send_file(chat_id, path, caption);
    if (err != ESP_OK) {
        printf("qq_send_file failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("QQ file sent\n");
    return 0;
}

static int qq_func(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **) &qq_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, qq_args.end, argv[0]);
        return 1;
    }

    int operation_count = qq_args.set_credentials->count + qq_args.start->count + qq_args.stop->count +
                          qq_args.send_text->count + qq_args.send_image->count + qq_args.send_file->count;

    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    if (qq_args.set_credentials->count) {
        if (!qq_args.app_id->count || !qq_args.app_secret->count) {
            printf("'--set-credentials' requires '--app-id' and '--app-secret'\n");
            return 1;
        }

        return cmd_qq_credentials(qq_args.app_id->sval[0], qq_args.app_secret->sval[0]);
    }

    if (qq_args.start->count) {
        return cmd_qq_start();
    }

    if (qq_args.stop->count) {
        return cmd_qq_stop();
    }

    if (qq_args.send_text->count) {
        if (!qq_args.text->count) {
            printf("'--send-text' requires '--text'\n");
            return 1;
        }

        return cmd_qq_send_text(qq_args.send_text->sval[0], qq_args.text->sval[0]);
    }

    if (!qq_args.path->count) {
        printf("'--send-image' and '--send-file' require '--path'\n");
        return 1;
    }

    if (qq_args.send_image->count) {
        return cmd_qq_send_image(qq_args.send_image->sval[0],
                                 qq_args.path->sval[0],
                                 qq_args.caption->count ? qq_args.caption->sval[0] : NULL);
    }

    return cmd_qq_send_file(qq_args.send_file->sval[0],
                            qq_args.path->sval[0],
                            qq_args.caption->count ? qq_args.caption->sval[0] : NULL);
}

void register_cap_im_qq(void)
{
    qq_args.set_credentials = arg_lit0("c", "set-credentials", "Set QQ bot credentials");
    qq_args.start = arg_lit0(NULL, "start", "Start the QQ bot gateway");
    qq_args.stop = arg_lit0(NULL, "stop", "Stop the QQ bot gateway");
    qq_args.send_text = arg_str0(NULL, "send-text", "<chat_id>", "Send text to a QQ chat");
    qq_args.send_image = arg_str0(NULL, "send-image", "<chat_id>", "Send an image to a QQ chat");
    qq_args.send_file = arg_str0(NULL, "send-file", "<chat_id>", "Send a file to a QQ chat");
    qq_args.app_id = arg_str0(NULL, "app-id", "<app_id>", "QQ bot app id");
    qq_args.app_secret = arg_str0(NULL, "app-secret", "<app_secret>", "QQ bot app secret");
    qq_args.text = arg_str0("t", "text", "<text>", "Text content");
    qq_args.path = arg_str0("p", "path", "<path>", "Local file path");
    qq_args.caption = arg_str0(NULL, "caption", "<caption>", "Optional caption");
    qq_args.end = arg_end(8);

    const esp_console_cmd_t qq_cmd = {
        .command = "qq",
        .help = "QQ operation.\n"
        "Examples:\n"
        " qq --set-credentials --app-id 123 --app-secret abc\n"
        " qq --start\n"
        " qq --stop\n"
        " qq --send-text group123 --text \"hello\"\n"
        " qq --send-image group123 --path /spiffs/a.jpg --caption \"hi\"\n"
        " qq --send-file group123 --path /spiffs/a.txt --caption \"file\"\n",
        .func = qq_func,
        .argtable = &qq_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&qq_cmd));
}
