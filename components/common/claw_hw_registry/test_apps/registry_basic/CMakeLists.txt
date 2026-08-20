cmake_minimum_required(VERSION 3.16)

# Point EXTRA_COMPONENT_DIRS directly at the `claw_hw_registry` component
# directory (which has its own top-level CMakeLists.txt). ESP-IDF then
# registers it as a single component instead of scanning sibling
# directories under `components/common/`.
set(EXTRA_COMPONENT_DIRS "../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(registry_basic)
