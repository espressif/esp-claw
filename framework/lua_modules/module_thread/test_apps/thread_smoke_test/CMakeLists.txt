# This is the project CMakeLists.txt file for the test subproject
cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)
set(EXTRA_COMPONENT_DIRS
    "../../../../../framework/capabilities/lua_runtime"
    "../../../../../framework/foundation/claw_cap"
    "../../../../../framework/foundation/claw_core"
    "../../../../../framework/foundation/claw_event_router"
    "../../../../../framework/foundation/claw_skill"
    "../../../../../framework/platform/esp_idf/http_reuse"
    "../.."
    "../../../module_delay"
)

if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(thread_smoke_test)
