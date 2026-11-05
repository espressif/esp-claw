/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cap_skill_mgr.h"
#include "claw_cap.h"
#include "claw_skill.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "unity.h"
#include "wear_levelling.h"

#define TEST_BASE_PATH       "/testfs"
#define TEST_PARTITION_LABEL "storage"
#define TEST_RUNTIME_ROOT    TEST_BASE_PATH "/skills"
#define TEST_READONLY_ROOT   TEST_BASE_PATH "/system_skills"
#define TEST_SESSION_ROOT    TEST_BASE_PATH "/sessions"
#define TEST_SKILL_ID        "runtime_test"
#define TEST_READONLY_ID     "readonly_test"
#define TEST_INVALID_ID      "invalid_test"
#define TEST_OUTPUT_SIZE     2048

static const char *TAG = "skill_runtime_test";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static char s_output[TEST_OUTPUT_SIZE];

static void make_dir(const char *path)
{
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "mkdir failed: path=%s errno=%d", path, errno);
        TEST_FAIL_MESSAGE("failed to create test directory");
    }
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL(strlen(text), fwrite(text, 1, strlen(text), file));
    TEST_ASSERT_EQUAL(0, fclose(file));
}

static bool path_exists(const char *path)
{
    struct stat st = {0};
    return stat(path, &st) == 0;
}

static void create_skill_fixture(void)
{
    make_dir(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID);
    make_dir(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID "/scripts");
    make_dir(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID "/references");
    make_dir(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID "/assets");
    write_text(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID "/SKILL.md",
               "---\n"
               "{\"name\":\"runtime_test\",\"description\":\"Runtime test skill.\","
               "\"metadata\":{\"cap_groups\":[\"cap_skill_manage\"]}}\n"
               "---\n\n"
               "Run {CUR_SKILL_DIR}/scripts/main.lua\n");
    write_text(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID "/scripts/main.lua", "return true\n");
    write_text(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID "/references/readme.md", "reference\n");
    write_text(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID "/assets/icon.jpg", "jpeg\n");
}

static void create_readonly_fixture(void)
{
    make_dir(TEST_READONLY_ROOT "/" TEST_READONLY_ID);
    write_text(TEST_READONLY_ROOT "/" TEST_READONLY_ID "/skill.md",
               "---\n"
               "{\"name\":\"readonly_test\",\"description\":\"Readonly test skill.\"}\n"
               "---\n\nReadonly content.\n");
}

static void create_invalid_fixture(void)
{
    make_dir(TEST_RUNTIME_ROOT "/" TEST_INVALID_ID);
    write_text(TEST_RUNTIME_ROOT "/" TEST_INVALID_ID "/SKILL.md",
               "---\n"
               "{\"name\":\"invalid_test\",\"description\":}\n"
               "---\n\nInvalid content.\n");
}

typedef struct {
    bool found_valid;
    bool found_readonly;
    bool found_invalid;
} skill_catalog_result_t;

static esp_err_t collect_skill(const claw_skill_catalog_entry_t *entry, void *user_ctx)
{
    skill_catalog_result_t *result = user_ctx;
    result->found_valid |= strcmp(entry->id, TEST_SKILL_ID) == 0;
    result->found_readonly |= strcmp(entry->id, TEST_READONLY_ID) == 0;
    result->found_invalid |= strcmp(entry->id, TEST_INVALID_ID) == 0;
    return ESP_OK;
}

TEST_CASE("skill manager exposes only the compact tool surface", "[skill][cap]")
{
    const claw_cap_descriptor_t *list = claw_cap_find("list_skill");
    const claw_cap_descriptor_t *activate = claw_cap_find("activate_skill");
    const claw_cap_descriptor_t *publish = claw_cap_find("publish_skill");
    const claw_cap_descriptor_t *remove = claw_cap_find("remove_skill");

    TEST_ASSERT_NOT_NULL(list);
    TEST_ASSERT_NOT_NULL(activate);
    TEST_ASSERT_NOT_NULL(publish);
    TEST_ASSERT_NOT_NULL(remove);
    TEST_ASSERT_EQUAL_UINT32(0, list->cap_flags & CLAW_CAP_FLAG_CALLABLE_BY_LLM);
    TEST_ASSERT_NOT_EQUAL(0, activate->cap_flags & CLAW_CAP_FLAG_CALLABLE_BY_LLM);
    TEST_ASSERT_NOT_EQUAL(0, publish->cap_flags & CLAW_CAP_FLAG_CALLABLE_BY_LLM);
    TEST_ASSERT_NOT_EQUAL(0, remove->cap_flags & CLAW_CAP_FLAG_CALLABLE_BY_LLM);
    TEST_ASSERT_NULL(claw_cap_find("register_skill"));
    TEST_ASSERT_NULL(claw_cap_find("unregister_skill"));
    TEST_ASSERT_NULL(claw_cap_find("set_skill_launcher"));
    TEST_ASSERT_NULL(claw_cap_find("remove_skill_launcher"));
}

