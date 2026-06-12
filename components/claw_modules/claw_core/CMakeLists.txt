idf_component_register(
    SRCS
        "src/claw_core.c"
        "src/claw_version.c"
        "src/claw_paths.c"
        "src/claw_core_agent_loop.c"
        "src/claw_core_context.c"
        "src/claw_core_control.c"
        "src/claw_core_events.c"
        "src/claw_core_messages.c"
        "src/claw_core_context_persist.c"
        "src/claw_core_utils.c"
        "src/claw_task.c"
        "src/claw_core_llm.c"
        "src/llm/claw_llm_runtime.c"
        "src/llm/claw_llm_http_transport.c"
        "src/llm/backends/claw_llm_backend_anthropic.c"
        "src/llm/backends/claw_llm_backend_openai_compatible.c"
        "src/llm/backends/claw_llm_backend_custom.c"
        "src/llm/media/claw_media_pipeline.c"
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES
        esp_http_client
        http_reuse
        esp-tls
        freertos
        mbedtls
        claw_utils
    PRIV_REQUIRES
        claw_event_router
)

set(CLAW_GIT_VERSION "unknown")
execute_process(
    COMMAND git -C "${CMAKE_CURRENT_LIST_DIR}/../../.." describe --always --tags --dirty
    OUTPUT_VARIABLE CLAW_GIT_VERSION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT CLAW_GIT_VERSION)
    set(CLAW_GIT_VERSION "unknown")
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE CLAW_GIT_VERSION="${CLAW_GIT_VERSION}")
