/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "app_claw_cli.h"
#include "app_claw.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "esp_idf_version.h"

#if CONFIG_APP_CLAW_CAP_LUA
#include "cmd_cap_lua.h"
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
#include "claw_cap.h"
#include "claw_agent_mgr.h"
#include "claw_core.h"
#include "claw_hw_registry.h"
#include "cJSON.h"
#include "esp_console.h"
#include "esp_log.h"

static const char *TAG = "app_claw_cli";
static const size_t CAP_OUTPUT_BUF_SIZE = 8192;

static uint32_t s_next_request_id = 1;
static char s_current_session_id[64] = "default";

static ssize_t app_claw_cli_read_blocking(int fd, void *buffer, size_t size)
{
    for (;;) {
        ssize_t ret = read(fd, buffer, size);
        if (ret >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return ret;
        }
        vTaskDelay(1);
    }
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
    claw_core_response_t response = {0};
    uint32_t request_id = 0;
    esp_err_t err;

    if (session_id && session_id[0]) {
        printf("Submitting request %" PRIu32 " [session=%s]...\n",
               s_next_request_id,
               session_id);
    } else {
        printf("Submitting request %" PRIu32 " [single-turn]...\n", s_next_request_id);
    }

    if (!app_claw_get_core()) {
        printf("claw_core is not ready\n");
        return 1;
    }

    err = claw_agent_mgr_start_root_run_text(
        prompt, session_id, CLAW_CORE_REQUEST_FLAG_PUBLISH_STAGE_MESSAGE,
        5000, &request_id);
    if (err != ESP_OK) {
        printf("submit failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    s_next_request_id = request_id + 1;

    err = claw_agent_mgr_receive_root_for(request_id, &response, 130000);
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
    const char *session_id = s_current_session_id;
    int prompt_index = 1;
    int rc;

    if (argc > 1 && strcmp(argv[1], "--once") == 0) {
        session_id = NULL;
        prompt_index++;
    }
    if (argc <= prompt_index) {
        printf("Usage: ask [--once] <prompt>\n");
        return 1;
    }

    prompt = join_args_from(argc, argv, prompt_index);
    if (!prompt) {
        printf("Out of memory\n");
        return 1;
    }

    rc = submit_and_print(prompt, session_id);
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
        .core = app_claw_get_core(),
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

static int cmd_cap(int argc, char **argv)
{
    if (argc < 2) {
        printf("Usage: cap <list|call|groups> ...\n");
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
    printf("Unknown cap subcommand: %s\n", argv[1]);
    printf("Usage: cap <list|call|groups> ...\n");
    return 1;
}

#if CONFIG_APP_CLAW_CAP_FILES
static int cmd_ls(int argc, char **argv)
{
    claw_cap_call_context_t ctx = {
        .caller = CLAW_CAP_CALLER_CONSOLE,
        .session_id = s_current_session_id,
        .core = app_claw_get_core(),
    };
    cJSON *input = NULL;
    char *input_json = NULL;
    char *output = NULL;
    esp_err_t err;

    if (argc > 2) {
        printf("Usage: ls [keyword]\n");
        return 1;
    }

    input = cJSON_CreateObject();
    if (!input || (argc == 2 && !cJSON_AddStringToObject(input, "keyword", argv[1]))) {
        cJSON_Delete(input);
        printf("Out of memory\n");
        return 1;
    }
    input_json = cJSON_PrintUnformatted(input);
    cJSON_Delete(input);
    output = calloc(1, CAP_OUTPUT_BUF_SIZE);
    if (!input_json || !output) {
        free(input_json);
        free(output);
        printf("Out of memory\n");
        return 1;
    }

    err = claw_cap_call("list_dir", input_json, &ctx, output, CAP_OUTPUT_BUF_SIZE);
    printf("%s\n", output[0] ? output : esp_err_to_name(err));
    free(input_json);
    free(output);
    return err == ESP_OK ? 0 : 1;
}
#endif

/* `hwmap` — dump the claw_hw_registry state or query one resource. */
typedef struct {
    int count;
} hwmap_ctx_t;

static const char *hwmap_mode_str(claw_hw_mode_t mode)
{
    switch (mode) {
    case CLAW_HW_MODE_EXCLUSIVE:  return "EXCLUSIVE";
    case CLAW_HW_MODE_SHARED_READ: return "SHARED_READ";
    default: return "?";
    }
}

static void hwmap_iter_cb(const char *resource, const char *owner_tag,
                          claw_hw_mode_t mode, void *user_ctx)
{
    hwmap_ctx_t *ctx = (hwmap_ctx_t *)user_ctx;
    ctx->count++;
    printf("  %-11s  %-32s  %s\n",
           hwmap_mode_str(mode),
           resource ? resource : "?",
           owner_tag ? owner_tag : "?");
}

static int cmd_hwmap(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "query") == 0) {
        if (argc < 3) {
            printf("Usage: hwmap query <resource>\n");
            return 1;
        }
        const char *holder = NULL;
        esp_err_t err = claw_hw_query(argv[2], &holder);
        if (err == ESP_OK && holder) {
            printf("%s -> %s\n", argv[2], holder);
        } else {
            printf("%s -> (unclaimed)\n", argv[2]);
        }
        return 0;
    }

    printf("Active hardware leases:\n");
    printf("  %-11s  %-32s  %s\n", "MODE", "RESOURCE", "OWNER");
    hwmap_ctx_t ctx = {0};
    esp_err_t err = claw_hw_foreach(hwmap_iter_cb, &ctx);
    if (err != ESP_OK) {
        printf("hwmap: iteration failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("(%d entries)\n", ctx.count);
    return 0;
}

static void register_cap_cli_commands(void)
{
#if CONFIG_APP_CLAW_CAP_LUA
    register_cap_lua();
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
}

esp_err_t app_claw_cli_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();

    ESP_LOGI(TAG, "Starting console REPL");

    repl_config.prompt = "app> ";
    repl_config.task_stack_size = 10240;
    repl_config.max_cmdline_length = 512;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT || CONFIG_ESP_CONSOLE_UART_CUSTOM
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl));
#elif CONFIG_ESP_CONSOLE_USB_CDC
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl));
#else
    ESP_LOGE(TAG, "No supported console backend is enabled");
    return ESP_ERR_NOT_SUPPORTED;
#endif
    linenoiseSetReadFunction(app_claw_cli_read_blocking);

    register_cap_cli_commands();

    {
        esp_console_cmd_t ask_cmd = {
            .command = "ask",
            .help = "Submit a prompt using the current session, or use --once for a single turn",
            .func = cmd_ask,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ask_cmd));
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
            .help = "Capability operations: cap <list|call|groups> ...",
            .func = cmd_cap,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cap_cmd));
    }

#if CONFIG_APP_CLAW_CAP_FILES
    {
        esp_console_cmd_t ls_cmd = {
            .command = "ls",
            .help = "List files under DATA and SYSTEM roots, optionally filtered by keyword",
            .func = cmd_ls,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&ls_cmd));
    }
#endif

    {
        esp_console_cmd_t hwmap_cmd = {
            .command = "hwmap",
            .help = "Dump the hardware lease registry: hwmap [query <resource>]",
            .func = cmd_hwmap,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&hwmap_cmd));
    }

    printf("Type 'help' to list commands, or 'ls [keyword]' to list files\n");
    return esp_console_start_repl(repl);
}