TEST_CASE("skill tools publish activate and remove complete runtime directory", "[skill][runtime]")
{
    const char *visible_groups[] = {"cap_skill"};
    claw_cap_call_context_t system_ctx = {.caller = CLAW_CAP_CALLER_SYSTEM};
    claw_cap_call_context_t agent_ctx = {
        .session_id = "skill-runtime-session",
        .caller = CLAW_CAP_CALLER_AGENT,
    };
    bool deleted_session = false;

    memset(s_output, 0, sizeof(s_output));
    TEST_ASSERT_EQUAL(ESP_OK, claw_skill_delete_session_state(agent_ctx.session_id, &deleted_session));
    TEST_ASSERT_EQUAL(ESP_OK, claw_cap_set_session_llm_visible_groups(agent_ctx.session_id, NULL, 0));
    create_skill_fixture();
    create_readonly_fixture();
    TEST_ASSERT_EQUAL(ESP_OK, claw_cap_set_llm_visible_groups(visible_groups, 1));

    /* Management tools stay hidden until the fixture skill is activated. */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      claw_cap_call("remove_skill", "{\"skill_id\":\"runtime_test\"}", &agent_ctx, s_output, sizeof(s_output)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      claw_cap_call("publish_skill", "{\"skill_id\":\"runtime_test\",\"file\":\"SKILL.md\"}", &system_ctx, s_output, sizeof(s_output)));

    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_cap_call("publish_skill", "{\"skill_id\":\"runtime_test\"}", &system_ctx, s_output, sizeof(s_output)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"skill_id\":\"runtime_test\"}", s_output);

    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_cap_call("activate_skill", "{\"skill_id\":\"runtime_test\"}", &agent_ctx, s_output, sizeof(s_output)));
    TEST_ASSERT_NOT_NULL(strstr(s_output, "<skill_content name=\"runtime_test\">"));
    TEST_ASSERT_NOT_NULL(strstr(s_output, TEST_RUNTIME_ROOT "/runtime_test/scripts/main.lua"));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE,
                      claw_cap_call("remove_skill", "{\"skill_id\":\"readonly_test\"}", &agent_ctx, s_output, sizeof(s_output)));
    TEST_ASSERT_NOT_NULL(strstr(s_output, "\"code\":\"readonly_skill\""));
    TEST_ASSERT_TRUE(path_exists(TEST_READONLY_ROOT "/" TEST_READONLY_ID));

    TEST_ASSERT_EQUAL(ESP_OK,
                      claw_cap_call("remove_skill", "{\"skill_id\":\"runtime_test\"}", &agent_ctx, s_output, sizeof(s_output)));
    TEST_ASSERT_EQUAL_STRING("{\"ok\":true,\"skill_id\":\"runtime_test\"}", s_output);
    /* The complete payload tree must be gone after removal. */
    TEST_ASSERT_FALSE(path_exists(TEST_RUNTIME_ROOT "/" TEST_SKILL_ID));
}

TEST_CASE("skill registry skips invalid skill metadata", "[skill][registry]")
{
    skill_catalog_result_t result = {0};

    create_skill_fixture();
    create_invalid_fixture();
    TEST_ASSERT_EQUAL(ESP_OK, claw_skill_reload_registry());
    TEST_ASSERT_EQUAL(ESP_OK, claw_skill_foreach_catalog_entry(collect_skill, &result));
    TEST_ASSERT_TRUE(result.found_valid);
    TEST_ASSERT_TRUE(result.found_readonly);
    TEST_ASSERT_FALSE(result.found_invalid);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_skill_publish(TEST_INVALID_ID));
}

static void init_test_runtime(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 16,
        .allocation_unit_size = 4096,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };
    const claw_skill_config_t skill_config = {
        .session_state_root_dir = TEST_SESSION_ROOT,
        .max_file_bytes = 8192,
    };

    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(TEST_BASE_PATH, TEST_PARTITION_LABEL, &mount_config, &s_wl_handle));
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_format_cfg_rw_wl(TEST_BASE_PATH, TEST_PARTITION_LABEL, &mount_config));
    make_dir(TEST_RUNTIME_ROOT);
    make_dir(TEST_READONLY_ROOT);
    make_dir(TEST_SESSION_ROOT);
    ESP_ERROR_CHECK(claw_skill_init(&skill_config));
    ESP_ERROR_CHECK(claw_skill_add_directory(TEST_RUNTIME_ROOT));
    ESP_ERROR_CHECK(claw_skill_add_directory(TEST_READONLY_ROOT));
    ESP_ERROR_CHECK(claw_skill_reload_registry());
    ESP_ERROR_CHECK(claw_cap_init());
    ESP_ERROR_CHECK(cap_skill_mgr_register_group());
    ESP_ERROR_CHECK(claw_cap_start_all());
}

void app_main(void)
{
    init_test_runtime();
    ESP_LOGI(TAG, "Starting skill runtime tests");
    unity_run_menu();
}
