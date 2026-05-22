/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw_cli.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CONFIG_APP_CLAW_CAP_IM_QQ
#include "cap_im_qq.h"
#include "cmd_cap_im_qq.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
#include "cmd_cap_im_feishu.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
#include "cmd_cap_im_tg.h"
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
#include "cmd_cap_im_wechat.h"
#endif
#if CONFIG_APP_CLAW_CAP_LLM_INSPECT
#include "cmd_cap_llm_inspect.h"
#endif
#if CONFIG_APP_CLAW_CAP_LUA
#include "cmd_cap_lua.h"
#endif
#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
#include "cmd_cap_mcp_client.h"
#endif
#if CONFIG_APP_CLAW_CAP_MCP_SERVER
#include "cmd_cap_mcp_server.h"
#endif
#if CONFIG_APP_CLAW_CAP_ROUTER_MGR
#include "cmd_cap_router_mgr.h"
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
#include "cmd_cap_scheduler.h"
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
#include "cmd_cap_skill.h"
#endif
#if CONFIG_APP_CLAW_CAP_TIME
#include "cmd_cap_time.h"
#endif
#if CONFIG_APP_CLAW_CAP_WEB_SEARCH
#include "cmd_cap_web_search.h"
#endif
#include "claw_cap.h"
#include "claw_core.h"
#include "claw_event_publisher.h"
#include "claw_event_router.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "app_claw_cli";
static const size_t CAP_OUTPUT_BUF_SIZE = 1024;

static uint32_t s_next_request_id = 1;
static char s_current_session_id[64] = "default";

static char *join_prompt_args(int argc, char **argv)
{
    char *prompt = NULL;
    size_t prompt_len = 0;
    int i;

    if (argc < 2) {
        return NULL;
    }

    for (i = 1; i < argc; i++) {
        prompt_len += strlen(argv[i]) + 1;
    }

    prompt = calloc(1, prompt_len + 1);
    if (!prompt) {
        return NULL;
    }

    for (i = 1; i < argc; i++) {
        if (i > 1) {
            strcat(prompt, " ");
        }
        strcat(prompt, argv[i]);
    }

    return prompt;
}

static char *join_args_from(int argc, char **argv, int start_index)
{
    char *prompt = NULL;
    size_t prompt_len = 0;
    int i;

    if (argc <= start_index) {
        return NULL;
    }

    for (i = start_index; i < argc; i++) {
        prompt_len += strlen(argv[i]) + 1;
    }

    prompt = calloc(1, prompt_len + 1);
    if (!prompt) {
        return NULL;
    }

    for (i = start_index; i < argc; i++) {
        if (i > start_index) {
            strcat(prompt, " ");
        }
        strcat(prompt, argv[i]);
    }

    return prompt;
}

