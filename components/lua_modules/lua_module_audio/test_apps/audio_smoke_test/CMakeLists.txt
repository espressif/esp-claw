# This is the project CMakeLists.txt file for the test subproject
cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)
set(EXTRA_COMPONENT_DIRS
    "../../../../claw_capabilities/cap_lua"
    "../../../../claw_modules/claw_cap"
    "../../../../claw_modules/claw_core"
    "../../../../claw_modules/claw_event_router"
    "../../../../claw_modules/claw_manager"
    "../../../../claw_modules/claw_skill"
    "../../../../common/http_reuse"
    "../.."
)

if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(audio_smoke_test)
