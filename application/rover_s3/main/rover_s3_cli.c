/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rover_s3_cli.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cap_voice.h"
#include "rover_s3_settings.h"

static const char *TAG = "rover_s3_cli";
static esp_console_repl_t *s_repl;

static const char *find_arg(int argc, char **argv, const char *name)
{
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static void copy_arg(char *dst, size_t dst_size, const char *src)
{
    if (src && dst && dst_size > 0) {
        strlcpy(dst, src, dst_size);
    }
}

static const char *secret_state(const char *s)
{
    return (s && s[0]) ? "<set>" : "<empty>";
}

static void print_settings(const rover_s3_settings_t *s)
{
    printf("wifi_ssid: %s\n", s->wifi_ssid[0] ? s->wifi_ssid : "<empty>");
    printf("wifi_password: %s\n", secret_state(s->wifi_password));
    printf("tg_bot_token: %s\n", secret_state(s->tg_bot_token));
    printf("llm_api_key: %s\n", secret_state(s->llm_api_key));
    printf("llm_backend: %s\n", s->llm_backend_type);
    printf("llm_profile: %s\n", s->llm_profile);
    printf("llm_model: %s\n", s->llm_model);
    printf("llm_base_url: %s\n", s->llm_base_url);
    printf("llm_auth: %s\n", s->llm_auth_type);
    printf("llm_timeout_ms: %s\n", s->llm_timeout_ms);
    printf("time_timezone: %s\n", s->time_timezone);
    printf("whisper_api_key: %s\n", secret_state(s->whisper_api_key));
    printf("whisper_base_url: %s\n", s->whisper_base_url[0] ? s->whisper_base_url : "<empty>");
    printf("tts_api_key: %s\n", secret_state(s->tts_api_key));
    printf("tts_base_url: %s\n", s->tts_base_url[0] ? s->tts_base_url : "<empty>");
    printf("tts_voice: %s\n", s->tts_voice[0] ? s->tts_voice : "<empty>");
    printf("wake_sensitivity: %s\n", s->wake_sensitivity[0] ? s->wake_sensitivity : "<empty>");
    printf("multinet_threshold: %s\n", s->multinet_threshold[0] ? s->multinet_threshold : "<empty>");
    printf("voice_enabled: %s\n", s->voice_enabled[0] ? s->voice_enabled : "<empty>");
}

static void print_usage(void)
{
    printf("Usage:\n");
    printf("  rover_config show\n");
    printf("  rover_config wifi --ssid <ssid> --password <password>\n");
    printf("  rover_config tg --token <token>\n");
    printf("  rover_config llm --api-key <key> [--model <model>] [--base-url <url>] [--backend <type>] [--profile <profile>] [--auth <type>] [--timeout-ms <ms>]\n");
    printf("  rover_config timezone --tz <TZ>\n");
    printf("  rover_config voice [--whisper-key <key>] [--whisper-url <url>] [--tts-key <key>] [--tts-url <url>] [--voice <name>] [--enabled 0|1]\n");
    printf("  rover_config clear\n");
    printf("  rover_config reboot\n");
}

static int save_and_report(const rover_s3_settings_t *s)
{
    esp_err_t err = rover_s3_settings_save(s);
    printf("save: %s\n", esp_err_to_name(err));
    if (err == ESP_OK) {
        printf("Reboot to apply network, Telegram, and LLM settings.\n");
    }
    return err == ESP_OK ? 0 : 1;
}

static int cmd_rover_config(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "reboot") == 0) {
        printf("rebooting...\n");
        esp_restart();
        return 0;
    }

    rover_s3_settings_t s = {0};
    esp_err_t err = rover_s3_settings_load(&s);
    if (err != ESP_OK) {
        printf("load: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (strcmp(argv[1], "show") == 0) {
        print_settings(&s);
        return 0;
    }
    if (strcmp(argv[1], "wifi") == 0) {
        const char *ssid = find_arg(argc, argv, "--ssid");
        const char *password = find_arg(argc, argv, "--password");
        if (!ssid && argc >= 3) {
            ssid = argv[2];
        }
        if (!password && argc >= 4) {
            password = argv[3];
        }
        if (!ssid) {
            printf("Missing --ssid\n");
            return 1;
        }
        copy_arg(s.wifi_ssid, sizeof(s.wifi_ssid), ssid);
        copy_arg(s.wifi_password, sizeof(s.wifi_password), password ? password : "");
        return save_and_report(&s);
    }
    if (strcmp(argv[1], "tg") == 0) {
        const char *token = find_arg(argc, argv, "--token");
        if (!token && argc >= 3) {
            token = argv[2];
        }
        if (!token) {
            printf("Missing --token\n");
            return 1;
        }
        copy_arg(s.tg_bot_token, sizeof(s.tg_bot_token), token);
        return save_and_report(&s);
    }
    if (strcmp(argv[1], "llm") == 0) {
        const char *api_key = find_arg(argc, argv, "--api-key");
        const char *model = find_arg(argc, argv, "--model");
        const char *base_url = find_arg(argc, argv, "--base-url");
        const char *backend = find_arg(argc, argv, "--backend");
        const char *profile = find_arg(argc, argv, "--profile");
        const char *auth = find_arg(argc, argv, "--auth");
        const char *timeout_ms = find_arg(argc, argv, "--timeout-ms");
        if (!api_key) {
            printf("Missing --api-key\n");
            return 1;
        }
        copy_arg(s.llm_api_key, sizeof(s.llm_api_key), api_key);
        copy_arg(s.llm_model, sizeof(s.llm_model), model ? model : s.llm_model);
        copy_arg(s.llm_base_url, sizeof(s.llm_base_url), base_url ? base_url : s.llm_base_url);
        copy_arg(s.llm_backend_type, sizeof(s.llm_backend_type), backend ? backend : "openai_compatible");
        copy_arg(s.llm_profile, sizeof(s.llm_profile), profile ? profile : "openai");
        copy_arg(s.llm_auth_type, sizeof(s.llm_auth_type), auth ? auth : "bearer");
        copy_arg(s.llm_timeout_ms, sizeof(s.llm_timeout_ms), timeout_ms ? timeout_ms : "30000");
        return save_and_report(&s);
    }
    if (strcmp(argv[1], "timezone") == 0) {
        const char *tz = find_arg(argc, argv, "--tz");
        if (!tz && argc >= 3) {
            tz = argv[2];
        }
        if (!tz) {
            printf("Missing --tz\n");
            return 1;
        }
        copy_arg(s.time_timezone, sizeof(s.time_timezone), tz);
        return save_and_report(&s);
    }
    if (strcmp(argv[1], "voice") == 0) {
        const char *whisper_key  = find_arg(argc, argv, "--whisper-key");
        const char *whisper_url  = find_arg(argc, argv, "--whisper-url");
        const char *tts_key      = find_arg(argc, argv, "--tts-key");
        const char *tts_url      = find_arg(argc, argv, "--tts-url");
        const char *tts_voice    = find_arg(argc, argv, "--voice");
        const char *enabled      = find_arg(argc, argv, "--enabled");
        if (whisper_key) copy_arg(s.whisper_api_key,    sizeof(s.whisper_api_key),  whisper_key);
        if (whisper_url) copy_arg(s.whisper_base_url,   sizeof(s.whisper_base_url), whisper_url);
        if (tts_key)     copy_arg(s.tts_api_key,        sizeof(s.tts_api_key),      tts_key);
        if (tts_url)     copy_arg(s.tts_base_url,       sizeof(s.tts_base_url),     tts_url);
        if (tts_voice)   copy_arg(s.tts_voice,          sizeof(s.tts_voice),        tts_voice);
        if (enabled)     copy_arg(s.voice_enabled,      sizeof(s.voice_enabled),    enabled);
        return save_and_report(&s);
    }
    if (strcmp(argv[1], "clear") == 0) {
        err = rover_s3_settings_clear();
        printf("clear: %s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    printf("Unknown rover_config subcommand: %s\n", argv[1]);
    print_usage();
    return 1;
}

static int cmd_voice_test(int argc, char **argv)
{
    (void)argc; (void)argv;
    return cap_voice_speak("Привет, я ровер S3!") == ESP_OK ? 0 : 1;
}

static int cmd_voice_listen(int argc, char **argv)
{
    (void)argc; (void)argv;
    ESP_LOGI(TAG, "voice_listen: not implemented");
    return 0;
}

static int cmd_voice_tone(int argc, char **argv)
{
    (void)argc; (void)argv;
    extern esp_err_t cap_voice_audio_play_tone(int freq_hz, int duration_ms);
    return cap_voice_audio_play_tone(440, 500) == ESP_OK ? 0 : 1;
}

esp_err_t rover_s3_cli_init(void)
{
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    if (s_repl) {
        return ESP_OK;
    }

    repl_config.prompt = "rover> ";
    repl_config.task_stack_size = 8192;
    repl_config.max_cmdline_length = 512;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&hw_config, &repl_config, &s_repl), TAG, "new UART REPL");
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &s_repl), TAG, "new USB serial JTAG REPL");
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &s_repl), TAG, "new USB CDC REPL");
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    esp_console_register_help_command();
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "rover_config",
        .help = "Show or update rover runtime settings",
        .hint = "<show|wifi|tg|llm|timezone|clear|reboot>",
        .func = cmd_rover_config,
    }), TAG, "register rover_config");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "voice_test",
        .help = "Test voice recognition",
        .func = cmd_voice_test,
    }), TAG, "register voice_test");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "voice_listen",
        .help = "Start voice listening mode",
        .func = cmd_voice_listen,
    }), TAG, "register voice_listen");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "voice_tone",
        .help = "Play a test tone through speaker",
        .func = cmd_voice_tone,
    }), TAG, "register voice_tone");

    return ESP_OK;
}

esp_err_t rover_s3_cli_start(void)
{
    if (!s_repl) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "Starting console REPL");
    return esp_console_start_repl(s_repl);
}
