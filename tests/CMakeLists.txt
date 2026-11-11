# oshot's tests, with -DBUILD_TESTS=ON (see README.md). Catch2 is vendored
# as its amalgamated files in catch2/
add_library(catch2 STATIC catch2/catch_amalgamated.cpp)
target_include_directories(catch2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# main.cpp has its own main(), to set up g_config
target_compile_definitions(catch2 PRIVATE CATCH_AMALGAMATED_CUSTOM_MAIN)

add_executable(
    oshot_tests
    main.cpp
    config.cpp
    hotkey.cpp
    screen_capture.cpp
    text_extraction.cpp
    util.cpp
)
# oshotpm's code is only in plugin builds
if(NOT DISABLE_PLUGINS)
    target_sources(
        oshot_tests
        PRIVATE manifest.cpp plugin_index.cpp text_transformers.cpp
    )

    # The SDK's mock host, driving example plugins compiled in with their
    # oshot_plugin_entry() renamed, so they don't clash
    target_sources(oshot_tests PRIVATE plugin_mock.cpp plugin_mock_c.c)
    foreach(
        _example
        uppercase:c/uppercase.c
        join_lines:text-transformers/join_lines.c
        extract_links:text-transformers/extract_links.c
        export_png:export-png/export_png.c
    )
        string(REPLACE ":" ";" _example "${_example}")
        list(GET _example 0 _name)
        list(GET _example 1 _source)
        set(_source "${CMAKE_SOURCE_DIR}/examples/plugins/${_source}")
        target_sources(oshot_tests PRIVATE "${_source}")
        set_source_files_properties(
            "${_source}"
            PROPERTIES
                COMPILE_DEFINITIONS
                    "oshot_plugin_entry=oshot_test_${_name}_entry"
        )
    endforeach()
    # The examples and the mock include oshot/<header> from the SDK
    target_include_directories(oshot_tests PRIVATE "${OSHOT_SDK_DIR}/include")
    add_dependencies(oshot_tests oshot_sdk_headers)
    find_package(Threads REQUIRED)
    target_link_libraries(oshot_tests PRIVATE Threads::Threads)
endif()
add_dependencies(oshot_tests generate_version)
target_compile_options(oshot_tests PRIVATE ${OSHOT_WARNING_FLAGS})
target_compile_definitions(
    oshot_tests
    PRIVATE OSHOT_TESTS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
target_link_libraries(oshot_tests PRIVATE oshot_common catch2)
set_target_properties(
    oshot_tests
    PROPERTIES
        BUILD_RPATH
            "$<TARGET_FILE_DIR:imgui>${OSHOT_COMMON_RPATH_ENTRY}"
)

if(UNIX AND NOT APPLE)
    target_link_libraries(oshot_tests PRIVATE X11::X11)
endif()

if(WIN32)
    # The UTF-8 code page, for a build folder like C:\Users\Тест
    target_sources(oshot_tests PRIVATE ${CMAKE_BINARY_DIR}/manifest.rc)
endif()

# In the order they're written, which keeps the log readable (Catch2 shuffles
# by default)
add_test(NAME oshot_tests COMMAND oshot_tests --order decl)
