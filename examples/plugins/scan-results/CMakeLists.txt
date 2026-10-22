# Builds the example plugin against the SDK of an oshot "-plugins" build:
#   cmake -S . -B build -DOSHOT_SDK=<sdk dir> && cmake --build build
# oshotpm sets the OSHOT_SDK environment variable when it builds plugins.
cmake_minimum_required(VERSION 3.16)
project(oshot_scan_results C)

set(OSHOT_SDK "$ENV{OSHOT_SDK}" CACHE PATH "oshot plugin SDK, the folder with include/oshot/oshot_plugin.h")
if(NOT EXISTS "${OSHOT_SDK}/include/oshot/oshot_plugin.h")
    message(FATAL_ERROR "oshot's plugin SDK wasn't found: pass -DOSHOT_SDK=<folder with include/oshot/oshot_plugin.h>")
endif()

add_library(scan_results MODULE scan_results.c)
target_include_directories(scan_results PRIVATE "${OSHOT_SDK}/include")
set_target_properties(scan_results PROPERTIES C_STANDARD 11 C_VISIBILITY_PRESET hidden)

# The library goes to build/out, for every generator and configuration
set_target_properties(scan_results PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${config}" config)
    set_target_properties(scan_results PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/out")
endforeach()
