cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/../../components/claw_modules"
    # Only include the specific capabilities rover_s3 actually uses
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_rover"
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_unitv"
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_voice"
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_im_platform"
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_files"
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_skill_mgr"
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_session_mgr"
    "${CMAKE_SOURCE_DIR}/../../components/claw_capabilities/cap_system"
    # Common utilities (only what rover_s3 actually needs)
    "${CMAKE_SOURCE_DIR}/../../components/common/json"
    "${CMAKE_SOURCE_DIR}/../../components/common/http_reuse"
    "${CMAKE_SOURCE_DIR}/../../components/common/captive_dns"
    "${CMAKE_SOURCE_DIR}/../../components/common/app_claw"
    "${CMAKE_SOURCE_DIR}/components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover_s3)
