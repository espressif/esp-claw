/* Unity tests for claw_hw_registry. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "unity.h"

#include "claw_hw_registry.h"

static const char *TAG = "test_registry";

typedef struct {
    int count;
    const char *want;
    bool found;
    claw_hw_mode_t want_mode;
    bool want_mode_matched;
} iter_ctx_t;

static void iter_count_cb(const char *resource,
                          const char *owner_tag,
                          claw_hw_mode_t mode,
                          void *user_ctx)
{
    iter_ctx_t *c = (iter_ctx_t *)user_ctx;
    c->count++;
    if (c->want != NULL && strcmp(resource, c->want) == 0) {
        c->found = true;
        if (mode == c->want_mode) {
            c->want_mode_matched = true;
        }
    }
    (void)owner_tag;
}

static int registry_count(void)
{
    iter_ctx_t ctx = { 0 };
    TEST_ESP_OK(claw_hw_foreach(iter_count_cb, &ctx));
    return ctx.count;
}

/* Drain every tag the test cases below use, so each test starts clean. */
static void drain_all_known_tags(void)
{
    const char *known[] = {
        "test/a", "test/b", "test/c", "test/d",
        "lua/job/1", "lua/job/2",
        "board/x",
    };
    for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); ++i) {
        claw_hw_release_by_tag(known[i]);
    }
}

static void test_init_idempotent(void)
{
    TEST_ESP_OK(claw_hw_registry_init());
    TEST_ESP_OK(claw_hw_registry_init());
    TEST_ESP_OK(claw_hw_registry_init());
}

static void test_claim_release_roundtrip(void)
{
    drain_all_known_tags();

    claw_hw_claim_config_t cfg = {
        .resource   = "gpio:9",
        .owner_tag  = "test/a",
        .mode       = CLAW_HW_MODE_EXCLUSIVE,
    };
    claw_hw_lease_handle_t lease = NULL;
    TEST_ESP_OK(claw_hw_claim(&cfg, &lease));
    TEST_ASSERT_NOT_NULL(lease);

    const char *tag = NULL;
    TEST_ESP_OK(claw_hw_query("gpio:9", &tag));
    TEST_ASSERT_EQUAL_STRING("test/a", tag);

    TEST_ESP_OK(claw_hw_release(lease));

    tag = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:9", &tag));
}

static void test_exclusive_conflict(void)
{
    drain_all_known_tags();

    claw_hw_claim_config_t cfg_a = {
        .resource  = "gpio:9",
        .owner_tag = "test/a",
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    claw_hw_lease_handle_t lease_a = NULL;
    TEST_ESP_OK(claw_hw_claim(&cfg_a, &lease_a));

    claw_hw_claim_config_t cfg_b = {
        .resource  = "gpio:9",
        .owner_tag = "test/b",
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    claw_hw_lease_handle_t lease_b = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, claw_hw_claim(&cfg_b, &lease_b));
    TEST_ASSERT_NULL(lease_b);

    TEST_ESP_OK(claw_hw_release(lease_a));
    TEST_ESP_OK(claw_hw_claim(&cfg_b, &lease_b));
    TEST_ASSERT_NOT_NULL(lease_b);
    TEST_ESP_OK(claw_hw_release(lease_b));
}

static void test_shared_read_allowed(void)
{
    drain_all_known_tags();

    claw_hw_claim_config_t cfg = {
        .resource  = "i2c:0/0x55",
        .owner_tag = "test/a",
        .mode      = CLAW_HW_MODE_SHARED_READ,
    };
    claw_hw_lease_handle_t lease[3] = { 0 };
    for (int i = 0; i < 3; ++i) {
        TEST_ESP_OK(claw_hw_claim(&cfg, &lease[i]));
        TEST_ASSERT_NOT_NULL(lease[i]);
    }

    claw_hw_claim_config_t ex = {
        .resource  = "i2c:0/0x55",
        .owner_tag = "test/b",
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    claw_hw_lease_handle_t bad = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, claw_hw_claim(&ex, &bad));

    const char *tag = NULL;
    TEST_ESP_OK(claw_hw_query("i2c:0/0x55", &tag));
    TEST_ASSERT_EQUAL_STRING("test/a", tag);

    for (int i = 0; i < 3; ++i) {
        TEST_ESP_OK(claw_hw_release(lease[i]));
    }
    TEST_ESP_OK(claw_hw_claim(&ex, &bad));
    TEST_ESP_OK(claw_hw_release(bad));
}

static void test_sub_resource_rollback(void)
{
    drain_all_known_tags();

    claw_hw_claim_config_t pre = {
        .resource  = "gpio:9",
        .owner_tag = "test/a",
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    claw_hw_lease_handle_t pre_lease = NULL;
    TEST_ESP_OK(claw_hw_claim(&pre, &pre_lease));

    /* Sub-resource claim must fail on gpio:9 without inserting either row. */
    const char *subs[] = { "gpio:8", "gpio:9", NULL };
    claw_hw_claim_config_t cfg = {
        .resource      = "ledc:0",
        .owner_tag     = "test/b",
        .mode          = CLAW_HW_MODE_EXCLUSIVE,
        .sub_resources = subs,
    };
    claw_hw_lease_handle_t lease = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, claw_hw_claim(&cfg, &lease));

    const char *tag = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("ledc:0", &tag));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:8", &tag));

    iter_ctx_t ctx = { .want = "gpio:9", .want_mode = CLAW_HW_MODE_EXCLUSIVE };
    TEST_ESP_OK(claw_hw_foreach(iter_count_cb, &ctx));
    TEST_ASSERT_EQUAL_INT(1, ctx.count);
    TEST_ASSERT_TRUE(ctx.found);

    TEST_ESP_OK(claw_hw_release(pre_lease));
}

