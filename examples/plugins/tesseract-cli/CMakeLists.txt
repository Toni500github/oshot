# Builds the example plugin against the SDK of an oshot "-plugins" build:
#   cmake -S . -B build -DOSHOT_SDK=<sdk dir> && cmake --build build
# oshotpm sets the OSHOT_SDK environment variable when it builds plugins.
cmake_minimum_required(VERSION 3.16)
project(oshot_tesseract_cli C)

set(OSHOT_SDK "$ENV{OSHOT_SDK}" CACHE PATH "oshot plugin SDK, the folder with include/oshot/oshot_plugin.h")
if(NOT EXISTS "${OSHOT_SDK}/include/oshot/oshot_plugin.h")
    message(FATAL_ERROR "oshot's plugin SDK wasn't found: pass -DOSHOT_SDK=<folder with include/oshot/oshot_plugin.h>")
endif()

find_package(Threads REQUIRED)

add_library(tesseract_cli MODULE tesseract_cli.c)
target_include_directories(tesseract_cli PRIVATE "${OSHOT_SDK}/include")
target_link_libraries(tesseract_cli PRIVATE Threads::Threads)
set_target_properties(tesseract_cli PROPERTIES C_STANDARD 11 C_VISIBILITY_PRESET hidden)

# The library goes to build/out, for every generator and configuration
set_target_properties(tesseract_cli PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
foreach(config ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER "${config}" config)
    set_target_properties(tesseract_cli PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/out")
endforeach()