static int submit_and_print(const char *prompt, const char *session_id)
{
    claw_core_request_t request = {0};
    claw_core_response_t response = {0};
    esp_err_t err;

    request.request_id = s_next_request_id++;
    request.user_text = prompt;
    request.session_id = session_id;

    if (session_id && session_id[0]) {
        printf("Submitting request %" PRIu32 " [session=%s]...\n",
               request.request_id,
               session_id);
    } else {
        printf("Submitting request %" PRIu32 " [single-turn]...\n", request.request_id);
    }

    err = claw_core_submit(&request, 5000);
    if (err != ESP_OK) {
        printf("submit failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    err = claw_core_receive_for(request.request_id, &response, 130000);
    if (err != ESP_OK) {
        printf("receive failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    if (response.status == CLAW_CORE_RESPONSE_STATUS_OK && response.text) {
        printf("\nassistant> %s\n\n", response.text);
    } else {
        printf("\nerror> %s\n\n",
               response.error_message ? response.error_message : "unknown error");
    }

    claw_core_response_free(&response);
    return 0;
}

static int cmd_ask(int argc, char **argv)
{
    char *prompt = NULL;

    if (argc < 2) {
        printf("Usage: ask <prompt>\n");
        return 1;
    }

    prompt = join_prompt_args(argc, argv);
    if (!prompt) {
        printf("Out of memory\n");
        return 1;
    }

    argc = submit_and_print(prompt, s_current_session_id);
    free(prompt);
    return argc;
}

static int cmd_ask_once(int argc, char **argv)
{
    char *prompt = NULL;
    int rc;

    if (argc < 2) {
        printf("Usage: ask_once <prompt>\n");
        return 1;
    }

    prompt = join_prompt_args(argc, argv);
    if (!prompt) {
        printf("Out of memory\n");
        return 1;
    }

    rc = submit_and_print(prompt, NULL);
    free(prompt);
    return rc;
}

static int cmd_session(int argc, char **argv)
{
    if (argc == 1) {
        printf("Current session: %s\n", s_current_session_id);
        return 0;
    }

    if (argc != 2) {
        printf("Usage: session [id]\n");
        return 1;
    }

    if (argv[1][0] == '\0') {
        printf("session id cannot be empty\n");
        return 1;
    }

    strlcpy(s_current_session_id, argv[1], sizeof(s_current_session_id));
    printf("Switched session to: %s\n", s_current_session_id);
    return 0;
}

static int cmd_cap_list(int argc, char **argv)
{
    claw_cap_list_t list;
    size_t i;

    (void)argc;
    (void)argv;

    list = claw_cap_list();
    if (list.count == 0) {
        printf("No capabilities registered\n");
        return 0;
    }

    for (i = 0; i < list.count; i++) {
        const claw_cap_descriptor_t *item = &list.items[i];

        printf("%s [%s] %s\n",
               item->name,
               item->family ? item->family : "cap",
               item->description ? item->description : "");
    }

    return 0;
}

static int cmd_cap_call(int argc, char **argv)
{
    char *output = NULL;
    esp_err_t err;
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
        .session_id = s_current_session_id,
    };

    if (argc < 3) {
        printf("Usage: cap_call <name> <json>\n");
        return 1;
    }

    {
        cJSON *json = cJSON_Parse(argv[2]);

        if (!json) {
            printf("invalid json\n");
            return 1;
        }
        cJSON_Delete(json);
    }

    output = calloc(1, CAP_OUTPUT_BUF_SIZE);
    if (!output) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call(argv[1], argv[2], &ctx, output, CAP_OUTPUT_BUF_SIZE);
    if (err == ESP_OK) {
        printf("%s\n", output);
    } else {
        printf("%s\n", output[0] ? output : esp_err_to_name(err));
    }

    free(output);
    return err == ESP_OK ? 0 : 1;
}

static int cmd_cap_groups(int argc, char **argv)
{
    claw_cap_group_list_t list;
    size_t i;

    (void)argc;
    (void)argv;

    list = claw_cap_list_groups();
    if (list.count == 0) {
        printf("No cap groups loaded\n");
        return 0;
    }

    for (i = 0; i < list.count; i++) {
        const claw_cap_group_info_t *item = &list.items[i];

        printf("%s state=%s descriptors=%u plugin=%s version=%s\n",
               item->group_id ? item->group_id : "(null)",
               claw_cap_state_to_string(item->state),
               (unsigned)item->descriptor_count,
               item->plugin_name ? item->plugin_name : "-",
               item->version ? item->version : "-");
    }

    return 0;
}

static int cmd_cap_enable(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: cap_enable <group_id>\n");
        return 1;
    }

    err = claw_cap_enable_group(argv[1]);
    if (err != ESP_OK) {
        printf("cap_enable failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("enabled %s\n", argv[1]);
    return 0;
}

static int cmd_cap_disable(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: cap_disable <group_id>\n");
        return 1;
    }

    err = claw_cap_disable_group(argv[1]);
    if (err != ESP_OK) {
        printf("cap_disable failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("disabled %s\n", argv[1]);
    return 0;
}

static int cmd_cap_unload(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: cap_unload <group_id>\n");
        return 1;
    }

    err = claw_cap_unregister_group(argv[1], 10000);
    if (err != ESP_OK) {
        printf("cap_unload failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("unloaded %s\n", argv[1]);
    return 0;
}

static int cmd_cap_load(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: cap_load <plugin>\n");
        return 1;
    }

#if CONFIG_APP_CLAW_CAP_IM_QQ
    if (strcmp(argv[1], "qq") == 0 || strcmp(argv[1], "cap_im_qq") == 0) {
        err = cap_im_qq_register_group();
    } else
#endif
    {
        printf("unknown plugin: %s\n", argv[1]);
        return 1;
    }

    if (err != ESP_OK) {
        printf("cap_load failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("loaded %s\n", argv[1]);
    return 0;
}

static int cmd_cap(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cap <list|call|groups|enable|disable|unload|load> ...\n");
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        return cmd_cap_list(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "call") == 0) {
        return cmd_cap_call(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "groups") == 0) {
        return cmd_cap_groups(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "enable") == 0) {
        return cmd_cap_enable(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "disable") == 0) {
        return cmd_cap_disable(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "unload") == 0) {
        return cmd_cap_unload(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "load") == 0) {
        return cmd_cap_load(argc - 1, &argv[1]);
    }

    printf("Unknown cap subcommand: %s\n", argv[1]);
    printf("Usage: cap <list|call|groups|enable|disable|unload|load> ...\n");
    return 1;
}

static int cmd_auto_reload(int argc, char **argv)
{
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = claw_event_router_reload();
    if (err != ESP_OK) {
        printf("auto_reload failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("automation rules reloaded\n");
    return 0;
}

static int cmd_auto_rules(int argc, char **argv)
{
    char *output = NULL;
    esp_err_t err;

    (void)argc;
    (void)argv;

    output = calloc(1, 4096);
    if (!output) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_event_router_list_rules_json(output, 4096);
    if (err != ESP_OK) {
        printf("auto_rules failed: %s\n", esp_err_to_name(err));
        free(output);
        return 1;
    }

    printf("%s\n", output);
    free(output);
    return 0;
}

static int cmd_auto_rule(int argc, char **argv)
{
    char *output = NULL;
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: auto_rule <id>\n");
        return 1;
    }

    output = calloc(1, 2048);
    if (!output) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_event_router_get_rule_json(argv[1], output, 2048);
    if (err != ESP_OK) {
        printf("auto_rule failed: %s\n", esp_err_to_name(err));
        free(output);
        return 1;
    }

    printf("%s\n", output);
    free(output);
    return 0;
}

static int cmd_auto_last(int argc, char **argv)
{
    claw_event_router_result_t result = {0};
    esp_err_t err;

    (void)argc;
    (void)argv;

    err = claw_event_router_get_last_result(&result);
    if (err != ESP_OK) {
        printf("auto_last failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("matched=%s matched_rules=%d action_count=%d failed_actions=%d route=%d handled_at_ms=%" PRId64 "\n",
           result.matched ? "true" : "false",
           result.matched_rules,
           result.action_count,
           result.failed_actions,
           (int)result.route,
           result.handled_at_ms);
    printf("first_rule_id=%s\n", result.first_rule_id[0] ? result.first_rule_id : "-");
    printf("ack=%s\n", result.ack[0] ? result.ack : "-");
    printf("last_error=%s\n", esp_err_to_name(result.last_error));
    return 0;
}

static int cmd_auto_add_rule(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: auto_add_rule <rule_json>\n");
        return 1;
    }

    err = claw_event_router_add_rule_json(argv[1]);
    if (err != ESP_OK) {
        printf("auto_add_rule failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("automation rule added\n");
    return 0;
}

static int cmd_auto_update_rule(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: auto_update_rule <rule_json>\n");
        return 1;
    }

    err = claw_event_router_update_rule_json(argv[1]);
    if (err != ESP_OK) {
        printf("auto_update_rule failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("automation rule updated\n");
    return 0;
}

static int cmd_auto_delete_rule(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 2) {
        printf("Usage: auto_delete_rule <id>\n");
        return 1;
    }

    err = claw_event_router_delete_rule(argv[1]);
    if (err != ESP_OK) {
        printf("auto_delete_rule failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("automation rule deleted\n");
    return 0;
}

static int cmd_auto_emit_message(int argc, char **argv)
{
    char *text = NULL;
    esp_err_t err;

    if (argc < 5) {
        printf("Usage: auto_emit_message <source_cap> <channel> <chat_id> <text>\n");
        return 1;
    }

    text = join_args_from(argc, argv, 4);
    if (!text) {
        printf("Out of memory\n");
        return 1;
    }

    err = claw_event_router_publish_message(argv[1], argv[2], argv[3], text, "console", "cli-msg");
    if (err != ESP_OK) {
        printf("auto_emit_message failed: %s\n", esp_err_to_name(err));
        free(text);
        return 1;
    }

    printf("message event published via %s to %s:%s\n", argv[1], argv[2], argv[3]);
    free(text);
    return 0;
}

static int cmd_auto_emit_trigger(int argc, char **argv)
{
    esp_err_t err;

    if (argc != 5) {
        printf("Usage: auto_emit_trigger <source_cap> <event_type> <event_key> <payload_json>\n");
        return 1;
    }

    {
        cJSON *json = cJSON_Parse(argv[4]);

        if (!json || !cJSON_IsObject(json)) {
            cJSON_Delete(json);
            printf("payload_json must be a JSON object\n");
            return 1;
        }
        cJSON_Delete(json);
    }

    err = claw_event_router_publish_trigger(argv[1], argv[2], argv[3], argv[4]);
    if (err != ESP_OK) {
        printf("auto_emit_trigger failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("trigger event published via %s type=%s key=%s\n", argv[1], argv[2], argv[3]);
    return 0;
}

static int cmd_auto(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: auto <reload|rules|rule|add_rule|update_rule|delete_rule|last|emit_message|emit_trigger> ...\n");
        return 1;
    }

    if (strcmp(argv[1], "reload") == 0) {
        return cmd_auto_reload(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "rules") == 0) {
        return cmd_auto_rules(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "rule") == 0) {
        return cmd_auto_rule(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "last") == 0) {
        return cmd_auto_last(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "add_rule") == 0) {
        return cmd_auto_add_rule(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "update_rule") == 0) {
        return cmd_auto_update_rule(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "delete_rule") == 0) {
        return cmd_auto_delete_rule(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "emit_message") == 0) {
        return cmd_auto_emit_message(argc - 1, &argv[1]);
    }
    if (strcmp(argv[1], "emit_trigger") == 0) {
        return cmd_auto_emit_trigger(argc - 1, &argv[1]);
    }

    printf("Unknown auto subcommand: %s\n", argv[1]);
    printf("Usage: auto <reload|rules|rule|add_rule|update_rule|delete_rule|last|emit_message|emit_trigger> ...\n");
    return 1;
}

static void register_cap_cli_commands(void)
{
#if CONFIG_APP_CLAW_CAP_IM_QQ
    register_cap_im_qq();
#endif
#if CONFIG_APP_CLAW_CAP_IM_FEISHU
    register_cap_im_feishu();
#endif
#if CONFIG_APP_CLAW_CAP_IM_TG
    register_cap_im_tg();
#endif
#if CONFIG_APP_CLAW_CAP_IM_WECHAT
    register_cap_im_wechat();
#endif
#if CONFIG_APP_CLAW_CAP_LUA
    register_cap_lua();
#endif
#if CONFIG_APP_CLAW_CAP_LLM_INSPECT
    register_cap_llm_inspect();
#endif
#if CONFIG_APP_CLAW_CAP_MCP_CLIENT
    register_cap_mcp_client();
#endif
#if CONFIG_APP_CLAW_CAP_MCP_SERVER
    register_cap_mcp_server();
#endif
#if CONFIG_APP_CLAW_CAP_ROUTER_MGR
    register_cap_router_mgr();
#endif
#if CONFIG_APP_CLAW_CAP_SCHEDULER
    register_cap_scheduler();
#endif
#if CONFIG_APP_CLAW_CAP_SKILL_MGR
    register_cap_skill();
#endif
#if CONFIG_APP_CLAW_CAP_TIME
    register_cap_time();
#endif
#if CONFIG_APP_CLAW_CAP_WEB_SEARCH
    register_cap_web_search();
#endif
}

esp_err_t app_claw_cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    ESP_LOGI(TAG, "Starting console REPL");

    repl_config.prompt = "app> ";
    repl_config.task_stack_size = 10240;
    repl_config.max_cmdline_length = 512;

#if CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif

    esp_console_register_help_command();
    register_cap_cli_commands();

    {
        esp_console_cmd_t ask_cmd = {
            .command = "ask",
            .help = "Submit a multi-turn prompt using the current session: ask <prompt>",
            .func = cmd_ask,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ask_cmd));
    }

    {
        esp_console_cmd_t ask_once_cmd = {
            .command = "ask_once",
            .help = "Submit a single-turn prompt without session history: ask_once <prompt>",
            .func = cmd_ask_once,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ask_once_cmd));
    }

    {
        esp_console_cmd_t session_cmd = {
            .command = "session",
            .help = "Show or switch the current session: session [id]",
            .func = cmd_session,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&session_cmd));
    }

    {
        esp_console_cmd_t cap_cmd = {
            .command = "cap",
            .help = "cap operations: cap <list|call|groups|enable|disable|unload|load> ...",
            .func = cmd_cap,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cap_cmd));
    }

    {
        esp_console_cmd_t auto_cmd = {
            .command = "auto",
            .help = "Automation operations: auto <reload|rules|rule|add_rule|update_rule|delete_rule|last|emit_message|emit_trigger> ...",
            .func = cmd_auto,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&auto_cmd));
    }

    printf("Type 'help', 'auto rules', 'auto last', or 'auto emit_message qq_gateway qq 123 hello'\n");
    return esp_console_start_repl(repl);
}
