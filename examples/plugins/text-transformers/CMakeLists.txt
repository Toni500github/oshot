# Builds the two example transformers against oshot's plugin SDK:
#   cmake -S . -B build -DOSHOT_SDK=<sdk dir> && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(oshot_text_transformers C)

# oshotpm passes the SDK's folder in OSHOT_SDK. Without it, find_package()
# looks in the usual places, such as the prefix oshot was installed to.
set(OSHOT_SDK
    "$ENV{OSHOT_SDK}"
    CACHE PATH
    "oshot plugin SDK, the folder with include/oshot/oshot_plugin.h"
)
find_package(oshot REQUIRED CONFIG HINTS "${OSHOT_SDK}")

# Each plugin's library goes to its own build/out/<name>, the output-dir its
# manifest entry names
foreach(plugin join-lines extract-links)
    string(REPLACE "-" "_" target "${plugin}")
    oshot_add_plugin(${target} ${target}.c OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${plugin}")
    set_target_properties(${target} PROPERTIES C_STANDARD 11)
endforeach()
