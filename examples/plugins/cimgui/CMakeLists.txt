# Builds the cimgui example plugin against oshot's plugin SDK:
#   cmake -S . -B build -DOSHOT_SDK=<sdk dir> && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(oshot_line_lengths C)

# oshotpm passes the SDK's folder in OSHOT_SDK. Without it, find_package()
# looks in the usual places, such as the prefix oshot was installed to.
set(OSHOT_SDK
    "$ENV{OSHOT_SDK}"
    CACHE PATH
    "oshot plugin SDK, the folder with include/oshot/oshot_plugin.h"
)
find_package(oshot REQUIRED CONFIG HINTS "${OSHOT_SDK}")

# CIMGUI links what calling cimgui needs on each system (nothing on Linux and
# the BSDs, -undefined dynamic_lookup on macOS, the SDK's import library on
# Windows). The library goes to build/out, the manifest's output-dir
oshot_add_plugin(line_lengths CIMGUI line_lengths.c OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
set_target_properties(line_lengths PROPERTIES C_STANDARD 11)
