# Project file for the claw-sys on-device test app.
#
# Builds an ESP-IDF C/Unity app whose only job is to call the Rust scenario
# runners in `claw-sys-selftest` (built into a static lib by main/CMakeLists.txt)
# and assert their results. Run with pytest-embedded against an esp32s3 with
# Wi-Fi (see pytest_claw_sys.py). Requires the Espressif Rust toolchain on PATH.
cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(claw_sys_selftest_app)
