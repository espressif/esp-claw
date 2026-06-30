idf_component_register(
    SRCS
        "src/claw_task.c"
        "src/claw_paths.c"
        "src/claw_version.c"
        "src/claw_utils_string.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        freertos
        heap
)

set(CLAW_GIT_VERSION "unknown")
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/../../.." describe --always --tags --dirty "--match=claw-v*.*"
    OUTPUT_VARIABLE CLAW_GIT_VERSION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT CLAW_GIT_VERSION)
    set(CLAW_GIT_VERSION "unknown")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE CLAW_GIT_VERSION="${CLAW_GIT_VERSION}")
