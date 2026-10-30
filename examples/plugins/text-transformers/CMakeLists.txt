# Builds the two example transformers against the SDK of an oshot "-plugins" build:
#   cmake -S . -B build -DOSHOT_SDK=<sdk dir> && cmake --build build
# oshotpm sets the OSHOT_SDK environment variable when it builds plugins.
cmake_minimum_required(VERSION 3.16)
project(oshot_text_transformers C)

set(OSHOT_SDK "$ENV{OSHOT_SDK}" CACHE PATH "oshot plugin SDK, the folder with include/oshot/oshot_plugin.h")
if(NOT EXISTS "${OSHOT_SDK}/include/oshot/oshot_plugin.h")
    message(FATAL_ERROR "oshot's plugin SDK wasn't found: pass -DOSHOT_SDK=<folder with include/oshot/oshot_plugin.h>")
endif()

# Each plugin's library goes to its own build/out/<name>, the output-dir its manifest entry names,
# for every generator and configuration
foreach(plugin join-lines extract-links)
    string(REPLACE "-" "_" target "${plugin}")
    add_library(${target} MODULE ${target}.c)
    target_include_directories(${target} PRIVATE "${OSHOT_SDK}/include")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_VISIBILITY_PRESET hidden)

    set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${plugin}")
    foreach(config ${CMAKE_CONFIGURATION_TYPES})
        string(TOUPPER "${config}" config)
        set_target_properties(${target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${config} "${CMAKE_BINARY_DIR}/out/${plugin}")
    endforeach()
endforeach()
