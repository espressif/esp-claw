/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Unity cases for claw-sys. The C side only calls the Rust scenario runners and
 * asserts the returned status; all logic (threads, the embedded-executor async
 * driver, HTTP) lives in claw-sys-selftest. Wi-Fi is brought up once in
 * app_main before the Unity menu starts, so the network cases assume an IP.
 */
#include "unity.h"

#include "claw_sys_selftest.h"
#include "test_secrets.h"

TEST_CASE("claw_sys log sink bridges to ESP_LOGx", "[claw_sys]")
{
    TEST_ASSERT_EQUAL_INT(0, claw_sys_selftest_log());
}

TEST_CASE("claw_sys EspIdfThread spawns, runs and joins a worker", "[claw_sys]")
{
    TEST_ASSERT_EQUAL_INT(0, claw_sys_selftest_thread());
}

TEST_CASE("claw_sys sync ClawHttp POST returns HTTP 200", "[claw_sys][network]")
{
    char body[768] = {0};
    int status = claw_sys_selftest_sync_http_post(TEST_HTTP_URL, body, sizeof(body));
    printf("sync post status=%d body=%.200s\n", status, body);
    TEST_ASSERT_EQUAL_INT(200, status);
}

TEST_CASE("claw_sys async ClawHttpAsync runs 3 posts via edge-executor", "[claw_sys][network]")
{
    int ok = claw_sys_selftest_run_three_async_http_posts(TEST_HTTPS_URL);
    printf("async posts succeeded=%d\n", ok);
    TEST_ASSERT_EQUAL_INT(3, ok);
}

/* ---------- Resource profiling ---------- */

TEST_CASE("resource: heap baseline", "[resource]")
{
    TEST_ASSERT_EQUAL_INT(0, claw_sys_selftest_resource_baseline());
}

TEST_CASE("resource: sync HTTP POST memory profile", "[resource][network]")
{
    int rc = claw_sys_selftest_resource_sync_http(TEST_HTTP_URL);
    printf("resource sync_http rc=%d\n", rc);
    TEST_ASSERT_EQUAL_INT(200, rc);
}

TEST_CASE("resource: async HTTP concurrency scaling (1,2,3)", "[resource][network]")
{
    int rc = claw_sys_selftest_resource_async_http(TEST_HTTPS_URL);
    printf("resource async_http rc=%d\n", rc);
    TEST_ASSERT_EQUAL_INT(0, rc);
}

TEST_CASE("resource: sync vs async comparison summary", "[resource][network]")
{
    int rc = claw_sys_selftest_resource_summary(TEST_HTTP_URL, TEST_HTTPS_URL);
    printf("resource summary rc=%d\n", rc);
    TEST_ASSERT_EQUAL_INT(0, rc);
}
