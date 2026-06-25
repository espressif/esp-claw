#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0

import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent
COMPONENT = ROOT


def test_shtc3_crc_and_conversion():
    source = COMPONENT / "src" / "shtc3_math.c"
    include_dir = COMPONENT / "src"
    assert source.exists(), "SHTC3 math helper source is missing"

    harness = r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "shtc3_math.h"

static int expect_u8(const char *name, uint8_t actual, uint8_t expected)
{
    if (actual != expected) {
        fprintf(stderr, "%s: got 0x%02x expected 0x%02x\n", name, actual, expected);
        return 1;
    }
    return 0;
}

static int expect_float(const char *name, float actual, float expected, float tolerance)
{
    if (fabsf(actual - expected) > tolerance) {
        fprintf(stderr, "%s: got %.6f expected %.6f\n", name, actual, expected);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failures = 0;
    const uint8_t zero[] = {0x00, 0x00};
    const uint8_t full[] = {0xff, 0xff};
    const uint8_t sample[] = {0xbe, 0xef};

    failures += expect_u8("crc zero", shtc3_crc8(zero, sizeof(zero)), 0x81);
    failures += expect_u8("crc full", shtc3_crc8(full, sizeof(full)), 0xac);
    failures += expect_u8("crc sample", shtc3_crc8(sample, sizeof(sample)), 0x92);
    failures += expect_float("temp min", shtc3_raw_to_celsius(0x0000), -45.0f, 0.001f);
    failures += expect_float("temp max", shtc3_raw_to_celsius(0xffff), 130.0f, 0.001f);
    failures += expect_float("humidity min", shtc3_raw_to_humidity(0x0000), 0.0f, 0.001f);
    failures += expect_float("humidity max", shtc3_raw_to_humidity(0xffff), 100.0f, 0.001f);
    return failures == 0 ? 0 : 1;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        harness_path = tmp_path / "shtc3_math_harness.c"
        binary_path = tmp_path / "shtc3_math_harness"
        harness_path.write_text(harness)
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(include_dir),
                str(harness_path),
                str(source),
                "-lm",
                "-o",
                str(binary_path),
            ],
            check=True,
        )
        subprocess.run([str(binary_path)], check=True)


def test_example_script_uses_safe_display_values():
    script = (COMPONENT / "test" / "environmental_read.lua").read_text()
    assert "read_safe" in script
    assert "N/A" in script
    assert "temperature_display" in script
    assert "humidity_display" in script


if __name__ == "__main__":
    test_shtc3_crc_and_conversion()
    test_example_script_uses_safe_display_values()
