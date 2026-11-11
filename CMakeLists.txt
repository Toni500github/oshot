cmake_minimum_required(VERSION 3.15)

# Release builds default to -O2 rather than CMake's -O3. OCR time goes to
# Tesseract, so -O3 read the test set no faster and only made the binaries 9%
# bigger. They're cached before project(), so a CMAKE_<LANG>_FLAGS_RELEASE
# given on the command line still wins
foreach(_lang C CXX OBJC OBJCXX)
    set(CMAKE_${_lang}_FLAGS_RELEASE
        "-O2 -DNDEBUG"
        CACHE STRING
        "Flags used by the ${_lang} compiler during Release builds"
    )
endforeach()

project(oshot VERSION 0.5.0 LANGUAGES C CXX)
set(PROJECT_VERSION_SUFFIX "-rc2")
set(PROJECT_FULL_VERSION "${PROJECT_VERSION}${PROJECT_VERSION_SUFFIX}")

if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)

#add_compile_options($<$<CONFIG:Debug>:-fsanitize=address>)
#add_compile_options($<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
#add_link_options($<$<CONFIG:Debug>:-fsanitize=address>)

# =============================================================================
# Options
# =============================================================================

# option() that an environment variable of the same name can override.
# With DEFINE_OPTION, an enabled option adds OPTION_NAME=1 to
# OSHOT_OPTION_DEFINITIONS, which only oshot_common gets, since
# add_definitions() would leak it into the vendored libraries too.
# https://github.com/libcpr/cpr/blob/5f47552/CMakeLists.txt#L39
set(OSHOT_OPTION_DEFINITIONS "")
macro(add_option OPTION_NAME OPTION_TEXT OPTION_DEFAULT DEFINE_OPTION)
    option(${OPTION_NAME} ${OPTION_TEXT} ${OPTION_DEFAULT})
    if(DEFINED ENV{${OPTION_NAME}})
        set(${OPTION_NAME} $ENV{${OPTION_NAME}})
    endif()
    if(${OPTION_NAME} AND ${DEFINE_OPTION})
        list(APPEND OSHOT_OPTION_DEFINITIONS ${OPTION_NAME}=1)
    endif()
    message(STATUS "  ${OPTION_NAME}: ${${OPTION_NAME}}")
endmacro()

add_option(DISABLE_PLUGINS "Disable plugins support" ON ON)
add_option(
    MACOS_OPENGL
    "Draw the overlay with OpenGL instead of Metal on macOS, for VMs without Metal"
    OFF
    ON
)
add_option(
    BUILD_EXAMPLES
    "Build the example plugins in examples/plugins (needs to be built with plugins)"
    OFF
    OFF
)
add_option(BUILD_TESTS "Build the tests (tests/, run with ctest)" OFF OFF)

# Arch-style default; Debian sets it empty, as the license goes into
# /usr/share/doc/oshot/copyright there
set(OSHOT_INSTALL_LICENSEDIR
    "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}"
    CACHE STRING
    "Where to install the LICENSE file, relative to the prefix (empty to skip)"
)

# =============================================================================
# Toolchain
# =============================================================================

# Enables LTO in Release builds for each target given.
# Never on Windows, where one link mixing LTO and non-LTO objects (oshot_common
# needs -Wl,--export-all-symbols, or is an OBJECT library merged into
# oshot.exe) hits GCC/MinGW bugs folding weak COMDAT symbols, like spdlog's
# thread_local statics and template instantiations.
function(enable_lto)
    if(WIN32)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(_flag -flto=auto)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        set(_flag -flto)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set(_flag -flto=thin)
    else()
        return()
    endif()
    foreach(_target ${ARGN})
        target_compile_options(${_target} PRIVATE $<$<CONFIG:Release>:${_flag}>)
        target_link_options(${_target} PRIVATE $<$<CONFIG:Release>:${_flag}>)
    endforeach()
endfunction()

# The static libraries hold only GCC's LTO bytecode, which plain ar can't index
# unless binutils finds the LTO plugin by itself, so archive with gcc-ar
if(
    CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
    AND NOT WIN32
    AND CMAKE_CXX_COMPILER_AR
    AND CMAKE_CXX_COMPILER_RANLIB
)
    set(CMAKE_AR "${CMAKE_CXX_COMPILER_AR}")
    set(CMAKE_RANLIB "${CMAKE_CXX_COMPILER_RANLIB}")
endif()

# pkg-config hands over every library behind GTK, Tesseract (libcurl,
# libarchive) and the Wayland modules, so link only the ones a binary calls,
# as Ubuntu's GCC and the distros' packaging flags already do
if(UNIX AND NOT APPLE)
    add_link_options(LINKER:--as-needed)
endif()

# The warnings oshot's own executables are built with (tests/ too)
set(OSHOT_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wno-unused-parameter)

# =============================================================================
# Dependencies
# =============================================================================
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

