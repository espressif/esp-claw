/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Unity case for claw-api. The C side only calls the Rust scenario runner and
 * asserts the result; the whole chat round-trip (build ClawApi over EspIdfHttp,
 * POST to the LLM, parse the reply) happens in claw-api-selftest. Wi-Fi is
 * brought up once in app_main before the Unity menu starts.
 */
#include <string.h>

#include "unity.h"

#include "claw_api_selftest.h"
#include "test_secrets.h"

TEST_CASE("claw_api chat hits the live LLM and returns text", "[claw_api][network]")
{
    char reply[1024] = {0};
    int rc = claw_api_selftest_chat(
        TEST_LLM_BASE_URL, TEST_LLM_API_KEY, TEST_LLM_MODEL,
        "Say hello in exactly two words.", reply, sizeof(reply));
    printf("chat rc=%d reply=%.300s\n", rc, reply);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(strlen(reply) > 0);
}

TEST_CASE("claw_api async chat via ClawHttpAsync returns text", "[claw_api][network][async]")
{
    char reply[1024] = {0};
    int rc = claw_api_selftest_chat_async(
        TEST_LLM_BASE_URL, TEST_LLM_API_KEY, TEST_LLM_MODEL,
        "Say hello in exactly two words.", reply, sizeof(reply));
    printf("async chat rc=%d reply=%.300s\n", rc, reply);
    TEST_ASSERT_EQUAL_INT(0, rc);
    TEST_ASSERT_TRUE(strlen(reply) > 0);
}
