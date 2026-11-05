/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_registry.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "unity.h"
#include "wear_levelling.h"

#define TEST_BASE_PATH "/testfs"
#define TEST_PARTITION_LABEL "storage"
#define TEST_RUNTIME_ROOT TEST_BASE_PATH "/apps"
#define TEST_SYSTEM_ROOT TEST_BASE_PATH "/system_apps"

static const char *TAG = "app_runtime_test";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static size_t s_change_count;

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

static void create_app(const char *root, const char *dir_id, const char *manifest_id, const char *display_name, bool with_icon)
{
    char path[128];
    char *manifest = calloc(1, 384);

    TEST_ASSERT_NOT_NULL(manifest);

    snprintf(path, sizeof(path), "%s/%s", root, dir_id);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/%s/scripts", root, dir_id);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/%s/assets", root, dir_id);
    make_dir(path);
    snprintf(path, sizeof(path), "%s/%s/scripts/main.lua", root, dir_id);
    write_text(path, "return true\n");
    if (with_icon) {
        snprintf(path, sizeof(path), "%s/%s/assets/icon.jpg", root, dir_id);
        write_text(path, "jpeg\n");
    }
    snprintf(manifest, 384,
             "{\"schema_version\":1,\"id\":\"%s\",\"display_name\":\"%s\",\"entry\":\"scripts/main.lua\","
             "\"icon\":\"assets/icon.jpg\",\"args\":{\"mode\":\"test\"},\"order\":7,\"visible\":true}",
             manifest_id, display_name);
    snprintf(path, sizeof(path), "%s/%s/launcher.json", root, dir_id);
    write_text(path, manifest);
    free(manifest);
}

typedef struct {
    size_t count;
    bool found;
    bool valid;
    bool default_icon;
    bool runtime_display_name;
    app_registry_manage_mode_t mode;
} catalog_result_t;

static esp_err_t collect_demo(const app_registry_entry_t *entry, void *user_ctx)
{
    catalog_result_t *result = user_ctx;

    result->count++;
    if (strcmp(entry->app_id, "demo") == 0) {
        result->found = true;
        result->valid = entry->app_dir && entry->entry && strstr(entry->entry, "/demo/scripts/main.lua") &&
                        entry->args_json && strcmp(entry->args_json, "{\"mode\":\"test\"}") == 0 && entry->order == 7 && entry->visible;
        result->default_icon = entry->icon == NULL;
        result->runtime_display_name = strcmp(entry->display_name, "Runtime Demo") == 0;
        result->mode = entry->manage_mode;
    }
    return ESP_OK;
}

static catalog_result_t read_catalog(void)
{
    catalog_result_t result = {0};
    TEST_ASSERT_EQUAL(ESP_OK, app_registry_foreach(collect_demo, &result));
    return result;
}

static void registry_changed(void *user_ctx)
{
    (void)user_ctx;
    s_change_count++;
}

TEST_CASE("App registry keeps runtime and system packages independent", "[app][registry]")
{
    catalog_result_t result = read_catalog();
    TEST_ASSERT_EQUAL(2, result.count);
    TEST_ASSERT_TRUE(result.found);
    TEST_ASSERT_TRUE(result.valid);
    TEST_ASSERT_TRUE(result.default_icon);
    TEST_ASSERT_EQUAL(APP_REGISTRY_MANAGE_MODE_READONLY, result.mode);

    create_app(TEST_RUNTIME_ROOT, "demo", "demo", "Runtime Demo", true);
    size_t before_publish = s_change_count;
    TEST_ASSERT_EQUAL(ESP_OK, app_registry_publish("demo"));
    TEST_ASSERT_EQUAL(before_publish + 1, s_change_count);
    result = read_catalog();
    TEST_ASSERT_EQUAL(2, result.count);
    TEST_ASSERT_TRUE(result.runtime_display_name);
    TEST_ASSERT_FALSE(result.default_icon);
    TEST_ASSERT_EQUAL(APP_REGISTRY_MANAGE_MODE_RUNTIME, result.mode);

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, app_registry_remove("readonly"));
    TEST_ASSERT_TRUE(path_exists(TEST_SYSTEM_ROOT "/readonly"));
    TEST_ASSERT_EQUAL(ESP_OK, app_registry_remove("demo"));
    TEST_ASSERT_FALSE(path_exists(TEST_RUNTIME_ROOT "/demo"));
    result = read_catalog();
    TEST_ASSERT_EQUAL(2, result.count);
    TEST_ASSERT_EQUAL(APP_REGISTRY_MANAGE_MODE_READONLY, result.mode);
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

    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_mount_rw_wl(TEST_BASE_PATH, TEST_PARTITION_LABEL, &mount_config, &s_wl_handle));
    ESP_ERROR_CHECK(esp_vfs_fat_spiflash_format_cfg_rw_wl(TEST_BASE_PATH, TEST_PARTITION_LABEL, &mount_config));
    make_dir(TEST_RUNTIME_ROOT);
    make_dir(TEST_SYSTEM_ROOT);
    create_app(TEST_SYSTEM_ROOT, "demo", "demo", "System Demo", false);
    create_app(TEST_SYSTEM_ROOT, "readonly", "readonly", "Read Only", true);
    create_app(TEST_SYSTEM_ROOT, "invalid", "wrong_id", "Invalid", true);
    ESP_ERROR_CHECK(app_registry_init());
    ESP_ERROR_CHECK(app_registry_add_directory(TEST_RUNTIME_ROOT));
    ESP_ERROR_CHECK(app_registry_add_directory(TEST_SYSTEM_ROOT));
    ESP_ERROR_CHECK(app_registry_register_changed_cb(registry_changed, NULL));
    ESP_ERROR_CHECK(app_registry_reload());
}

void app_main(void)
{
    init_test_runtime();
    ESP_LOGI(TAG, "Starting App runtime tests");
    unity_run_menu();
}
