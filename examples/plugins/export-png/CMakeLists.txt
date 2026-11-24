# Builds the example plugin against oshot's plugin SDK:
#   cmake -S . -B build -DOSHOT_SDK=<sdk dir> && cmake --build build
cmake_minimum_required(VERSION 3.16)
project(oshot_export_png C)

# oshotpm passes the SDK's folder in OSHOT_SDK. Without it, find_package()
# looks in the usual places, such as the prefix oshot was installed to.
set(OSHOT_SDK
    "$ENV{OSHOT_SDK}"
    CACHE PATH
    "oshot plugin SDK, the folder with include/oshot/oshot_plugin.h"
)
find_package(oshot REQUIRED CONFIG HINTS "${OSHOT_SDK}")
find_package(Threads REQUIRED)

# The library goes to build/out, the manifest's output-dir
oshot_add_plugin(export_png export_png.c OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out")
# stb_image_write.h comes from oshot's vendored libraries
target_include_directories(export_png PRIVATE ../../../include/libs/)
target_link_libraries(export_png PRIVATE Threads::Threads)
set_target_properties(export_png PROPERTIES C_STANDARD 11)
