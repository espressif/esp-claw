idf_component_register(
    SRCS
        "claw-sys/csrc/claw_rs_log.c"
    INCLUDE_DIRS
        "claw-cabi/include"
    REQUIRES
        claw_cap
        esp_http_client
        esp-tls
        mbedtls
        newlib
        freertos
        pthread
)

set(claw_rust_target "xtensa-esp32s3-espidf")
set(claw_rust_profile "release")
set(claw_rust_lib "${CMAKE_CURRENT_SOURCE_DIR}/target/${claw_rust_target}/${claw_rust_profile}/libclaw_cabi.a")

add_custom_target(claw_rust_build
    BYPRODUCTS "${claw_rust_lib}"
    COMMAND cargo +esp build --release -p claw-cabi
            --target ${claw_rust_target} -Z build-std=std,panic_abort
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building claw-cabi Rust staticlib (${claw_rust_target})"
    VERBATIM
    USES_TERMINAL
)

add_library(claw_cabi_rust STATIC IMPORTED GLOBAL)
set_target_properties(claw_cabi_rust PROPERTIES
    IMPORTED_LOCATION "${claw_rust_lib}"
    INTERFACE_LINK_LIBRARIES "idf::esp_http_client;idf::esp-tls;idf::mbedtls;idf::newlib;idf::freertos;idf::pthread"
)
add_dependencies(claw_cabi_rust claw_rust_build)

target_link_libraries(${COMPONENT_LIB} PUBLIC claw_cabi_rust)
add_dependencies(${COMPONENT_LIB} claw_rust_build)
