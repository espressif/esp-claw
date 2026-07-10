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

#include "audio_codec_data_if.h"
#include "audio_codec_if.h"
#include "audio_private.h"
#include "cap_lua.h"
#include "claw_cap.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "lua_module_audio.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "wear_levelling.h"

#define TEST_FATFS_BASE_PATH       "/fatfs"
#define TEST_FATFS_PARTITION_LABEL "storage"
#define TEST_LUA_BASE_DIR          TEST_FATFS_BASE_PATH "/scripts"
#define TEST_AUDIO_DIR             TEST_FATFS_BASE_PATH "/audio_test"
#define TEST_HARNESS_PATH          TEST_AUDIO_DIR "/harness.lua"
#define TEST_OUTPUT_SIZE           4096
#define TEST_MOCK_RATE             48000
#define TEST_MOCK_CHANNELS         2
#define TEST_MOCK_BITS             16

static const char *TEST_TAG = "audio_test_app";
static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static char s_lua_case_output[TEST_OUTPUT_SIZE];

esp_http_client_handle_t __wrap_esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t __wrap_esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t __wrap_esp_http_client_perform(esp_http_client_handle_t client);
static void *s_http_reuse_wrap_refs[] __attribute__((used)) = {
    __wrap_esp_http_client_init,
    __wrap_esp_http_client_cleanup,
    __wrap_esp_http_client_perform,
};

extern const uint8_t lua_harness_lua_start[] asm("_binary_harness_lua_start");
extern const uint8_t lua_harness_lua_end[] asm("_binary_harness_lua_end");

typedef struct {
    audio_codec_if_t base;
    esp_codec_dev_sample_info_t fs;
    bool opened;
    bool enabled;
    bool muted;
    bool mic_muted;
    float vol_db;
    float mic_gain_db;
} test_codec_if_t;

typedef struct {
    audio_codec_data_if_t base;
    esp_codec_dev_sample_info_t fs;
    bool opened;
    bool enabled;
    uint32_t read_idx;
    uint32_t read_bytes;
    uint32_t write_bytes;
} test_data_if_t;

typedef struct {
    test_codec_if_t codec;
    test_data_if_t data;
    esp_codec_dev_handle_t dev;
} test_codec_dev_t;

static test_codec_dev_t s_output_codec;
static test_codec_dev_t s_input_codec;

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
    }
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    ESP_LOGE(TEST_TAG, "mkdir failed: path=%s errno=%d", path, errno);
    return ESP_FAIL;
}

