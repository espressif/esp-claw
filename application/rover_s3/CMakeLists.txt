cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_SOURCE_DIR}/../../../components/claw_modules"
    "${CMAKE_SOURCE_DIR}/../../../components/claw_capabilities"
    "${CMAKE_SOURCE_DIR}/../../../components/common"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover_s3)
