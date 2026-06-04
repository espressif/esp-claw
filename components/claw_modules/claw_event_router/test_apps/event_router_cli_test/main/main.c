/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "cap_router_mgr.h"
#include "cmd_cap_router_mgr.h"
#include "claw_cap.h"
#include "claw_event_router.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wear_levelling.h"

static const char *TAG = "event_router_test";

#define TEST_FATFS_BASE_PATH       "/tmp"
#define TEST_FATFS_PARTITION_LABEL "storage"
#define TEST_AUTOMATION_DIR        TEST_FATFS_BASE_PATH "/auto"
#define TEST_RULES_PATH            TEST_AUTOMATION_DIR "/rules"

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;

static const char *s_seed_rules_json =
    "[]\n";

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    return err;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    esp_err_t err;

    err = esp_vfs_fat_spiflash_mount_rw_wl(TEST_FATFS_BASE_PATH,
                                           TEST_FATFS_PARTITION_LABEL,
                                           &mount_config,
                                           &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t ensure_dir(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to create directory %s: errno=%d", path, errno);
    return ESP_FAIL;
}

static esp_err_t write_text_file(const char *path, const char *content)
{
    FILE *file = NULL;

    if (!path || !content) {
        return ESP_ERR_INVALID_ARG;
    }

    file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open %s for writing: errno=%d", path, errno);
        return ESP_FAIL;
    }

    if (fwrite(content, 1, strlen(content), file) != strlen(content)) {
        fclose(file);
        ESP_LOGE(TAG, "Failed to write %s", path);
        return ESP_FAIL;
    }

    fclose(file);
    return ESP_OK;
}

static esp_err_t prepare_rules_file(void)
{
    ESP_RETURN_ON_ERROR(ensure_dir(TEST_AUTOMATION_DIR), TAG, "Failed to prepare automation dir");
    return write_text_file(TEST_RULES_PATH, s_seed_rules_json);
}

static esp_err_t init_console(void)
{
    esp_console_config_t console_config = {
        .max_cmdline_length = 512,
        .max_cmdline_args = 32,
    };

    ESP_RETURN_ON_ERROR(esp_console_init(&console_config), TAG, "Failed to init console");
    esp_console_register_help_command();
    ESP_RETURN_ON_ERROR(claw_cap_init(), TAG, "Failed to init claw_cap");
    ESP_RETURN_ON_ERROR(cap_router_mgr_register_group(), TAG, "Failed to register router manager cap");
    ESP_RETURN_ON_ERROR(claw_cap_start_all(), TAG, "Failed to start capabilities");
    register_cap_router_mgr();
    return ESP_OK;
}

static esp_err_t init_event_router(void)
{
    claw_event_router_config_t config = {
        .rules_path = TEST_RULES_PATH,
        .event_queue_len = 4,
        .task_stack_size = 4096,
        .task_priority = 4,
        .task_core = tskNO_AFFINITY,
        .default_route_messages_to_agent = false,
    };

    ESP_RETURN_ON_ERROR(claw_event_router_init(&config), TAG, "Failed to init event router");
    return claw_event_router_start();
}

static esp_err_t run_cli_capture(const char *command_line, char **out_stdout_text, int *out_cmd_ret)
{
    FILE *capture = NULL;
    FILE *saved_stdout = NULL;
    char *buffer = NULL;
    size_t buffer_len = 0;
    esp_err_t run_err;

    if (!command_line || !out_stdout_text || !out_cmd_ret) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_stdout_text = NULL;
    *out_cmd_ret = -1;

    capture = open_memstream(&buffer, &buffer_len);
    if (!capture) {
        return ESP_FAIL;
    }

    fflush(stdout);
    saved_stdout = stdout;
    stdout = capture;
    run_err = esp_console_run(command_line, out_cmd_ret);
    fflush(stdout);
    stdout = saved_stdout;

    if (fclose(capture) != 0) {
        free(buffer);
        return ESP_FAIL;
    }

    if (!buffer) {
        buffer = calloc(1, 1);
        if (!buffer) {
            return ESP_ERR_NO_MEM;
        }
    }

    *out_stdout_text = buffer;
    return run_err;
}

