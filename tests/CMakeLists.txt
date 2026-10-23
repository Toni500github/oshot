# Tests of the text extraction pipeline (OCR and barcodes), with
# -DBUILD_TESTS=ON. Catch2 is vendored as its amalgamated files in catch2/
add_library(catch2 STATIC catch2/catch_amalgamated.cpp)
target_include_directories(catch2 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# main.cpp has its own main(), to set up g_config
target_compile_definitions(catch2 PRIVATE CATCH_AMALGAMATED_CUSTOM_MAIN)

add_executable(oshot_tests main.cpp text_extraction.cpp)
add_dependencies(oshot_tests generate_version)
target_compile_options(
    oshot_tests
    PRIVATE -Wall -Wextra -Wpedantic -Wno-unused-parameter
)
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

add_test(NAME text_extraction COMMAND oshot_tests)