static void test_sub_resource_happy_path(void)
{
    drain_all_known_tags();

    const char *subs[] = { "gpio:8", "gpio:9", NULL };
    claw_hw_claim_config_t cfg = {
        .resource      = "ledc:0",
        .owner_tag     = "test/c",
        .mode          = CLAW_HW_MODE_EXCLUSIVE,
        .sub_resources = subs,
    };
    claw_hw_lease_handle_t lease = NULL;
    TEST_ESP_OK(claw_hw_claim(&cfg, &lease));

    const char *tag = NULL;
    TEST_ESP_OK(claw_hw_query("ledc:0", &tag));
    TEST_ASSERT_EQUAL_STRING("test/c", tag);
    TEST_ESP_OK(claw_hw_query("gpio:8", &tag));
    TEST_ASSERT_EQUAL_STRING("test/c", tag);
    TEST_ESP_OK(claw_hw_query("gpio:9", &tag));
    TEST_ASSERT_EQUAL_STRING("test/c", tag);

    TEST_ESP_OK(claw_hw_release(lease));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("ledc:0", &tag));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:8", &tag));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:9", &tag));
}

static void test_release_by_tag(void)
{
    drain_all_known_tags();

    claw_hw_claim_config_t c1 = {
        .resource = "gpio:1", .owner_tag = "lua/job/1", .mode = CLAW_HW_MODE_EXCLUSIVE };
    claw_hw_claim_config_t c2 = {
        .resource = "gpio:2", .owner_tag = "lua/job/1", .mode = CLAW_HW_MODE_EXCLUSIVE };
    claw_hw_claim_config_t c3 = {
        .resource = "gpio:3", .owner_tag = "lua/job/1", .mode = CLAW_HW_MODE_EXCLUSIVE };
    claw_hw_lease_handle_t l1, l2, l3;
    TEST_ESP_OK(claw_hw_claim(&c1, &l1));
    TEST_ESP_OK(claw_hw_claim(&c2, &l2));
    TEST_ESP_OK(claw_hw_claim(&c3, &l3));

    /* Unrelated tag must survive the release_by_tag below. */
    claw_hw_claim_config_t c_keep = {
        .resource = "gpio:10", .owner_tag = "lua/job/2", .mode = CLAW_HW_MODE_EXCLUSIVE };
    claw_hw_lease_handle_t l_keep;
    TEST_ESP_OK(claw_hw_claim(&c_keep, &l_keep));

    TEST_ESP_OK(claw_hw_release_by_tag("lua/job/1"));

    const char *tag = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:1", &tag));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:2", &tag));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:3", &tag));
    TEST_ESP_OK(claw_hw_query("gpio:10", &tag));
    TEST_ASSERT_EQUAL_STRING("lua/job/2", tag);

    TEST_ESP_OK(claw_hw_release(l_keep));
    (void)l1; (void)l2; (void)l3;
}

static void test_release_by_tag_no_match(void)
{
    drain_all_known_tags();
    TEST_ESP_OK(claw_hw_release_by_tag("no/such/tag"));
}

static void test_foreach_empty(void)
{
    drain_all_known_tags();
    int before = registry_count();
    TEST_ASSERT_EQUAL_INT(0, before);

    iter_ctx_t ctx = { 0 };
    TEST_ESP_OK(claw_hw_foreach(iter_count_cb, &ctx));
    TEST_ASSERT_EQUAL_INT(0, ctx.count);
}

