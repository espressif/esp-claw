/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cap_lua.h"
#include "claw_cap.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "lua_module_delay.h"
#include "lua_module_thread.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "wear_levelling.h"

#define TEST_FATFS_BASE_PATH       "/fatfs"
#define TEST_FATFS_PARTITION_LABEL "storage"
#define TEST_LUA_BASE_DIR          TEST_FATFS_BASE_PATH "/scripts"
#define TEST_THREAD_DIR            TEST_FATFS_BASE_PATH "/thread_test"
#define TEST_HARNESS_PATH          TEST_THREAD_DIR "/harness.lua"
#define TEST_OUTPUT_SIZE           4096

static const char *TAG = "thread_test_app";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static char s_lua_case_output[TEST_OUTPUT_SIZE];
static char s_lua_teardown_output[TEST_OUTPUT_SIZE];

extern const uint8_t lua_blocking_recv_lua_start[] asm("_binary_blocking_recv_lua_start");
extern const uint8_t lua_blocking_recv_lua_end[] asm("_binary_blocking_recv_lua_end");
extern const uint8_t lua_child_a_lua_start[] asm("_binary_child_a_lua_start");
extern const uint8_t lua_child_a_lua_end[] asm("_binary_child_a_lua_end");
extern const uint8_t lua_child_b_lua_start[] asm("_binary_child_b_lua_start");
extern const uint8_t lua_child_b_lua_end[] asm("_binary_child_b_lua_end");
extern const uint8_t lua_consumer_lua_start[] asm("_binary_consumer_lua_start");
extern const uint8_t lua_consumer_lua_end[] asm("_binary_consumer_lua_end");
extern const uint8_t lua_echo_child_lua_start[] asm("_binary_echo_child_lua_start");
extern const uint8_t lua_echo_child_lua_end[] asm("_binary_echo_child_lua_end");
extern const uint8_t lua_harness_lua_start[] asm("_binary_harness_lua_start");
extern const uint8_t lua_harness_lua_end[] asm("_binary_harness_lua_end");
extern const uint8_t lua_lock_holder_lua_start[] asm("_binary_lock_holder_lua_start");
extern const uint8_t lua_lock_holder_lua_end[] asm("_binary_lock_holder_lua_end");
extern const uint8_t lua_lock_waiter_lua_start[] asm("_binary_lock_waiter_lua_start");
extern const uint8_t lua_lock_waiter_lua_end[] asm("_binary_lock_waiter_lua_end");
extern const uint8_t lua_producer_lua_start[] asm("_binary_producer_lua_start");
extern const uint8_t lua_producer_lua_end[] asm("_binary_producer_lua_end");
extern const uint8_t lua_sync_echo_lua_start[] asm("_binary_sync_echo_lua_start");
extern const uint8_t lua_sync_echo_lua_end[] asm("_binary_sync_echo_lua_end");

typedef struct {
    const char *path;
    const uint8_t *start;
    const uint8_t *end;
} test_lua_asset_t;

static const test_lua_asset_t s_lua_assets[] = {
    { TEST_THREAD_DIR "/blocking_recv.lua", lua_blocking_recv_lua_start, lua_blocking_recv_lua_end },
    { TEST_THREAD_DIR "/child_a.lua", lua_child_a_lua_start, lua_child_a_lua_end },
    { TEST_THREAD_DIR "/child_b.lua", lua_child_b_lua_start, lua_child_b_lua_end },
    { TEST_THREAD_DIR "/consumer.lua", lua_consumer_lua_start, lua_consumer_lua_end },
    { TEST_THREAD_DIR "/echo_child.lua", lua_echo_child_lua_start, lua_echo_child_lua_end },
    { TEST_THREAD_DIR "/harness.lua", lua_harness_lua_start, lua_harness_lua_end },
    { TEST_THREAD_DIR "/lock_holder.lua", lua_lock_holder_lua_start, lua_lock_holder_lua_end },
    { TEST_THREAD_DIR "/lock_waiter.lua", lua_lock_waiter_lua_start, lua_lock_waiter_lua_end },
    { TEST_THREAD_DIR "/producer.lua", lua_producer_lua_start, lua_producer_lua_end },
    { TEST_THREAD_DIR "/sync_echo.lua", lua_sync_echo_lua_start, lua_sync_echo_lua_end },
};

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "mkdir failed: path=%s errno=%d", path, errno);
    return ESP_FAIL;
}

