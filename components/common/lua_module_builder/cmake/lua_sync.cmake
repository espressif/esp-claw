include_guard(GLOBAL)

function(lua_module_builder_require_args function_name prefix)
    foreach(required_arg IN LISTS ARGN)
        if(NOT ${prefix}_${required_arg})
            message(FATAL_ERROR "${function_name} requires ${required_arg}")
        endif()
    endforeach()
endfunction()

function(lua_module_builder_configure_builtin_lua_sync)
    # DOCS_OUTPUT_DIR is parsed for backward compatibility but no longer used:
    # module docs are now bundled into the builtin_lua_modules skill source tree
    # so {CUR_SKILL_DIR}/scripts/docs/... resolves correctly at runtime.
    set(one_value_args TARGET BUILTIN_OUTPUT_DIR DOCS_OUTPUT_DIR)
    cmake_parse_arguments(arg "" "${one_value_args}" "" ${ARGN})

    lua_module_builder_require_args(lua_module_builder_configure_builtin_lua_sync arg TARGET BUILTIN_OUTPUT_DIR)

    idf_build_get_property(python PYTHON)
    set(tools_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools")
    set(scripts_stamp "${CMAKE_BINARY_DIR}/lua_module_builder_builtin_scripts_sync.stamp")
    set(scripts_depfile "${CMAKE_BINARY_DIR}/lua_module_builder_builtin_scripts_sync.d")
    set(modules_skill_stamp "${CMAKE_BINARY_DIR}/lua_module_builder_builtin_lua_modules_skill_generate.stamp")
    set(modules_skill_depfile "${CMAKE_BINARY_DIR}/lua_module_builder_builtin_lua_modules_skill_generate.d")
    set(docs_stamp "${CMAKE_BINARY_DIR}/lua_module_builder_docs_sync.stamp")
    set(docs_depfile "${CMAKE_BINARY_DIR}/lua_module_builder_docs_sync.d")
    set(common_script_args
        --build-dir "${CMAKE_BINARY_DIR}"
    )

    add_custom_command(
        OUTPUT "${scripts_stamp}"
        COMMAND ${python}
                "${tools_dir}/sync_lua_module_resources.py"
                ${common_script_args}
                --builtin-output-dir "${arg_BUILTIN_OUTPUT_DIR}"
                --libs-manifest-path "${CMAKE_BINARY_DIR}/lua_module_builder_builtin_lua_libs_manifest.json"
                --tests-manifest-path "${CMAKE_BINARY_DIR}/lua_module_builder_builtin_lua_tests_manifest.json"
                --stamp-path "${scripts_stamp}"
                --depfile "${scripts_depfile}"
        DEPENDS
                "${tools_dir}/sync_lua_module_resources.py"
                "${tools_dir}/lua_sync_common.py"
                "${CMAKE_BINARY_DIR}/project_description.json"
        DEPFILE "${scripts_depfile}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM
    )
    add_custom_target(lua_module_builder_sync_builtin_scripts ALL DEPENDS "${scripts_stamp}")

    add_custom_command(
        OUTPUT "${modules_skill_stamp}"
        COMMAND ${python}
                "${tools_dir}/generate_builtin_modules_skill.py"
                ${common_script_args}
                --stamp-path "${modules_skill_stamp}"
                --depfile "${modules_skill_depfile}"
        DEPENDS
                "${tools_dir}/generate_builtin_modules_skill.py"
                "${tools_dir}/sync_lua_module_docs.py"
                "${tools_dir}/lua_sync_common.py"
                "${CMAKE_BINARY_DIR}/project_description.json"
        DEPFILE "${modules_skill_depfile}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM
    )
    add_custom_target(lua_module_builder_generate_builtin_lua_modules_skill ALL DEPENDS "${modules_skill_stamp}")

    add_custom_command(
        OUTPUT "${docs_stamp}"
        COMMAND ${python}
                "${tools_dir}/sync_lua_module_docs.py"
                ${common_script_args}
                --manifest-path "${CMAKE_BINARY_DIR}/lua_module_builder_docs_manifest.json"
                --stamp-path "${docs_stamp}"
                --depfile "${docs_depfile}"
        DEPENDS
                "${tools_dir}/sync_lua_module_docs.py"
                "${tools_dir}/lua_sync_common.py"
                "${CMAKE_BINARY_DIR}/project_description.json"
        DEPFILE "${docs_depfile}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM
    )
    add_custom_target(lua_module_builder_sync_docs ALL DEPENDS "${docs_stamp}")

    # Docs and tests now live inside the builtin_lua_modules skill source dir,
    # so they must be written before skill_builder packs the skill via the
    # generate target.
    add_dependencies(lua_module_builder_generate_builtin_lua_modules_skill
        lua_module_builder_sync_docs
        lua_module_builder_sync_builtin_scripts)

    if(COMMAND skill_builder_add_target)
        skill_builder_add_target(lua_module_builder_generate_builtin_lua_modules_skill)
    else()
        message(WARNING
            "lua_module_builder builtin Lua modules skill generation is enabled without skill_builder; generated skill markdown will not be synced."
        )
    endif()

    add_dependencies(${arg_TARGET} lua_module_builder_sync_builtin_scripts)
    add_dependencies(${arg_TARGET} lua_module_builder_sync_docs)
endfunction()