static esp_err_t write_harness(void)
{
    FILE *file;
    size_t size;

    size = (size_t)(lua_harness_lua_end - lua_harness_lua_start);
    if (size > 0 && lua_harness_lua_start[size - 1] == '\0') {
        size--;
    }

    file = fopen(TEST_HARNESS_PATH, "wb");
    if (!file) {
        ESP_LOGE(TEST_TAG, "open failed: path=%s errno=%d", TEST_HARNESS_PATH, errno);
        return ESP_FAIL;
    }
    size_t written = fwrite(lua_harness_lua_start, 1, size, file);
    if (written != size) {
        fclose(file);
        ESP_LOGE(TEST_TAG, "write failed: path=%s written=%u size=%u errno=%d", TEST_HARNESS_PATH, (unsigned)written, (unsigned)size, errno);
        return ESP_FAIL;
    }
    if (fclose(file) != 0) {
        ESP_LOGE(TEST_TAG, "close failed: path=%s errno=%d", TEST_HARNESS_PATH, errno);
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
    err = ensure_dir(TEST_AUDIO_DIR);
    if (err != ESP_OK) {
        return err;
    }
    return write_harness();
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

    err = esp_vfs_fat_spiflash_mount_rw_wl(TEST_FATFS_BASE_PATH, TEST_FATFS_PARTITION_LABEL, &mount_config, &s_wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TEST_TAG, "mount FATFS failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_vfs_fat_spiflash_format_cfg_rw_wl(TEST_FATFS_BASE_PATH, TEST_FATFS_PARTITION_LABEL, &mount_config);
    if (err != ESP_OK) {
        ESP_LOGE(TEST_TAG, "format FATFS failed: %s", esp_err_to_name(err));
    }
    return err;
}

static int test_data_open(const audio_codec_data_if_t *h, void *data_cfg, int cfg_size)
{
    test_data_if_t *data = (test_data_if_t *)h;

    (void)data_cfg;
    (void)cfg_size;
    data->opened = true;
    return ESP_CODEC_DEV_OK;
}

static bool test_data_is_open(const audio_codec_data_if_t *h)
{
    return ((const test_data_if_t *)h)->opened;
}

static int test_data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t dev_type, bool enable)
{
    test_data_if_t *data = (test_data_if_t *)h;

    (void)dev_type;
    data->enabled = enable;
    return ESP_CODEC_DEV_OK;
}

static int test_data_set_fmt(const audio_codec_data_if_t *h, esp_codec_dev_type_t dev_type, esp_codec_dev_sample_info_t *fs)
{
    test_data_if_t *data = (test_data_if_t *)h;

    (void)dev_type;
    data->fs = *fs;
    return ESP_CODEC_DEV_OK;
}

static int test_data_read(const audio_codec_data_if_t *h, uint8_t *data_buf, int size)
{
    test_data_if_t *data = (test_data_if_t *)h;

    if (!data || !data_buf || size < 0) {
        ESP_LOGE(TEST_TAG, "mock read invalid arg");
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    for (int i = 0; i < size; i++) {
        data_buf[i] = (uint8_t)((data->read_idx + (uint32_t)i) & 0xFF);
    }
    data->read_idx += (uint32_t)size;
    data->read_bytes += (uint32_t)size;
    return ESP_CODEC_DEV_OK;
}

static int test_data_write(const audio_codec_data_if_t *h, uint8_t *data_buf, int size)
{
    test_data_if_t *data = (test_data_if_t *)h;

    if (!data || !data_buf || size < 0) {
        ESP_LOGE(TEST_TAG, "mock write invalid arg");
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    data->write_bytes += (uint32_t)size;
    return ESP_CODEC_DEV_OK;
}

static int test_data_close(const audio_codec_data_if_t *h)
{
    test_data_if_t *data = (test_data_if_t *)h;

    data->opened = false;
    data->enabled = false;
    return ESP_CODEC_DEV_OK;
}

static int test_codec_open(const audio_codec_if_t *h, void *cfg, int cfg_size)
{
    test_codec_if_t *codec = (test_codec_if_t *)h;

    (void)cfg;
    (void)cfg_size;
    codec->opened = true;
    return ESP_CODEC_DEV_OK;
}

static bool test_codec_is_open(const audio_codec_if_t *h)
{
    return ((const test_codec_if_t *)h)->opened;
}

static int test_codec_enable(const audio_codec_if_t *h, bool enable)
{
    test_codec_if_t *codec = (test_codec_if_t *)h;

    codec->enabled = enable;
    return ESP_CODEC_DEV_OK;
}

static int test_codec_set_fs(const audio_codec_if_t *h, esp_codec_dev_sample_info_t *fs)
{
    test_codec_if_t *codec = (test_codec_if_t *)h;

    codec->fs = *fs;
    return ESP_CODEC_DEV_OK;
}

static int test_codec_mute(const audio_codec_if_t *h, bool mute)
{
    test_codec_if_t *codec = (test_codec_if_t *)h;

    codec->muted = mute;
    return ESP_CODEC_DEV_OK;
}

static int test_codec_set_vol(const audio_codec_if_t *h, float db)
{
    test_codec_if_t *codec = (test_codec_if_t *)h;

    codec->vol_db = db;
    return ESP_CODEC_DEV_OK;
}

static int test_codec_set_mic_gain(const audio_codec_if_t *h, float db)
{
    test_codec_if_t *codec = (test_codec_if_t *)h;

    codec->mic_gain_db = db;
    return ESP_CODEC_DEV_OK;
}

static int test_codec_mute_mic(const audio_codec_if_t *h, bool mute)
{
    test_codec_if_t *codec = (test_codec_if_t *)h;

    codec->mic_muted = mute;
    return ESP_CODEC_DEV_OK;
}

static int test_codec_get_reg(const audio_codec_if_t *h, int reg, int *value)
{
    (void)h;

    if (!value) {
        ESP_LOGE(TEST_TAG, "mock get_reg invalid value pointer");
        return ESP_CODEC_DEV_INVALID_ARG;
    }
    switch (reg) {
    case AUDIO_CODEC_VREG_FORMAT_MAGIC:
        *value = AUDIO_CODEC_VREG_FORMAT_MAGIC_VALUE;
        return ESP_CODEC_DEV_OK;
    case AUDIO_CODEC_VREG_FORMAT_SAMPLE_RATE:
        *value = TEST_MOCK_RATE;
        return ESP_CODEC_DEV_OK;
    case AUDIO_CODEC_VREG_FORMAT_CHANNELS:
        *value = TEST_MOCK_CHANNELS;
        return ESP_CODEC_DEV_OK;
    case AUDIO_CODEC_VREG_FORMAT_BITS:
        *value = TEST_MOCK_BITS;
        return ESP_CODEC_DEV_OK;
    default:
        return ESP_CODEC_DEV_NOT_SUPPORT;
    }
}

static int test_codec_close(const audio_codec_if_t *h)
{
    test_codec_if_t *codec = (test_codec_if_t *)h;

    codec->opened = false;
    codec->enabled = false;
    return ESP_CODEC_DEV_OK;
}

static void init_mock_ifaces(test_codec_dev_t *mock)
{
    memset(mock, 0, sizeof(*mock));
    mock->codec.base.open = test_codec_open;
    mock->codec.base.is_open = test_codec_is_open;
    mock->codec.base.enable = test_codec_enable;
    mock->codec.base.set_fs = test_codec_set_fs;
    mock->codec.base.mute = test_codec_mute;
    mock->codec.base.set_vol = test_codec_set_vol;
    mock->codec.base.set_mic_gain = test_codec_set_mic_gain;
    mock->codec.base.mute_mic = test_codec_mute_mic;
    mock->codec.base.get_reg = test_codec_get_reg;
    mock->codec.base.close = test_codec_close;
    mock->data.base.open = test_data_open;
    mock->data.base.is_open = test_data_is_open;
    mock->data.base.enable = test_data_enable;
    mock->data.base.set_fmt = test_data_set_fmt;
    mock->data.base.read = test_data_read;
    mock->data.base.write = test_data_write;
    mock->data.base.close = test_data_close;
    mock->codec.base.open(&mock->codec.base, NULL, 0);
    mock->data.base.open(&mock->data.base, NULL, 0);
}

static esp_err_t init_mock_codecs(void)
{
    esp_codec_dev_cfg_t output_cfg;
    esp_codec_dev_cfg_t input_cfg;

    init_mock_ifaces(&s_output_codec);
    init_mock_ifaces(&s_input_codec);

    output_cfg = (esp_codec_dev_cfg_t) {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = &s_output_codec.codec.base,
        .data_if = &s_output_codec.data.base,
    };
    input_cfg = (esp_codec_dev_cfg_t) {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = &s_input_codec.codec.base,
        .data_if = &s_input_codec.data.base,
    };

    s_output_codec.dev = esp_codec_dev_new(&output_cfg);
    s_input_codec.dev = esp_codec_dev_new(&input_cfg);
    if (!s_output_codec.dev || !s_input_codec.dev) {
        ESP_LOGE(TEST_TAG, "mock codec device create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void reset_mock_stats(void)
{
    s_output_codec.data.read_idx = 0;
    s_output_codec.data.read_bytes = 0;
    s_output_codec.data.write_bytes = 0;
    s_input_codec.data.read_idx = 0;
    s_input_codec.data.read_bytes = 0;
    s_input_codec.data.write_bytes = 0;
}

static int lua_audio_test_output_codec(lua_State *L)
{
    lua_pushlightuserdata(L, s_output_codec.dev);
    return 1;
}

static int lua_audio_test_input_codec(lua_State *L)
{
    lua_pushlightuserdata(L, s_input_codec.dev);
    return 1;
}

static int lua_audio_test_reset_stats(lua_State *L)
{
    reset_mock_stats();
    lua_pushboolean(L, 1);
    return 1;
}

static int lua_audio_test_stats(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, s_output_codec.data.write_bytes);
    lua_setfield(L, -2, "output_write_bytes");
    lua_pushinteger(L, s_input_codec.data.read_bytes);
    lua_setfield(L, -2, "input_read_bytes");
    return 1;
}

static int luaopen_audio_test(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"output_codec", lua_audio_test_output_codec},
        {"input_codec",  lua_audio_test_input_codec},
        {"reset_stats",  lua_audio_test_reset_stats},
        {"stats",        lua_audio_test_stats},
        {NULL, NULL},
    };

    lua_newtable(L);
    luaL_setfuncs(L, funcs, 0);
    return 1;
}

static esp_err_t init_lua_runtime(void)
{
    esp_err_t err = claw_cap_init();
    if (err != ESP_OK) {
        ESP_LOGE(TEST_TAG, "claw_cap_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = lua_module_audio_register();
    if (err != ESP_OK) {
        ESP_LOGE(TEST_TAG, "lua_module_audio_register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_lua_register_module("audio_test", luaopen_audio_test);
    if (err != ESP_OK) {
        ESP_LOGE(TEST_TAG, "audio_test module register failed: %s", esp_err_to_name(err));
        return err;
    }
    err = cap_lua_register_group();
    if (err != ESP_OK) {
        ESP_LOGE(TEST_TAG, "cap_lua_register_group failed: %s", esp_err_to_name(err));
        return err;
    }
    err = claw_cap_start_all();
    if (err != ESP_OK) {
        ESP_LOGE(TEST_TAG, "claw_cap_start_all failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void run_lua_case(const char *case_name, uint32_t timeout_ms, const char *expected)
{
    char args_json[160];
    esp_err_t err;

    memset(s_lua_case_output, 0, sizeof(s_lua_case_output));
    snprintf(args_json, sizeof(args_json), "{\"case\":\"%s\",\"base\":\"%s\"}", case_name, TEST_AUDIO_DIR);
    err = cap_lua_run_script(TEST_HARNESS_PATH, args_json, timeout_ms, s_lua_case_output, sizeof(s_lua_case_output));
    printf("Lua case '%s' returned %s\n%s\n", case_name, esp_err_to_name(err), s_lua_case_output);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    if (expected) {
        TEST_ASSERT_NOT_NULL(strstr(s_lua_case_output, expected));
    }
}

void setUp(void)
{
    reset_mock_stats();
}

void tearDown(void)
{
    reset_mock_stats();
}

TEST_CASE("audio functional: require module exposes object APIs", "[audio][functional]")
{
    run_lua_case("api", 5000, "api ok");
}

TEST_CASE("audio error: invalid device arguments return clear errors", "[audio][error]")
{
    run_lua_case("invalid_args", 5000, "invalid_args ok");
}

TEST_CASE("audio output: open info volume mute write and tone", "[audio][output]")
{
    run_lua_case("output_device", 10000, "output_device ok");
}

TEST_CASE("audio input: open info volume and read", "[audio][input]")
{
    run_lua_case("input_device", 5000, "input_device ok");
}

TEST_CASE("audio player: object lifecycle keeps output busy", "[audio][player]")
{
    run_lua_case("player_lifecycle", 5000, "player_lifecycle ok");
}

TEST_CASE("audio analyzer: level and spectrum reads", "[audio][analyzer]")
{
    run_lua_case("analyzer", 10000, "analyzer ok");
}

TEST_CASE("audio recorder: write WAV from input stream", "[audio][recorder]")
{
    run_lua_case("recorder_wav", 10000, "recorder_wav ok");
}

void app_main(void)
{
    ESP_ERROR_CHECK(init_fatfs());
    ESP_ERROR_CHECK(prepare_lua_scripts());
    ESP_ERROR_CHECK(init_mock_codecs());
    ESP_ERROR_CHECK(init_lua_runtime());
    unity_run_menu();
}