static bool output_contains(const char *text, const char *needle)
{
    if (!needle || !needle[0]) {
        return true;
    }

    return text && strstr(text, needle) != NULL;
}

static bool run_cli_and_check(const char *label,
                              const char *command_line,
                              const char *expect_1,
                              const char *expect_2,
                              TickType_t wait_after_ticks)
{
    char *stdout_text = NULL;
    int cmd_ret = -1;
    esp_err_t run_err;
    bool passed = true;

    ESP_LOGI(TAG, "[RUN] %s", label);
    ESP_LOGI(TAG, "cmd: %s", command_line);

    run_err = run_cli_capture(command_line, &stdout_text, &cmd_ret);
    if (run_err != ESP_OK) {
        ESP_LOGE(TAG, "Command dispatch failed: %s", esp_err_to_name(run_err));
        free(stdout_text);
        return false;
    }

    ESP_LOGI(TAG, "ret=%d", cmd_ret);
    ESP_LOGI(TAG, "stdout:\n%s", stdout_text ? stdout_text : "");

    if (cmd_ret != 0) {
        ESP_LOGE(TAG, "Command returned non-zero status");
        passed = false;
    }
    if (!output_contains(stdout_text, expect_1)) {
        ESP_LOGE(TAG, "Missing expected output: %s", expect_1);
        passed = false;
    }
    if (!output_contains(stdout_text, expect_2)) {
        ESP_LOGE(TAG, "Missing expected output: %s", expect_2);
        passed = false;
    }

    free(stdout_text);

    if (passed && wait_after_ticks > 0) {
        vTaskDelay(wait_after_ticks);
    }

    ESP_LOGI(TAG, "[%s] %s", passed ? "PASS" : "FAIL", label);
    return passed;
}

static bool run_agent_without_submit_check(void)
{
    static const char *agent_rule_json =
        "{\"id\":\"agent_missing_submit\",\"enabled\":true,\"consume_on_match\":true,"
        "\"match\":{\"event_type\":\"message\",\"content_type\":\"text\",\"source_cap\":\"test_source\","
        "\"channel\":\"cli\",\"chat_id\":\"room_agent\",\"text\":\"agent please\"},"
        "\"actions\":[{\"type\":\"run_agent\"}]}";
    claw_event_t event = {
        .source_cap = "test_source",
        .event_type = "message",
        .source_channel = "cli",
        .chat_id = "room_agent",
        .content_type = "text",
        .session_policy = CLAW_SESSION_POLICY_CHAT,
    };
    claw_event_router_result_t result = {0};
    esp_err_t err;
    bool passed;

    ESP_LOGI(TAG, "[RUN] run_agent_without_submit");
    err = claw_event_router_add_rule_json(agent_rule_json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add run_agent rule: %s", esp_err_to_name(err));
        return false;
    }

    event.text = strdup("agent please");
    if (!event.text) {
        (void)claw_event_router_delete_rule("agent_missing_submit");
        return false;
    }

    err = claw_event_router_handle_event(&event, &result);
    passed = err == ESP_ERR_INVALID_STATE || result.last_error == ESP_ERR_INVALID_STATE;
    if (!passed) {
        ESP_LOGE(TAG,
                 "run_agent without submit returned err=%s last_error=%s failed_actions=%d",
                 esp_err_to_name(err),
                 esp_err_to_name(result.last_error),
                 result.failed_actions);
    }

    claw_event_free(&event);
    (void)claw_event_router_delete_rule("agent_missing_submit");
    ESP_LOGI(TAG, "[%s] run_agent_without_submit", passed ? "PASS" : "FAIL");
    return passed;
}

