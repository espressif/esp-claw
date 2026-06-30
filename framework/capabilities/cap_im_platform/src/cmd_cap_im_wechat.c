/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cmd_cap_im_wechat.h"

#include <stdio.h>

#include "argtable3/argtable3.h"
#include "cap_im_wechat.h"
#include "esp_console.h"

static struct {
    struct arg_lit *set_config;
    struct arg_lit *start;
    struct arg_lit *stop;
    struct arg_str *send_text;
    struct arg_str *send_image;
    struct arg_str *token;
    struct arg_str *base_url;
    struct arg_str *cdn_base_url;
    struct arg_str *account_id;
    struct arg_str *app_id;
    struct arg_str *client_version;
    struct arg_str *route_tag;
    struct arg_str *text;
    struct arg_str *path;
    struct arg_str *caption;
    struct arg_end *end;
} wechat_args;

static int cmd_wechat_set_config(void)
{
    cap_im_wechat_client_config_t config = {0};
    esp_err_t err;

    if (!wechat_args.token->count || !wechat_args.base_url->count) {
        printf("'--set-config' requires '--token' and '--base-url'\n");
        return 1;
    }

    config.token = wechat_args.token->sval[0];
    config.base_url = wechat_args.base_url->sval[0];
    config.cdn_base_url = wechat_args.cdn_base_url->count ? wechat_args.cdn_base_url->sval[0] : NULL;
    config.account_id = wechat_args.account_id->count ? wechat_args.account_id->sval[0] : NULL;
    config.app_id = wechat_args.app_id->count ? wechat_args.app_id->sval[0] : NULL;
    config.client_version = wechat_args.client_version->count ? wechat_args.client_version->sval[0] :
                            NULL;
    config.route_tag = wechat_args.route_tag->count ? wechat_args.route_tag->sval[0] : NULL;

    err = cap_im_wechat_set_client_config(&config);
    if (err != ESP_OK) {
        printf("wechat_set_config failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("WeChat client config updated\n");
    return 0;
}

static int cmd_wechat_start(void)
{
    esp_err_t err = cap_im_wechat_start();

    if (err != ESP_OK) {
        printf("wechat_start failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("WeChat gateway started\n");
    return 0;
}

static int cmd_wechat_stop(void)
{
    esp_err_t err = cap_im_wechat_stop();

    if (err != ESP_OK) {
        printf("wechat_stop failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("WeChat gateway stopped\n");
    return 0;
}

static int cmd_wechat_send_text(const char *chat_id, const char *text)
{
    esp_err_t err = cap_im_wechat_send_text(chat_id, text);

    if (err != ESP_OK) {
        printf("wechat_send_text failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("WeChat text sent\n");
    return 0;
}

static int cmd_wechat_send_image(const char *chat_id, const char *path, const char *caption)
{
    esp_err_t err = cap_im_wechat_send_image(chat_id, path, caption);

    if (err != ESP_OK) {
        printf("wechat_send_image failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("WeChat image sent\n");
    return 0;
}

static int wechat_func(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&wechat_args);
    int operation_count;

    if (nerrors != 0) {
        arg_print_errors(stderr, wechat_args.end, argv[0]);
        return 1;
    }

    operation_count = wechat_args.set_config->count + wechat_args.start->count +
                      wechat_args.stop->count + wechat_args.send_text->count +
                      wechat_args.send_image->count;
    if (operation_count != 1) {
        printf("Exactly one operation must be specified\n");
        return 1;
    }

    if (wechat_args.set_config->count) {
        return cmd_wechat_set_config();
    }

    if (wechat_args.start->count) {
        return cmd_wechat_start();
    }

    if (wechat_args.stop->count) {
        return cmd_wechat_stop();
    }

    if (wechat_args.send_text->count) {
        if (!wechat_args.text->count) {
            printf("'--send-text' requires '--text'\n");
            return 1;
        }

        return cmd_wechat_send_text(wechat_args.send_text->sval[0], wechat_args.text->sval[0]);
    }

    if (!wechat_args.path->count) {
        printf("'--send-image' requires '--path'\n");
        return 1;
    }

    return cmd_wechat_send_image(wechat_args.send_image->sval[0],
                                 wechat_args.path->sval[0],
                                 wechat_args.caption->count ? wechat_args.caption->sval[0] : NULL);
}

void register_cap_im_wechat(void)
{
    wechat_args.set_config = arg_lit0("c", "set-config", "Set WeChat client config");
    wechat_args.start = arg_lit0(NULL, "start", "Start the WeChat gateway");
    wechat_args.stop = arg_lit0(NULL, "stop", "Stop the WeChat gateway");
    wechat_args.send_text = arg_str0(NULL, "send-text", "<chat_id>", "Send text to a WeChat chat");
    wechat_args.send_image =
        arg_str0(NULL, "send-image", "<chat_id>", "Send an image to a WeChat chat");
    wechat_args.token = arg_str0(NULL, "token", "<token>", "WeChat token");
    wechat_args.base_url = arg_str0(NULL, "base-url", "<url>", "WeChat base URL");
    wechat_args.cdn_base_url = arg_str0(NULL, "cdn-base-url", "<url>", "WeChat CDN base URL");
    wechat_args.account_id = arg_str0(NULL, "account-id", "<id>", "WeChat account ID");
    wechat_args.app_id = arg_str0(NULL, "app-id", "<id>", "WeChat app ID");
    wechat_args.client_version =
        arg_str0(NULL, "client-version", "<ver>", "WeChat client version");
    wechat_args.route_tag = arg_str0(NULL, "route-tag", "<tag>", "WeChat route tag");
    wechat_args.text = arg_str0("t", "text", "<text>", "Text content");
    wechat_args.path = arg_str0("p", "path", "<path>", "Local file path");
    wechat_args.caption = arg_str0(NULL, "caption", "<caption>", "Optional caption");
    wechat_args.end = arg_end(10);

    const esp_console_cmd_t wechat_cmd = {
        .command = "wechat",
        .help = "WeChat operation.\n"
        "Examples:\n"
        " wechat --set-config --token abc --base-url https://ilinkai.weixin.qq.com\n"
        " wechat --start\n"
        " wechat --stop\n"
        " wechat --send-text room123 --text \"hello\"\n"
        " wechat --send-image room123 --path /spiffs/a.jpg --caption \"hi\"\n",
        .func = wechat_func,
        .argtable = &wechat_args,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&wechat_cmd));
}