if(TARGET glfw::glfw)
    set(GLFW_TARGET glfw::glfw)
elseif(TARGET glfw)
    set(GLFW_TARGET glfw)
elseif(TARGET glfw3)
    set(GLFW_TARGET glfw3)
else()
    message(FATAL_ERROR "GLFW found but no usable CMake target was exported")
endif()

pkg_check_modules(TESSERACT REQUIRED IMPORTED_TARGET tesseract)
pkg_check_modules(LEPTONICA REQUIRED IMPORTED_TARGET lept)
pkg_check_modules(ZBAR REQUIRED IMPORTED_TARGET zbar)
# Where Tesseract is installed, one of the places to look for its models
pkg_get_variable(TESSERACT_DATADIR tesseract datadir)

# Homebrew's lept.pc points Cflags at .../include/leptonica (its own
# convention is `#include <allheaders.h>`), while we include
# <leptonica/allheaders.h>, which needs the parent directory too
message(STATUS "LEPTONICA_INCLUDE_DIRS before: ${LEPTONICA_INCLUDE_DIRS}")
foreach(_lept_dir ${LEPTONICA_INCLUDE_DIRS})
    get_filename_component(_lept_parent_dir "${_lept_dir}" DIRECTORY)
    list(APPEND LEPTONICA_INCLUDE_DIRS "${_lept_parent_dir}")
endforeach()
list(REMOVE_DUPLICATES LEPTONICA_INCLUDE_DIRS)
message(STATUS "LEPTONICA_INCLUDE_DIRS after: ${LEPTONICA_INCLUDE_DIRS}")

# Tesseract runs its LSTM on OpenMP threads, which makes it slower. The
# OpenMP runtime lets text_extraction.cpp turn that off. macOS's Tesseract
# (static, built with Apple clang) has no OpenMP
if(NOT APPLE)
    find_package(OpenMP COMPONENTS CXX)
endif()

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    # The imported targets carry the full library paths. On the BSDs these
    # live in /usr/local/lib, which the linker doesn't search by default.
    # Ayatana comes first, because from 0.6 on it has the "activate" signal.
    # Without it, a left click on the tray icon in StatusNotifier hosts can
    # only open the menu
    pkg_check_modules(APPINDICATOR IMPORTED_TARGET ayatana-appindicator3-0.1)
    if(NOT APPINDICATOR_FOUND)
        pkg_check_modules(
            APPINDICATOR
            REQUIRED
            IMPORTED_TARGET
            appindicator3-0.1
        )
    endif()
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
    pkg_check_modules(XRANDR REQUIRED IMPORTED_TARGET xrandr)
    pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    # For a UI font covering the language's script
    # (util.cpp, find_language_fonts())
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    pkg_check_modules(
        WAYLAND
        REQUIRED
        IMPORTED_TARGET
        wayland-client
        wayland-server
        wayland-egl
        wayland-cursor
    )
