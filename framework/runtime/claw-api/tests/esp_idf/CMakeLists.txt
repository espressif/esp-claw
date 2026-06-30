# Project file for the claw-api on-device test app.
#
# Builds an ESP-IDF C/Unity app that calls the Rust scenario runner in
# `claw-api-selftest` (a static lib built by main/CMakeLists.txt) to drive
# `ClawApi::chat` over `EspIdfHttp` against a live LLM. Run with pytest-embedded
# against an esp32s3 with Wi-Fi (see pytest_claw_api.py). Requires the Espressif
# Rust toolchain on PATH.
cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(claw_api_selftest_app)