static void test_key_helpers(void)
{
    char buf[64];
    TEST_ASSERT_EQUAL_STRING("gpio:5",       claw_hw_key_gpio(buf, sizeof(buf), 5));
    TEST_ASSERT_EQUAL_STRING("i2c:1/0x2a",   claw_hw_key_i2c(buf, sizeof(buf), 1, 0x2A));
    TEST_ASSERT_EQUAL_STRING("i2c:0/0x00",   claw_hw_key_i2c(buf, sizeof(buf), 0, 0x00));
    TEST_ASSERT_EQUAL_STRING("i2c:2/0xff",   claw_hw_key_i2c(buf, sizeof(buf), 2, 0xFF));
    TEST_ASSERT_EQUAL_STRING("spi:2/cs10",   claw_hw_key_spi(buf, sizeof(buf), 2, 10));
    TEST_ASSERT_EQUAL_STRING("i2s:0/tx",     claw_hw_key_i2s(buf, sizeof(buf), 0, true));
    TEST_ASSERT_EQUAL_STRING("i2s:0/rx",     claw_hw_key_i2s(buf, sizeof(buf), 0, false));
    TEST_ASSERT_EQUAL_STRING("rmt:3",        claw_hw_key_rmt(buf, sizeof(buf), 3));
    TEST_ASSERT_EQUAL_STRING("adc:1/ch4",    claw_hw_key_adc(buf, sizeof(buf), 1, 4));
    TEST_ASSERT_EQUAL_STRING("dev:audio_dac", claw_hw_key_device(buf, sizeof(buf), "audio_dac"));

    char small[16];
    TEST_ASSERT_NULL(claw_hw_key_gpio(small, sizeof(small), 5));
}

static void test_invalid_args(void)
{
    drain_all_known_tags();

    claw_hw_lease_handle_t lease = NULL;
    claw_hw_claim_config_t bad_res = {
        .resource = NULL, .owner_tag = "x", .mode = CLAW_HW_MODE_EXCLUSIVE };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, claw_hw_claim(&bad_res, &lease));

    claw_hw_claim_config_t bad_tag = {
        .resource = "gpio:1", .owner_tag = "", .mode = CLAW_HW_MODE_EXCLUSIVE };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, claw_hw_claim(&bad_tag, &lease));

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, claw_hw_claim(NULL, &lease));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, claw_hw_release(NULL));
}

typedef struct {
    int calls;
    char last_resource[32];
    char last_owner[32];
} release_probe_t;

static void on_release_probe(const char *resource,
                             const char *owner_tag,
                             void *user_ctx)
{
    release_probe_t *p = (release_probe_t *)user_ctx;
    p->calls++;
    strncpy(p->last_resource, resource, sizeof(p->last_resource) - 1);
    p->last_resource[sizeof(p->last_resource) - 1] = '\0';
    strncpy(p->last_owner, owner_tag, sizeof(p->last_owner) - 1);
    p->last_owner[sizeof(p->last_owner) - 1] = '\0';
}

static void test_on_release_callback(void)
{
    drain_all_known_tags();

    release_probe_t probe = { 0 };
    const char *subs[] = { "gpio:8", NULL };
    claw_hw_claim_config_t cfg = {
        .resource      = "ledc:0",
        .owner_tag     = "test/d",
        .mode          = CLAW_HW_MODE_EXCLUSIVE,
        .on_release    = on_release_probe,
        .user_ctx      = &probe,
        .sub_resources = subs,
    };
    claw_hw_lease_handle_t lease = NULL;
    TEST_ESP_OK(claw_hw_claim(&cfg, &lease));
    TEST_ESP_OK(claw_hw_release(lease));

    /* Callback fires once per lease, even when sub rows are present. */
    TEST_ASSERT_EQUAL_INT(1, probe.calls);
    TEST_ASSERT_EQUAL_STRING("ledc:0", probe.last_resource);
    TEST_ASSERT_EQUAL_STRING("test/d", probe.last_owner);
}

/* Story: two Lua jobs race on gpio:9; job A wins; job B unblocks after
 * cap_lua invokes release_by_tag from job A's exit path. */