static esp_err_t write_asset(const test_lua_asset_t *asset)
{
    FILE *file;
    size_t size;

    if (!asset || !asset->path || !asset->start || !asset->end || asset->end < asset->start) {
        return ESP_ERR_INVALID_ARG;
    }

    size = (size_t)(asset->end - asset->start);
    if (size > 0 && asset->start[size - 1] == '\0') {
        size--;
    }

    file = fopen(asset->path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "open failed: path=%s errno=%d", asset->path, errno);
        return ESP_FAIL;
    }
    size_t written = fwrite(asset->start, 1, size, file);
    if (written != size) {
        fclose(file);
        ESP_LOGE(TAG,
                 "write failed: path=%s written=%u size=%u errno=%d",
                 asset->path,
                 (unsigned)written,
                 (unsigned)size,
                 errno);
        return ESP_FAIL;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TAG, "close failed: path=%s errno=%d", asset->path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t prepare_lua_scripts(void)
{
    esp_err_t err = ensure_dir(TEST_LUA_BASE_DIR);
    if (err != ESP_OK) {
        return err;
    }
    err = ensure_dir(TEST_THREAD_DIR);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < sizeof(s_lua_assets) / sizeof(s_lua_assets[0]); i++) {
        err = write_asset(&s_lua_assets[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t init_fatfs(void)
{
    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 12,
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
        return err;
    }

    return esp_vfs_fat_spiflash_format_cfg_rw_wl(TEST_FATFS_BASE_PATH,
                                                 TEST_FATFS_PARTITION_LABEL,
                                                 &mount_config);
}

static esp_err_t init_lua_runtime(void)
{
    esp_err_t err = claw_cap_init();
    if (err != ESP_OK) {
        return err;
    }
    err = lua_module_thread_register();
    if (err != ESP_OK) {
        return err;
    }
    err = lua_module_delay_register();
    if (err != ESP_OK) {
        return err;
    }
    err = cap_lua_register_group(TEST_LUA_BASE_DIR);
    if (err != ESP_OK) {
        return err;
    }
    return claw_cap_start_all();
}

static void run_lua_case(const char *case_name, uint32_t timeout_ms, const char *expected)
{
    char args_json[160];
    esp_err_t err;

    memset(s_lua_case_output, 0, sizeof(s_lua_case_output));
    snprintf(args_json,
             sizeof(args_json),
             "{\"case\":\"%s\",\"base\":\"%s\"}",
             case_name,
             TEST_THREAD_DIR);
    err = cap_lua_run_script(TEST_HARNESS_PATH,
                             args_json,
                             timeout_ms,
                             s_lua_case_output,
                             sizeof(s_lua_case_output));
    printf("Lua case '%s' returned %s\n%s\n", case_name, esp_err_to_name(err), s_lua_case_output);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    if (expected) {
        TEST_ASSERT_NOT_NULL(strstr(s_lua_case_output, expected));
    }
}

void setUp(void)
{
}

void tearDown(void)
{
    memset(s_lua_teardown_output, 0, sizeof(s_lua_teardown_output));
    (void)cap_lua_stop_all_jobs(NULL, 3000, s_lua_teardown_output, sizeof(s_lua_teardown_output));
    printf("tearDown stop jobs: %s\n", s_lua_teardown_output);
    run_lua_case("cleanup_all", 5000, "cleanup_all ok");
    TEST_ASSERT_EQUAL(0, cap_lua_get_active_async_job_count());
}

TEST_CASE("thread functional: require module exposes job and sync APIs", "[thread][functional]")
{
    run_lua_case("api", 5000, "api ok");
}

TEST_CASE("thread functional: run passes args and captures output", "[thread][functional]")
{
    run_lua_case("run_args", 5000, "run_args ok");
}

TEST_CASE("thread flow: parent creates children and completes queue sem lock handoff", "[thread][flow]")
{
    run_lua_case("full_flow", 10000, "full_flow ok");
}

TEST_CASE("thread job: start list get and named replace", "[thread][job]")
{
    run_lua_case("job_named_replace", 10000, "job_named_replace ok");
}

TEST_CASE("thread job: exclusive rejects concurrent job unless replace", "[thread][job]")
{
    run_lua_case("job_exclusive", 10000, "job_exclusive ok");
}

TEST_CASE("thread sync: queue preserves order binary payload and timeout", "[thread][sync]")
{
    run_lua_case("queue_semantics", 5000, "queue_semantics ok");
}

TEST_CASE("thread sync: semaphore counting behavior", "[thread][sync]")
{
    run_lua_case("semaphore_counting", 5000, "semaphore_counting ok");
}

TEST_CASE("thread sync: lock ownership and contention", "[thread][sync]")
{
    run_lua_case("lock_contention", 10000, "lock_contention ok");
}

TEST_CASE("thread stop: stop unblocks queue recv", "[thread][stop]")
{
    run_lua_case("stop_queue", 10000, "stop_queue ok");
}

TEST_CASE("thread stop: stop unblocks lock wait", "[thread][stop]")
{
    run_lua_case("stop_lock", 10000, "stop_lock ok");
}

TEST_CASE("thread error: invalid args and opts", "[thread][error]")
{
    run_lua_case("invalid_args_opts", 5000, "invalid_args_opts ok");
}

TEST_CASE("thread error: sync invalid names and duplicate objects", "[thread][error]")
{
    run_lua_case("sync_errors", 5000, "sync_errors ok");
}

TEST_CASE("thread stability: repeated create communicate cleanup cycles", "[thread][stability]")
{
    run_lua_case("stability_cycles", 20000, "stability_cycles ok");
}

TEST_CASE("thread concurrency: multiple producers consumers", "[thread][concurrency]")
{
    run_lua_case("concurrency", 15000, "concurrency ok");
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(prepare_lua_scripts());
    ESP_ERROR_CHECK(init_lua_runtime());
    unity_run_menu();
}
