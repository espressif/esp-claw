cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "../../components/claw_modules"
    "../../components/claw_capabilities"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rover_mcp_demo)