static void test_ws10_two_lua_jobs_same_gpio(void)
{
    drain_all_known_tags();

    claw_hw_claim_config_t cfg_a = {
        .resource  = "gpio:9",
        .owner_tag = "lua/job/1",
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    claw_hw_lease_handle_t lease_a = NULL;
    TEST_ESP_OK(claw_hw_claim(&cfg_a, &lease_a));

    claw_hw_claim_config_t cfg_b = {
        .resource  = "gpio:9",
        .owner_tag = "lua/job/2",
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    claw_hw_lease_handle_t lease_b = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, claw_hw_claim(&cfg_b, &lease_b));

    TEST_ESP_OK(claw_hw_release_by_tag("lua/job/1"));
    TEST_ESP_OK(claw_hw_claim(&cfg_b, &lease_b));
    TEST_ASSERT_NOT_NULL(lease_b);

    TEST_ESP_OK(claw_hw_release(lease_b));
    (void)lease_a; /* released via release_by_tag above */
}

/* Story: one Lua job holds a button pin, a shared I2C sensor, and an LED PWM
 * with a GPIO sub-resource; release_by_tag must sweep all four rows. */
static void test_ws10_job_death_multi_resource_cleanup(void)
{
    drain_all_known_tags();

    claw_hw_claim_config_t c_btn = {
        .resource  = "gpio:1",
        .owner_tag = "lua/job/1",
        .mode      = CLAW_HW_MODE_EXCLUSIVE,
    };
    claw_hw_claim_config_t c_i2c = {
        .resource  = "i2c:0/0x55",
        .owner_tag = "lua/job/1",
        .mode      = CLAW_HW_MODE_SHARED_READ,
    };
    const char *led_subs[] = { "gpio:2", NULL };
    claw_hw_claim_config_t c_led = {
        .resource      = "ledc:0",
        .owner_tag     = "lua/job/1",
        .mode          = CLAW_HW_MODE_EXCLUSIVE,
        .sub_resources = led_subs,
    };
    claw_hw_lease_handle_t l_btn, l_i2c, l_led;
    TEST_ESP_OK(claw_hw_claim(&c_btn, &l_btn));
    TEST_ESP_OK(claw_hw_claim(&c_i2c, &l_i2c));
    TEST_ESP_OK(claw_hw_claim(&c_led, &l_led));

    TEST_ESP_OK(claw_hw_release_by_tag("lua/job/1"));

    const char *tag = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:1", &tag));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("i2c:0/0x55", &tag));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("ledc:0", &tag));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, claw_hw_query("gpio:2", &tag));

    (void)l_btn; (void)l_i2c; (void)l_led;
}

static void test_leak_1000_iterations(void)
{
    drain_all_known_tags();

    /* Warm up lazy allocations (e.g. table growth) before measuring. */
    claw_hw_claim_config_t warmup = {
        .resource = "gpio:9", .owner_tag = "test/a", .mode = CLAW_HW_MODE_EXCLUSIVE };
    claw_hw_lease_handle_t l;
    TEST_ESP_OK(claw_hw_claim(&warmup, &l));
    TEST_ESP_OK(claw_hw_release(l));

    size_t before = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    for (int i = 0; i < 1000; ++i) {
        const char *subs[] = { "gpio:8", NULL };
        claw_hw_claim_config_t cfg = {
            .resource      = "gpio:9",
            .owner_tag     = "test/a",
            .mode          = CLAW_HW_MODE_EXCLUSIVE,
            .sub_resources = subs,
        };
        claw_hw_lease_handle_t lease = NULL;
        TEST_ESP_OK(claw_hw_claim(&cfg, &lease));
        TEST_ESP_OK(claw_hw_release(lease));
    }
    size_t after = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "heap free before=%u after=%u delta=%d",
             (unsigned)before, (unsigned)after, (int)((long)before - (long)after));
    /* Allow a small slack for logging allocations; the registry itself is
     * expected to be leak-clean. */
    TEST_ASSERT_TRUE_MESSAGE((long)after >= (long)before - 32,
                             "1000-iter loop leaked heap");
}

void app_main(void)
{
    TEST_ESP_OK(claw_hw_registry_init());

    UNITY_BEGIN();
    RUN_TEST(test_init_idempotent);
    RUN_TEST(test_claim_release_roundtrip);
    RUN_TEST(test_exclusive_conflict);
    RUN_TEST(test_shared_read_allowed);
    RUN_TEST(test_sub_resource_happy_path);
    RUN_TEST(test_sub_resource_rollback);
    RUN_TEST(test_release_by_tag);
    RUN_TEST(test_release_by_tag_no_match);
    RUN_TEST(test_foreach_empty);
    RUN_TEST(test_key_helpers);
    RUN_TEST(test_invalid_args);
    RUN_TEST(test_on_release_callback);
    RUN_TEST(test_ws10_two_lua_jobs_same_gpio);
    RUN_TEST(test_ws10_job_death_multi_resource_cleanup);
    RUN_TEST(test_leak_1000_iterations);
    UNITY_END();
}