elseif(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
endif()

# =============================================================================
# Generated sources
# =============================================================================

set(VERSION_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h)
set(VERSION_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generateVersion.sh)

# It runs on every build, so a new commit or tag is picked up. The script only
# rewrites the file when it has changed, and only src/globals.cpp includes it
add_custom_target(
    generate_version
    COMMAND ${VERSION_SCRIPT}
    BYPRODUCTS ${VERSION_HEADER}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating version header"
)

# The po/<code>.po translations, embedded into a source file for src/i18n.cpp
set(TRANSLATIONS_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/translations.cpp)
file(GLOB PO_FILES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/po/*.po)
add_custom_command(
    OUTPUT ${TRANSLATIONS_SOURCE}
    COMMAND
        ${CMAKE_COMMAND} -DPO_DIR=${CMAKE_CURRENT_SOURCE_DIR}/po
        -DOUTPUT=${TRANSLATIONS_SOURCE} -P
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_translations.cmake
    DEPENDS
        ${PO_FILES}
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts/embed_translations.cmake
    COMMENT "Embedding translations"
)

# The Wayland xdg-output protocol, for the monitors' logical geometry
if(UNIX AND NOT APPLE)
    set(PROTOCOL_BASE_NAME xdg-output-unstable-v1)
    set(PROTOCOL_HEADER
        ${CMAKE_SOURCE_DIR}/include/${PROTOCOL_BASE_NAME}-client-protocol.h
    )
    set(PROTOCOL_SOURCE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/${PROTOCOL_BASE_NAME}-protocol.c
    )

    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
    pkg_get_variable(WAYLAND_SCANNER wayland-scanner wayland_scanner)
    set(XDG_OUTPUT_XML
        "${WAYLAND_PROTOCOLS_DIR}/unstable/xdg-output/${PROTOCOL_BASE_NAME}.xml"
    )

    add_custom_command(
        OUTPUT ${PROTOCOL_HEADER}
        COMMAND
            ${WAYLAND_SCANNER} client-header ${XDG_OUTPUT_XML}
            ${PROTOCOL_HEADER}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ${XDG_OUTPUT_XML}
        COMMENT "Generating Wayland ${PROTOCOL_BASE_NAME} header"
    )
    add_custom_command(
        OUTPUT ${PROTOCOL_SOURCE}
        COMMAND
            ${WAYLAND_SCANNER} private-code ${XDG_OUTPUT_XML} ${PROTOCOL_SOURCE}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ${XDG_OUTPUT_XML}
        COMMENT "Generating Wayland ${PROTOCOL_BASE_NAME} private code"
    )
    add_custom_target(
        generate_protocol
        DEPENDS ${PROTOCOL_HEADER} ${PROTOCOL_SOURCE}
    )
endif()

# =============================================================================
# rpath
# =============================================================================
if(APPLE)
    set(_rpath_origin "@loader_path")
else()
    set(_rpath_origin "$ORIGIN")
endif()

# With plugins, oshot's shared libraries (imgui, cimgui, oshot_common) go in a
# private lib/oshot folder, so their generic names don't clash with other
# packages' libraries. Windows keeps the DLLs next to oshot.exe.
if(WIN32)
    set(OSHOT_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
    set(_full_libdir "${CMAKE_INSTALL_FULL_LIBDIR}")
else()
    set(OSHOT_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}/${PROJECT_NAME}")
    set(_full_libdir "${CMAKE_INSTALL_FULL_LIBDIR}/${PROJECT_NAME}")
endif()

# Relative, so the install stays relocatable (zip, AppImage), and it follows
# CMAKE_INSTALL_LIBDIR, which is lib64 on Fedora/openSUSE and
# lib/<multiarch> on Debian
file(RELATIVE_PATH _bin_to_lib "${CMAKE_INSTALL_FULL_BINDIR}" "${_full_libdir}")
set(OSHOT_INSTALL_RPATH "${_rpath_origin}/${_bin_to_lib}")

# With plugins, oshot_common is a shared library that the executables
# have to find in the build tree too
if(NOT DISABLE_PLUGINS)
    set(OSHOT_COMMON_RPATH_ENTRY ";$<TARGET_FILE_DIR:oshot_common>")
else()
    set(OSHOT_COMMON_RPATH_ENTRY "")
endif()
set(OSHOT_BUILD_RPATH
    "${_rpath_origin}/../lib;$<TARGET_FILE_DIR:imgui>${OSHOT_COMMON_RPATH_ENTRY}"
)

# =============================================================================
# Vendored libraries
# =============================================================================

# ---- fmt ----
add_library(fmt STATIC src/libs/fmt/os.cc src/libs/fmt/format.cc)
target_include_directories(fmt PUBLIC include/libs)
target_compile_definitions(fmt PUBLIC FMT_HEADER_ONLY=0)

# ---- toml++ ----
add_library(toml++ STATIC src/libs/toml++/toml.cpp)
target_include_directories(toml++ PUBLIC include/libs)
target_compile_definitions(toml++ PUBLIC TOML_HEADER_ONLY=0)

# ---- tfd (tinyfiledialogs) ----
add_library(tfd STATIC src/libs/tinyfiledialogs/tinyfiledialogs.c)
target_include_directories(tfd PUBLIC include/libs)

# ---- imgui ----
file(GLOB IMGUI_SOURCES src/libs/imgui/*.cpp)
if(APPLE)
    list(APPEND IMGUI_SOURCES src/libs/imgui/imgui_impl_metal.mm)
endif()

if(DISABLE_PLUGINS)
    add_library(imgui STATIC ${IMGUI_SOURCES})
else()
    add_library(imgui SHARED ${IMGUI_SOURCES})
    set_target_properties(
        imgui
        PROPERTIES VERSION 1.92.9 SOVERSION 1 WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

target_include_directories(imgui PUBLIC include/libs/imgui)
target_compile_definitions(
    imgui
    PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD IMGUI_DISABLE_OBSOLETE_FUNCTIONS
)
target_link_libraries(imgui PUBLIC ${GLFW_TARGET})
if(APPLE)
    target_link_libraries(
        imgui
        PUBLIC
            ${METAL_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK}
            ${QUARTZCORE_FRAMEWORK}
    )
endif()

# ---- cimgui (C-ABI plugins only) ----
if(NOT DISABLE_PLUGINS)
    add_library(cimgui SHARED src/libs/cimgui/cimgui.cpp)
    target_include_directories(cimgui PUBLIC include/libs)
    target_link_libraries(cimgui PUBLIC imgui)
    set_target_properties(
        cimgui
        PROPERTIES
            VERSION 1
            SOVERSION 1
            WINDOWS_EXPORT_ALL_SYMBOLS ON
            INSTALL_RPATH "${_rpath_origin}"
    )
endif()

# ---- tray ----
if(WIN32)
    add_library(tray STATIC src/libs/tray/tray_windows.cpp)
    target_compile_definitions(
        tray
        PRIVATE TRAY_WINAPI=1 WIN32_LEAN_AND_MEAN NOMINMAX
    )
elseif(APPLE)
    add_library(tray STATIC src/libs/tray/tray_darwin.mm)
    target_compile_definitions(tray PRIVATE TRAY_APPKIT=1)
    target_link_libraries(tray PRIVATE ${COCOA_FRAMEWORK})
else()
    add_library(tray STATIC src/libs/tray/tray_linux.cpp)
    target_compile_definitions(tray PRIVATE TRAY_APPINDICATOR=1)
    target_link_libraries(tray PRIVATE PkgConfig::GTK PkgConfig::APPINDICATOR)
endif()
target_include_directories(tray PUBLIC include/libs)

# ---- clip ----
if(WIN32)
    set(CLIP_PLATFORM_SOURCES
        src/libs/clip/clip_win.cpp
        src/libs/clip/clip_win_bmp.cpp
        src/libs/clip/clip_win_wic.cpp
    )
elseif(APPLE)
    set(CLIP_PLATFORM_SOURCES src/libs/clip/clip_osx.mm)
else()
    # X11 and Wayland go through xclip/wl-copy instead (clipboard.cpp)
    set(CLIP_PLATFORM_SOURCES src/libs/clip/clip_none.cpp)
endif()

add_library(
    clip
    STATIC
    src/libs/clip/clip.cpp
    src/libs/clip/image.cpp
    ${CLIP_PLATFORM_SOURCES}
)
target_include_directories(clip PUBLIC include/libs/clip)
target_compile_definitions(clip PUBLIC CLIP_ENABLE_IMAGE=1)

if(MSVC)
    target_compile_definitions(clip PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(MINGW)
    # MinGW needs windowscodecs only because it defines the CLSIDs in
    # libwindowscodecs.a rather than in wincodec.h (?!)
    target_link_libraries(clip PUBLIC shlwapi windowscodecs)
endif()

# ---- tinygettext ----
file(GLOB TINYGETTEXT_SOURCES src/libs/tinygettext/*.cpp)
add_library(tinygettext STATIC ${TINYGETTEXT_SOURCES})
set_target_properties(tinygettext PROPERTIES CXX_STANDARD 17)
target_include_directories(tinygettext PUBLIC include/libs)
# The .po files are UTF-8 (as xgettext, msgmerge and Weblate write them), and
# the strings go to ImGui as UTF-8. There's no charset conversion, so there's
# no iconv to ship on Windows/macOS. PUBLIC, because it changes a type in
# iconv.hpp, which i18n.cpp includes through po_parser.hpp
target_compile_definitions(
    tinygettext
    PUBLIC TINYGETTEXT_UTF8_ONLY
    PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>
)

# ---- dylib (plugins, and the DPI awareness functions on Windows) ----
if(WIN32 OR NOT DISABLE_PLUGINS)
    file(GLOB DYLIB_SOURCES src/libs/dylib/*.cpp)
    add_library(dylib STATIC ${DYLIB_SOURCES})
    target_include_directories(dylib PUBLIC include/libs/)
    target_link_libraries(dylib PRIVATE ${CMAKE_DL_LIBS})
    enable_lto(dylib)
endif()

# ---- nvdialog ----
if(WIN32)
    file(GLOB NVD_BACKEND_SOURCES src/libs/nvdialog/backend/win32/*.c)
elseif(APPLE)
    file(GLOB NVD_BACKEND_SOURCES src/libs/nvdialog/backend/cocoa/*.m)
else()
    file(GLOB NVD_BACKEND_SOURCES src/libs/nvdialog/backend/gtk/*.c)
endif()
file(GLOB NVD_COMMON_SOURCES src/libs/nvdialog/*.c)

add_library(nvdialog STATIC ${NVD_COMMON_SOURCES} ${NVD_BACKEND_SOURCES})
target_include_directories(
    nvdialog
    PUBLIC include/libs/nvdialog/ include/libs
    PRIVATE src/libs/nvdialog/impl/
)
target_compile_definitions(
    nvdialog
    PUBLIC NVDIALOG_MAXBUF=4096 NVD_STATIC_LINKAGE=1
    PRIVATE NVD_EXPORT_SYMBOLS
)

if(WIN32)
    target_link_libraries(
        nvdialog
        PUBLIC comdlg32 shell32 user32 gdi32 ole32
    )
elseif(APPLE)
    target_compile_definitions(nvdialog PRIVATE NVD_USE_COCOA=1)
    target_compile_options(nvdialog PRIVATE -fno-objc-arc)
    target_link_libraries(
        nvdialog
        PRIVATE
            "-framework AppKit"
            "-framework Cocoa"
            "-framework Foundation"
            "-framework UserNotifications"
    )
else()
    target_link_libraries(nvdialog PRIVATE PkgConfig::GTK PkgConfig::DBUS)
endif()

# ---- tiny-process-library ----
if(WIN32)
    add_library(
        tiny-process-library
        STATIC
        src/libs/tiny-process-library/process.cpp
        src/libs/tiny-process-library/process_win.cpp
    )
    # MSYS2 builds run commands with sh
    if(MSYS)
        target_compile_definitions(
            tiny-process-library
            PUBLIC MSYS_PROCESS_USE_SH
        )
    endif()
else()
    add_library(
        tiny-process-library
        STATIC
        src/libs/tiny-process-library/process.cpp
        src/libs/tiny-process-library/process_unix.cpp
    )
endif()

if(MSVC)
    target_compile_definitions(
        tiny-process-library
        PRIVATE _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(tiny-process-library PRIVATE -Wall -Wextra)
endif()
target_include_directories(
    tiny-process-library
    PUBLIC include/libs/tiny-process-library
)
target_link_libraries(tiny-process-library PUBLIC Threads::Threads)

# ---- shared by the vendored libraries ----
set_target_properties(
    tfd
    nvdialog
    PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
)
enable_lto(
    fmt
    toml++
    tfd
    imgui
    tray
    clip
    tinygettext
    nvdialog
    tiny-process-library
)

# =============================================================================
# oshot_common, everything but main()
# =============================================================================
# With plugins it's a SHARED library, since oshot, oshotpm and the libraries
# they load must share one copy of the globals in globals.cpp (g_ss_tool,
# g_config, g_cache, ...), and an OBJECT library would copy them into each
# of its users, making the copies drift apart and get destroyed twice.
# Without plugins, oshot is its only user, so the simpler OBJECT library is
# enough.
if(NOT DISABLE_PLUGINS)
    set(OSHOT_COMMON_LIB_TYPE SHARED)
else()
    set(OSHOT_COMMON_LIB_TYPE OBJECT)
endif()

set(OSHOT_COMMON_SOURCES
    src/cache.cpp
    src/clipboard.cpp
    src/config.cpp
    src/globals.cpp
    src/hotkey.cpp
    src/i18n.cpp
    src/screen_capture.cpp
    src/screenshot_tool.cpp
    src/text_extraction.cpp
    src/util.cpp
    ${TRANSLATIONS_SOURCE}
)

# The plugin and oshotpm sources are compiled in too. globals.cpp makes a
# StateManager and screenshot_tool.cpp loads plugins, while those sources need
# oshot_common, so as libraries of their own they'd form a link cycle
if(NOT DISABLE_PLUGINS)
    list(
        APPEND OSHOT_COMMON_SOURCES
        src/plugins.cpp
        src/plugins/oshot_plugins.cc
        oshotpm/src/manifest.cc
        oshotpm/src/plugin_index.cc
        oshotpm/src/plugin_manager.cc
        oshotpm/src/state_manager.cc
    )
endif()

# spdlog's implementation is compiled once, here, instead of in every file
# that includes it (header-only, it cost each of them ~6 s at -O3). Being in
# oshot_common also gives oshot, oshotpm and liboshot_common one registry
set(SPDLOG_SOURCES
    src/libs/spdlog/async.cpp
    src/libs/spdlog/color_sinks.cpp
    src/libs/spdlog/file_sinks.cpp
    src/libs/spdlog/spdlog.cpp
)
list(APPEND OSHOT_COMMON_SOURCES ${SPDLOG_SOURCES})

# AppleClang's LTO hides explicit template instantiations like
# base_sink<std::mutex> from liboshot_common's exports, so oshot can't link
if(APPLE)
    set_source_files_properties(
        ${SPDLOG_SOURCES}
        PROPERTIES COMPILE_OPTIONS -fno-lto
    )
endif()

if(UNIX AND NOT APPLE)
    list(APPEND OSHOT_COMMON_SOURCES src/wl_get_monitors.cpp ${PROTOCOL_SOURCE})
endif()

add_library(oshot_common ${OSHOT_COMMON_LIB_TYPE} ${OSHOT_COMMON_SOURCES})
enable_lto(oshot_common)

# globals.cpp includes the generated version.h
add_dependencies(oshot_common generate_version)

# PkgConfig::LEPTONICA's include dirs lack the parent directory added above
target_include_directories(
    oshot_common
    PUBLIC include/ include/libs ${LEPTONICA_INCLUDE_DIRS}
)
target_compile_definitions(
    oshot_common
    PUBLIC
        VERSION="${PROJECT_VERSION}"
        TOML_HEADER_ONLY=0
        SPDLOG_COMPILED_LIB
        ${OSHOT_OPTION_DEFINITIONS}
    PRIVATE $<$<CONFIG:Debug>:DEBUG=1>
)
if(TESSERACT_DATADIR)
    target_compile_definitions(
        oshot_common
        PRIVATE OSHOT_TESSERACT_DATADIR="${TESSERACT_DATADIR}"
    )
endif()

if(WIN32)
    target_link_libraries(oshot_common PRIVATE ws2_32 shcore d3d11 dxgi)
    if(MINGW)
        target_link_options(oshot_common PRIVATE -Wl,--export-all-symbols)
    endif()
elseif(APPLE)
    target_compile_options(
        oshot_common
        PRIVATE -fobjc-arc $<$<CXX_COMPILER_ID:AppleClang>:-stdlib=libc++>
    )
    target_link_options(
        oshot_common
        PRIVATE $<$<CXX_COMPILER_ID:AppleClang>:-stdlib=libc++>
    )
    target_link_libraries(
        oshot_common
        PRIVATE
            "-framework Metal"
            "-framework QuartzCore"
            "-framework CoreGraphics"
            "-framework Cocoa"
            "-framework Carbon"
            "-framework IOKit"
            "-framework OpenGL"
    )
else()
    # GTK, since util.hpp calls gtk_events_pending()/gtk_main_iteration()
    target_link_libraries(
        oshot_common
        PRIVATE
            X11::X11
            PkgConfig::GTK
            PkgConfig::GIO
            PkgConfig::XRANDR
            PkgConfig::WAYLAND
            PkgConfig::FONTCONFIG
    )
    add_dependencies(oshot_common generate_protocol)
endif()

# i18n.cpp parses the translations with tinygettext
target_link_libraries(
    oshot_common
    PUBLIC
        fmt
        toml++
        tfd
        imgui
        tray
        clip
        nvdialog
        tiny-process-library
        PkgConfig::TESSERACT
        PkgConfig::LEPTONICA
        PkgConfig::ZBAR
        OpenGL::GL
        ${GLFW_TARGET}
    PRIVATE tinygettext Threads::Threads ZLIB::ZLIB
)
if(OpenMP_CXX_FOUND)
    target_link_libraries(oshot_common PRIVATE OpenMP::OpenMP_CXX)
endif()

if(NOT DISABLE_PLUGINS)
    set_target_properties(
        oshot_common
        PROPERTIES
            VERSION 1
            SOVERSION 1
            WINDOWS_EXPORT_ALL_SYMBOLS ON
            # So it finds cimgui and imgui in lib/oshot, since the
            # executables' RUNPATH isn't used for their libraries' dependencies
            INSTALL_RPATH "${_rpath_origin}"
    )
    target_include_directories(
        oshot_common
        PUBLIC ${CMAKE_SOURCE_DIR}/oshotpm/include
    )
    # Plugins use cimgui. Since oshot already loads it, they find it without
    # linking it (Linux/BSD/macOS) or having to locate it (AppImage)
    target_link_libraries(oshot_common PRIVATE dylib cimgui)
endif()

# =============================================================================
# oshot executable
# =============================================================================
add_executable(
    ${PROJECT_NAME}
    src/main.cpp
    src/main_tool_opengl3.cpp
    src/libs/getopt_port/getopt.c
)
enable_lto(${PROJECT_NAME})
add_dependencies(${PROJECT_NAME} generate_version)

target_compile_definitions(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:DEBUG=1>)
target_compile_options(
    ${PROJECT_NAME}
    PRIVATE
        ${OSHOT_WARNING_FLAGS}
        $<$<CONFIG:Debug>:-ggdb3;-fno-omit-frame-pointer>
)
target_link_libraries(${PROJECT_NAME} PRIVATE oshot_common)
set_target_properties(
    ${PROJECT_NAME}
    PROPERTIES
        BUILD_RPATH "${OSHOT_BUILD_RPATH}"
        INSTALL_RPATH "${OSHOT_INSTALL_RPATH}"
)

if(APPLE AND NOT MACOS_OPENGL)
    target_sources(${PROJECT_NAME} PRIVATE src/main_tool_metal.mm)
endif()

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    # WinMain() loads the DPI awareness functions at runtime
    target_link_libraries(${PROJECT_NAME} PRIVATE dylib)

    # The icon and version info, for Explorer, the taskbar and the installer,
    # and the manifest that sets the UTF-8 code page (oshotpm and the tests
    # embed it too)
    enable_language(RC)
    set(OSHOT_ICO_PATH ${CMAKE_CURRENT_SOURCE_DIR}/assets/windows/oshot.ico)
    set(OSHOT_MANIFEST_PATH
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/windows/oshot.manifest
    )
    configure_file(
        assets/windows/oshot.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/oshot.rc
        @ONLY
    )
    configure_file(
        assets/windows/manifest.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/manifest.rc
        @ONLY
    )
    target_sources(
        ${PROJECT_NAME}
        PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/oshot.rc
            ${CMAKE_CURRENT_BINARY_DIR}/manifest.rc
    )
elseif(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE X11::X11)
endif()

# =============================================================================
# oshotpm (oshot plugin manager CLI)
# =============================================================================
if(NOT DISABLE_PLUGINS)
    add_executable(oshotpm oshotpm/src/main.cpp)
    enable_lto(oshotpm)
    target_link_libraries(oshotpm PRIVATE oshot_common)
    set_target_properties(
        oshotpm
        PROPERTIES
            BUILD_RPATH "${OSHOT_BUILD_RPATH}"
            INSTALL_RPATH "${OSHOT_INSTALL_RPATH}"
    )
    if(WIN32)
        target_sources(oshotpm PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/manifest.rc)
    endif()
endif()

# =============================================================================
# Plugin SDK and example plugins
# =============================================================================
if(NOT DISABLE_PLUGINS)
    # The plugin SDK in the build tree, which is what packages ship next to
    # oshot and what oshotpm passes to plugin builds as OSHOT_SDK. Windows
    # plugins also need cimgui's import library.
    set(OSHOT_SDK_DIR "${CMAKE_BINARY_DIR}/sdk")
    set(OSHOT_SDK_HEADERS_SRC
        "${CMAKE_SOURCE_DIR}/src/plugins/oshot_plugin.h"
        "${CMAKE_SOURCE_DIR}/src/plugins/oshot_mock_host.h"
        "${CMAKE_SOURCE_DIR}/include/libs/cimgui.h"
    )
    set(OSHOT_SDK_HEADERS
        "${OSHOT_SDK_DIR}/include/oshot/oshot_plugin.h"
        "${OSHOT_SDK_DIR}/include/oshot/oshot_mock_host.h"
        "${OSHOT_SDK_DIR}/include/oshot/cimgui.h"
    )
    # A custom command with outputs, not a POST_BUILD step on cimgui. Ninja
    # compiles objects before the libraries they link are built, so the example
    # plugins would race the copy.
    add_custom_command(
        OUTPUT ${OSHOT_SDK_HEADERS}
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${OSHOT_SDK_DIR}/include/oshot"
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different ${OSHOT_SDK_HEADERS_SRC}
            "${OSHOT_SDK_DIR}/include/oshot/"
        DEPENDS ${OSHOT_SDK_HEADERS_SRC}
        COMMENT "Copying the plugin SDK headers to ${OSHOT_SDK_DIR}"
    )
    add_custom_target(oshot_sdk_headers ALL DEPENDS ${OSHOT_SDK_HEADERS})

    # find_package(oshot) and pkg-config files (oshot-plugin, oshot-cimgui),
    # in the SDK and installed. They find the headers relative to themselves,
    # so the SDK can be copied anywhere (packages, AppImage).
    include(CMakePackageConfigHelpers)
    set(OSHOT_SDK_FILES "${CMAKE_SOURCE_DIR}/src/plugins/sdk")
    configure_file(
        "${OSHOT_SDK_FILES}/oshotConfig.cmake"
        "${OSHOT_SDK_DIR}/lib/cmake/oshot/oshotConfig.cmake"
        COPYONLY
    )
    write_basic_package_version_file(
        "${OSHOT_SDK_DIR}/lib/cmake/oshot/oshotConfigVersion.cmake"
        COMPATIBILITY AnyNewerVersion
        ARCH_INDEPENDENT
    )
    if(WIN32)
        set(OSHOT_PC_CIMGUI_LIBS "-L\${libdir} -lcimgui")
    elseif(APPLE)
        set(OSHOT_PC_CIMGUI_LIBS "-Wl,-undefined,dynamic_lookup")
    else()
        set(OSHOT_PC_CIMGUI_LIBS "")
    endif()
    # ${pcfiledir} is <prefix>/<libdir>/pkgconfig
    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
        file(
            RELATIVE_PATH OSHOT_PC_INSTALL_PREFIX
            "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
            "${CMAKE_INSTALL_PREFIX}"
        )
    else()
        file(
            RELATIVE_PATH OSHOT_PC_INSTALL_PREFIX
            "/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
            "/prefix"
        )
    endif()
    string(REGEX REPLACE "/$" "" OSHOT_PC_INSTALL_PREFIX "${OSHOT_PC_INSTALL_PREFIX}")
    foreach(_pc oshot-plugin oshot-cimgui)
        set(OSHOT_PC_PREFIX "../..")
        configure_file(
            "${OSHOT_SDK_FILES}/${_pc}.pc.in"
            "${OSHOT_SDK_DIR}/lib/pkgconfig/${_pc}.pc"
            @ONLY
        )
        set(OSHOT_PC_PREFIX "${OSHOT_PC_INSTALL_PREFIX}")
        configure_file(
            "${OSHOT_SDK_FILES}/${_pc}.pc.in"
            "${CMAKE_BINARY_DIR}/pkgconfig-install/${_pc}.pc"
            @ONLY
        )
    endforeach()
    if(WIN32)
        add_custom_command(
            TARGET cimgui
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${OSHOT_SDK_DIR}/lib"
            COMMAND
                ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_LINKER_FILE:cimgui>" "${OSHOT_SDK_DIR}/lib/"
        )
    endif()

    # The C/C++ example plugins in examples/plugins, as oshot_example_<name>,
    # with -DBUILD_EXAMPLES=ON. CI builds them to check that they compile
    # everywhere. They aren't installed.
    if(BUILD_EXAMPLES)
        foreach(
            _example
            uppercase:c/uppercase.c
            uppercase_cpp:cpp/uppercase.cpp
            line_lengths:cimgui/line_lengths.c
            scan_results:scan-results/scan_results.c
            join_lines:text-transformers/join_lines.c
            extract_links:text-transformers/extract_links.c
            export_png:export-png/export_png.c
        )
            string(REPLACE ":" ";" _example "${_example}")
            list(GET _example 0 _name)
            list(GET _example 1 _source)
            add_library(
                oshot_example_${_name}
                MODULE
                examples/plugins/${_source}
                ${OSHOT_SDK_HEADERS}
            )
            target_include_directories(
                oshot_example_${_name}
                PRIVATE "${OSHOT_SDK_DIR}/include"
            )
            set_target_properties(
                oshot_example_${_name}
                PROPERTIES
                    C_VISIBILITY_PRESET hidden
                    CXX_VISIBILITY_PRESET hidden
                    VISIBILITY_INLINES_HIDDEN ON
            )
        endforeach()

        target_include_directories(
            oshot_example_export_png
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include/libs/
        )
        target_link_libraries(oshot_example_export_png PRIVATE Threads::Threads)

        # Only the cimgui one uses cimgui
        add_dependencies(oshot_example_line_lengths cimgui)
        if(WIN32)
            target_link_libraries(oshot_example_line_lengths PRIVATE cimgui)
        elseif(APPLE)
            target_link_options(
                oshot_example_line_lengths
                PRIVATE -undefined dynamic_lookup
            )
        endif()
    endif()
endif()

# =============================================================================
# Tests
# =============================================================================
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# =============================================================================
# Install
# =============================================================================
if(DISABLE_PLUGINS)
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    install(
        TARGETS ${PROJECT_NAME} imgui cimgui oshot_common oshotpm
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${OSHOT_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${OSHOT_INSTALL_LIBDIR}
    )
    install(
        FILES ${OSHOT_SDK_HEADERS_SRC}
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
    )
    install(
        FILES
            "${OSHOT_SDK_DIR}/lib/cmake/oshot/oshotConfig.cmake"
            "${OSHOT_SDK_DIR}/lib/cmake/oshot/oshotConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/oshot
    )
    install(
        FILES
            "${CMAKE_BINARY_DIR}/pkgconfig-install/oshot-plugin.pc"
            "${CMAKE_BINARY_DIR}/pkgconfig-install/oshot-cimgui.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endif()

if(OSHOT_INSTALL_LICENSEDIR)
    install(FILES LICENSE DESTINATION ${OSHOT_INSTALL_LICENSEDIR})
endif()

# Desktop integration for Linux and the BSDs (freedesktop.org)
if(UNIX AND NOT APPLE)
    # KWin before Plasma 6.8 matches X-KDE-DBUS-Restricted-Interfaces against the caller's real
    # executable, so the installed desktop file needs oshot's absolute path. It's written at
    # install time, since `cmake --install --prefix` (make install) can change the prefix.
    install(
        CODE
            "set(OSHOT_DESKTOP_SRC \"${CMAKE_CURRENT_SOURCE_DIR}/assets/linux/oshot.desktop\")
             set(OSHOT_DESKTOP_TMP \"${CMAKE_CURRENT_BINARY_DIR}/oshot.desktop\")
             set(OSHOT_BINDIR \"${CMAKE_INSTALL_BINDIR}\")
             set(OSHOT_APPSDIR \"${CMAKE_INSTALL_DATAROOTDIR}/applications\")"
    )
    install(
        CODE
            [[
            foreach(dir OSHOT_BINDIR OSHOT_APPSDIR)
                if(NOT IS_ABSOLUTE "${${dir}}")
                    set(${dir} "${CMAKE_INSTALL_PREFIX}/${${dir}}")
                endif()
            endforeach()
            file(READ "${OSHOT_DESKTOP_SRC}" entry)
            string(REPLACE "\nExec=oshot" "\nExec=${OSHOT_BINDIR}/oshot" entry "${entry}")
            file(WRITE "${OSHOT_DESKTOP_TMP}" "${entry}")
            file(INSTALL DESTINATION "${OSHOT_APPSDIR}" TYPE FILE FILES "${OSHOT_DESKTOP_TMP}")
            ]]
    )
    install(
        FILES assets/linux/io.github.toni500github.oshot.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    )
    install(
        FILES assets/oshot.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
    )
    install(
        FILES assets/oshot.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps
    )

    set(OSHOT_MAN_PAGES docs/oshot.1)
    if(NOT DISABLE_PLUGINS)
        list(APPEND OSHOT_MAN_PAGES docs/oshotpm.1)
    endif()
    install(FILES ${OSHOT_MAN_PAGES} DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
endif()