static bool run_smoke_suite(void)
{
    bool ok = true;

    ok = run_cli_and_check("list_empty_rules",
                           "event_router --rules",
                           "[]",
                           NULL,
                           0) && ok;

    ok = run_cli_and_check("add_message_rule",
                           "event_router --add-rule-json "
                           "{\"id\":\"msg_drop\",\"description\":\"drop_message\",\"ack\":\"message_ack_v1\","
                           "\"match\":{\"event_type\":\"message\",\"event_key\":\"text\",\"content_type\":\"text\","
                           "\"source_cap\":\"test_source\",\"source_channel\":\"cli\",\"chat_id\":\"room1\","
                           "\"text\":\"hello_router\"},\"actions\":[{\"type\":\"drop\"}]}",
                           "\"ok\":true",
                           NULL,
                           0) && ok;

    ok = run_cli_and_check("read_message_rule",
                           "event_router --rule msg_drop",
                           "\"id\":\"msg_drop\"",
                           "\"ack\":\"message_ack_v1\"",
                           0) && ok;

    ok = run_cli_and_check("update_message_rule",
                           "event_router --update-rule-json "
                           "{\"id\":\"msg_drop\",\"description\":\"drop_message_updated\",\"ack\":\"message_ack_v2\","
                           "\"match\":{\"event_type\":\"message\",\"event_key\":\"text\",\"content_type\":\"text\","
                           "\"source_cap\":\"test_source\",\"source_channel\":\"cli\",\"chat_id\":\"room1\","
                           "\"text\":\"hello_router\"},\"actions\":[{\"type\":\"drop\"}]}",
                           "\"ok\":true",
                           NULL,
                           0) && ok;

    ok = run_cli_and_check("reload_rules",
                           "event_router --reload",
                           "\"action\":\"reload_router_rules\"",
                           NULL,
                           0) && ok;

    ok = run_cli_and_check("emit_message",
                           "event_router --emit-message --source-cap test_source "
                           "--channel cli --chat-id room1 --text hello_router",
                           "message event published via test_source to cli:room1",
                           NULL,
                           pdMS_TO_TICKS(200)) && ok;

    ok = run_cli_and_check("check_last_message_result",
                           "event_router --last",
                           "first_rule_id=msg_drop",
                           "ack=message_ack_v2",
                           0) && ok;

    ok = run_cli_and_check("add_trigger_rule",
                           "event_router --add-rule-json "
                           "{\"id\":\"trigger_drop\",\"description\":\"drop_trigger\",\"ack\":\"trigger_ack\","
                           "\"match\":{\"event_type\":\"doorbell\",\"event_key\":\"ding\",\"source_cap\":\"test_source\"},"
                           "\"actions\":[{\"type\":\"drop\"}]}",
                           "\"ok\":true",
                           NULL,
                           0) && ok;

    ok = run_cli_and_check("emit_trigger",
                           "event_router --emit-trigger --source-cap test_source "
                           "--event-type doorbell --event-key ding --payload-json {\"state\":\"on\"}",
                           "trigger event published via test_source type=doorbell key=ding",
                           NULL,
                           pdMS_TO_TICKS(200)) && ok;

    ok = run_cli_and_check("check_last_trigger_result",
                           "event_router --last",
                           "first_rule_id=trigger_drop",
                           "ack=trigger_ack",
                           0) && ok;

    ok = run_cli_and_check("delete_message_rule",
                           "event_router --delete-rule msg_drop",
                           "\"action\":\"delete_router_rule\"",
                           NULL,
                           0) && ok;

    ok = run_cli_and_check("delete_trigger_rule",
                           "event_router --delete-rule trigger_drop",
                           "\"action\":\"delete_router_rule\"",
                           NULL,
                           0) && ok;

    ok = run_cli_and_check("list_rules_after_cleanup",
                           "event_router --rules",
                           "[]",
                           NULL,
                           0) && ok;

    ok = run_agent_without_submit_check() && ok;

    return ok;
}

void app_main(void)
{
    bool passed;

    ESP_LOGI(TAG, "Starting event_router CLI smoke app");
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(prepare_rules_file());
    ESP_ERROR_CHECK(init_console());
    ESP_ERROR_CHECK(init_event_router());

    passed = run_smoke_suite();
    if (!passed) {
        ESP_LOGE(TAG, "CLI smoke test failed");
        abort();
    }

    ESP_LOGI(TAG, "CLI smoke test passed");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}
