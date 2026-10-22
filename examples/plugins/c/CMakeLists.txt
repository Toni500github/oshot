# Builds the example plugin against the SDK of an oshot "-plugins" build:
#   cmake -S . -B build -DOSHOT_SDK=<sdk dir> && cmake --build build
# oshotpm sets the OSHOT_SDK environment variable when it builds plugins.
cmake_minimum_required(VERSION 3.16)
project(oshot_uppercase C)

set(OSHOT_SDK "$ENV{OSHOT_SDK}" CACHE PATH "oshot plugin SDK, the folder with include/oshot/oshot_plugin.h")
if(NOT EXISTS "${OSHOT_SDK}/include/oshot/oshot_plugin.h")
    message(FATAL_ERROR "oshot's plugin SDK wasn't found: pass -DOSHOT_SDK=<folder with include/oshot/oshot_plugin.h>")
endif()

add_library(uppercase MODULE uppercase.c)
target_include_directories(uppercase PRIVATE "${OSHOT_SDK}/include")
set_target_properties(uppercase PROPERTIES C_STANDARD 11 C_VISIBILITY_PRESET hidden)

# The library goes to build/out, for every generator and configuration
set_target_properties(uppercase PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${config}" config)
    set_target_properties(uppercase PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/out")
endforeach()

# cimgui comes from oshot: Linux and the BSDs resolve it when oshot loads the
# plugin, macOS needs to be told to, Windows links its import library
if(WIN32)
    find_library(CIMGUI_LIB NAMES cimgui libcimgui PATHS "${OSHOT_SDK}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(uppercase PRIVATE "${CIMGUI_LIB}")
elseif(APPLE)
    target_link_options(uppercase PRIVATE -undefined dynamic_lookup)
endif()
