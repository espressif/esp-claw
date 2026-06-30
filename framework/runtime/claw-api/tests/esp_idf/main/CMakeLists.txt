# Register the Unity test component and link the Rust `claw-api-selftest`
# staticlib (cargo -> staticlib -> imported library), mirroring
# framework/runtime/CMakeLists.txt and the sibling claw-sys app.
#
# `claw_rs_log.c` (the per-level ESP_LOGx bridge claw-sys's log sink calls) is
# compiled here so the linker can always resolve the `claw_rs_log_*` shims that
# the linked claw-sys archive may reference. WHOLE_ARCHIVE keeps the Unity
# `TEST_CASE`s and the shim regardless of archive scan order.
idf_component_register(
    SRCS
        "test_app_main.c"
        "test_claw_api.c"
        "test_wifi.c"
        "../../../../claw-sys/csrc/claw_rs_log.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        unity
        esp_http_client
        esp-tls
        mbedtls
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
    WHOLE_ARCHIVE
)

# The Rust workspace root is four levels up: main -> esp_idf -> tests -> claw-api -> rust.
set(claw_api_rust_dir "${CMAKE_CURRENT_SOURCE_DIR}/../../../..")
set(claw_api_rust_target "xtensa-esp32s3-espidf")
set(claw_api_rust_profile "release")
set(claw_api_rust_lib
    "${claw_api_rust_dir}/target/${claw_api_rust_target}/${claw_api_rust_profile}/libclaw_api_selftest.a")

# Always invoke cargo; it does its own incremental rebuild. `std` is not shipped
# precompiled for *-espidf, so it is built from source (the pinned `esp`
# toolchain accepts -Z).
add_custom_target(claw_api_selftest_build
    BYPRODUCTS "${claw_api_rust_lib}"
    COMMAND cargo build --${claw_api_rust_profile} -p claw-api-selftest
            --target ${claw_api_rust_target} -Z build-std=std,panic_abort
    WORKING_DIRECTORY "${claw_api_rust_dir}"
    COMMENT "Building claw-api-selftest Rust staticlib (${claw_api_rust_target})"
    VERBATIM
    USES_TERMINAL
)

add_library(claw_api_selftest_rust STATIC IMPORTED GLOBAL)
set_target_properties(claw_api_selftest_rust PROPERTIES
    IMPORTED_LOCATION "${claw_api_rust_lib}"
    # The Rust archive references esp_http_client_* etc (from claw-sys's FFI
    # bindings, transitively linked into claw-api-selftest). CMake places
    # imported libraries after the IDF component link groups, so those symbols
    # would otherwise be unresolved. INTERFACE_LINK_LIBRARIES makes CMake emit
    # the IDF libs directly after the Rust archive.
    INTERFACE_LINK_LIBRARIES "idf::esp_http_client;idf::esp-tls;idf::mbedtls;idf::newlib;idf::freertos;idf::pthread"
)
add_dependencies(claw_api_selftest_rust claw_api_selftest_build)

target_link_libraries(${COMPONENT_LIB} PUBLIC claw_api_selftest_rust)
add_dependencies(${COMPONENT_LIB} claw_api_selftest_build)
