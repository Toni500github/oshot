cmake_minimum_required(VERSION 3.15)

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

if(NOT MSVC AND NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "-O")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
endif()

# -----------------------------
# Helpers
# -----------------------------

# Enable LTO for a target in Release builds.
# Never on Windows: one link mixing LTO and non-LTO objects (oshot_common
# needs -Wl,--export-all-symbols, or is an OBJECT library merged into
# oshot.exe) hits GCC/MinGW bugs folding weak COMDAT symbols, like spdlog's
# thread_local statics and template instantiations.
function(enable_lto target)
    if(WIN32)
        return()
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(
            ${target}
            PRIVATE $<$<CONFIG:Release>:-flto=auto;-ffat-lto-objects>
        )
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-flto=auto>)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:-flto>)
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-flto>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(
            ${target}
            PRIVATE $<$<CONFIG:Release>:-flto=thin>
        )
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
    endif()
endfunction()

# option() that an environment variable of the same name can override.
# With DEFINE_OPTION, an enabled option adds OPTION_NAME=1 to
# OSHOT_OPTION_DEFINITIONS, which only oshot_common gets: add_definitions()
# would leak it into the vendored libraries too.
# https://github.com/libcpr/cpr/blob/5f475522597b8f3721e2440daddeced7a969f24c/CMakeLists.txt#L39
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

# -----------------------------
# Options
# -----------------------------
add_option(DISABLE_PLUGINS "Disable plugins support" ON ON)
add_option(MACOS_OPENGL "Draw the overlay with OpenGL instead of Metal on macOS, for VMs without Metal" OFF ON)

# Arch-style default; Debian sets it empty, as the license goes into
# /usr/share/doc/oshot/copyright there
set(OSHOT_INSTALL_LICENSEDIR
    "${CMAKE_INSTALL_DATAROOTDIR}/licenses/${PROJECT_NAME}"
    CACHE STRING
    "Where to install the LICENSE file, relative to the prefix (empty to skip)"
)

# -----------------------------
# Dependencies
# -----------------------------
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(glfw3 REQUIRED)
find_package(OpenGL REQUIRED)

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
# Where tesseract was installed to, one of the places to look for its models
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

if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    # Imported targets carry the full library paths: on the BSDs these
    # live in /usr/local/lib, which the linker doesn't search by default
    pkg_check_modules(APPINDICATOR IMPORTED_TARGET appindicator3-0.1)
    if(NOT APPINDICATOR_FOUND)
        pkg_check_modules(
            APPINDICATOR
            REQUIRED
            IMPORTED_TARGET
            ayatana-appindicator3-0.1
        )
    endif()
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
    pkg_check_modules(XRANDR REQUIRED IMPORTED_TARGET xrandr)
    pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    # A UI font covering the language's script (util.cpp, find_language_fonts())
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
endif()

# -----------------------------
# Generated sources
# -----------------------------

# include/version.h, with the git commit, branch and tag
set(VERSION_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/version.h)
set(VERSION_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generateVersion.sh)
# Runs on every build, so a new commit or tag is picked up. The script only rewrites
# the file when it changed, and only src/globals.cpp includes it
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

# The Wayland xdg-output protocol, for the monitors' logical geometry (wl_get_monitors.cpp)
if(UNIX AND NOT APPLE)
    set(PROTOCOL_BASE_NAME xdg-output-unstable-v1)
    set(PROTOCOL_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/include/${PROTOCOL_BASE_NAME}-client-protocol.h
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

# -----------------------------
# rpath
# -----------------------------
if(APPLE)
    set(_rpath_origin "@loader_path")
else()
    set(_rpath_origin "$ORIGIN")
endif()

# Relative, so the install stays relocatable (zip, AppImage), and following
# CMAKE_INSTALL_LIBDIR, which is lib64 on Fedora/openSUSE and
# lib/<multiarch> on Debian
file(
    RELATIVE_PATH
    _bin_to_lib
    "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}"
)
set(OSHOT_INSTALL_RPATH "${_rpath_origin}/${_bin_to_lib}")

# With plugins, oshot_common is a shared library that the executables
# have to find in the build tree too
if(NOT DISABLE_PLUGINS)
    set(OSHOT_COMMON_RPATH_ENTRY ";$<TARGET_FILE_DIR:oshot_common>")
else()
    set(OSHOT_COMMON_RPATH_ENTRY "")
endif()

# -----------------------------
# oshot_common: everything but main()
# -----------------------------
# With plugins it's a SHARED library: oshot, oshotpm and the libraries they
# load must share one copy of the globals in globals.cpp (g_ss_tool,
# g_config, g_cache, ...), and an OBJECT library would copy them into each
# of its users, making the copies drift apart and get destroyed twice.
# Without plugins oshot is its only user, so the simpler OBJECT library is enough.
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

# The plugin and oshotpm sources are compiled in too: globals.cpp makes a
# StateManager and screenshot_tool.cpp loads plugins, while they need
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

if(UNIX AND NOT APPLE)
    list(APPEND OSHOT_COMMON_SOURCES src/wl_get_monitors.cpp ${PROTOCOL_SOURCE})
    set_source_files_properties(
        src/wl_get_monitors.cpp
        ${PROTOCOL_SOURCE}
        PROPERTIES COMPILE_FLAGS "-Wno-unused-parameter"
    )
endif()

add_library(oshot_common ${OSHOT_COMMON_LIB_TYPE} ${OSHOT_COMMON_SOURCES})
enable_lto(oshot_common)

# util.hpp includes version.h
add_dependencies(oshot_common generate_version)

if(OSHOT_OPTION_DEFINITIONS)
    target_compile_definitions(oshot_common PUBLIC ${OSHOT_OPTION_DEFINITIONS})
endif()

if(NOT DISABLE_PLUGINS)
    set_target_properties(
        oshot_common
        PROPERTIES
            VERSION 1
            SOVERSION 1
            WINDOWS_EXPORT_ALL_SYMBOLS ON
            POSITION_INDEPENDENT_CODE ON
    )
    target_include_directories(
        oshot_common
        PUBLIC ${CMAKE_SOURCE_DIR}/oshotpm/include
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
    # GTK: util.hpp calls gtk_events_pending()/gtk_main_iteration()
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

# i18n.cpp parses the translations with it
target_link_libraries(oshot_common PRIVATE tinygettext)

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
    PRIVATE Threads::Threads
)
if(NOT DISABLE_PLUGINS)
    # cimgui: plugins use it, and oshot loading it means they find it without
    # linking it (Linux/BSD/macOS) or having to locate it (AppImage)
    target_link_libraries(oshot_common PRIVATE dylib cimgui)
endif()

target_compile_definitions(
    oshot_common
    PUBLIC VERSION="${PROJECT_VERSION}" TOML_HEADER_ONLY=0
)
if(TESSERACT_DATADIR)
    target_compile_definitions(
        oshot_common
        PRIVATE OSHOT_TESSERACT_DATADIR="${TESSERACT_DATADIR}"
    )
endif()

# PkgConfig::LEPTONICA's include dirs lack the parent directory added above
target_include_directories(
    oshot_common
    PUBLIC include/ include/libs ${LEPTONICA_INCLUDE_DIRS}
)

# -----------------------------
# oshot executable
# -----------------------------
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
        -Wall
        -Wextra
        -Wpedantic
        -Wno-unused-parameter
        $<$<CONFIG:Debug>:-ggdb3;-fno-omit-frame-pointer>
)
target_include_directories(
    ${PROJECT_NAME}
    PUBLIC include/ include/libs ${LEPTONICA_INCLUDE_DIRS}
)
target_compile_definitions(
    ${PROJECT_NAME}
    PUBLIC VERSION="${PROJECT_VERSION}" TOML_HEADER_ONLY=0
)

target_link_libraries(${PROJECT_NAME} PRIVATE oshot_common)

set_target_properties(
    ${PROJECT_NAME}
    PROPERTIES
        BUILD_RPATH
            "${_rpath_origin}/../lib;$<TARGET_FILE_DIR:imgui>${OSHOT_COMMON_RPATH_ENTRY}"
        INSTALL_RPATH "${OSHOT_INSTALL_RPATH}"
)

if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    # WinMain() loads the DPI awareness functions at runtime
    target_link_libraries(${PROJECT_NAME} PRIVATE dylib)

    # The icon and version info, for Explorer, the taskbar and the installer,
    # and the manifest that sets the UTF-8 code page
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
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE X11::X11)
endif()

if(APPLE AND NOT MACOS_OPENGL)
    target_sources(${PROJECT_NAME} PRIVATE src/main_tool_metal.mm)
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)
endif()

# -----------------------------
# oshotpm (oshot plugin manager CLI)
# -----------------------------
if(NOT DISABLE_PLUGINS)
    add_executable(oshotpm oshotpm/src/main.cpp)
    enable_lto(oshotpm)
    set_target_properties(
        oshotpm
        PROPERTIES
            BUILD_RPATH
                "${_rpath_origin}/../lib;$<TARGET_FILE_DIR:imgui>${OSHOT_COMMON_RPATH_ENTRY}"
            INSTALL_RPATH "${OSHOT_INSTALL_RPATH}"
    )
    target_link_libraries(oshotpm PRIVATE oshot_common)
    if(WIN32)
        target_sources(oshotpm PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/manifest.rc)
    endif()
endif()

# -----------------------------
# Vendor libraries
# -----------------------------
# -----------------------------
# fmt library
# -----------------------------
add_library(fmt STATIC src/libs/fmt/os.cc src/libs/fmt/format.cc)

enable_lto(fmt)

target_include_directories(fmt PUBLIC include/libs)
target_compile_definitions(fmt PUBLIC FMT_HEADER_ONLY=0)

# -----------------------------
# toml++ library
# -----------------------------
add_library(toml++ STATIC src/libs/toml++/toml.cpp)
enable_lto(toml++)

target_include_directories(toml++ PUBLIC include/libs)
target_compile_definitions(toml++ PUBLIC TOML_HEADER_ONLY=0)

# -----------------------------
# tfd (tinyfiledialogs) library
# -----------------------------
add_library(tfd STATIC src/libs/tinyfiledialogs/tinyfiledialogs.c)
enable_lto(tfd)

target_include_directories(tfd PUBLIC include/libs)
set_target_properties(tfd PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

# -----------------------------
# imgui library
# -----------------------------
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
        PROPERTIES
            VERSION 1.92.9
            SOVERSION 1
            WINDOWS_EXPORT_ALL_SYMBOLS ON
            POSITION_INDEPENDENT_CODE ON
    )
endif()
enable_lto(imgui)

target_include_directories(imgui PUBLIC include/libs/imgui)
target_compile_definitions(
    imgui
    PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD IMGUI_DISABLE_OBSOLETE_FUNCTIONS
)
target_link_libraries(imgui PUBLIC ${GLFW_TARGET})

if(APPLE)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    target_link_libraries(
        imgui
        PUBLIC
            ${METAL_FRAMEWORK}
            ${FOUNDATION_FRAMEWORK}
            ${QUARTZCORE_FRAMEWORK}
    )
endif()

# -----------------------------
# cimgui library (C-ABI plugins only)
# -----------------------------
if(NOT DISABLE_PLUGINS)
    add_library(cimgui SHARED src/libs/cimgui/cimgui.cpp)

    target_include_directories(cimgui PUBLIC include/libs include/libs/cimgui)
    target_link_libraries(cimgui PUBLIC imgui)
    set_target_properties(
        cimgui
        PROPERTIES
            VERSION 1
            SOVERSION 1
            WINDOWS_EXPORT_ALL_SYMBOLS ON
            POSITION_INDEPENDENT_CODE ON
    )
endif()

# -----------------------------
# Tray library (integrated from its own CMakeLists.txt)
# -----------------------------
if(WIN32)
    list(APPEND TRAY_SOURCES src/libs/tray/tray_windows.cpp)
elseif(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    list(APPEND TRAY_SOURCES src/libs/tray/tray_darwin.mm)
elseif(UNIX)
    list(APPEND TRAY_SOURCES src/libs/tray/tray_linux.cpp)
endif()

add_library(tray STATIC ${TRAY_SOURCES})

if(WIN32)
    target_compile_definitions(
        tray
        PRIVATE TRAY_WINAPI=1 WIN32_LEAN_AND_MEAN NOMINMAX
    )
elseif(APPLE)
    target_compile_definitions(tray PRIVATE TRAY_APPKIT=1)
    target_link_libraries(tray PRIVATE ${COCOA_FRAMEWORK})
elseif(UNIX)
    target_compile_definitions(tray PRIVATE TRAY_APPINDICATOR=1)
    target_link_libraries(tray PRIVATE PkgConfig::GTK PkgConfig::APPINDICATOR)
endif()

target_include_directories(tray PUBLIC include/libs)

# -----------------------------
# clip library (integrated from its own CMakeLists.txt)
# -----------------------------
add_library(clip STATIC src/libs/clip/clip.cpp src/libs/clip/image.cpp)

if(WIN32)
    target_sources(
        clip
        PRIVATE
            src/libs/clip/clip_win.cpp
            src/libs/clip/clip_win_bmp.cpp
            src/libs/clip/clip_win_wic.cpp
    )
elseif(APPLE)
    target_sources(clip PRIVATE src/libs/clip/clip_osx.mm)
else()
    # X11 and Wayland go through xclip/wl-copy instead (clipboard.cpp)
    target_sources(clip PRIVATE src/libs/clip/clip_none.cpp)
endif()

if(MSVC)
    target_compile_definitions(clip PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(MINGW)
    # MinGW requires the windowscodecs just because CLSIDs are defined
    # in the windowscodecs.a file instead of the wincodec.h file (?!)
    target_link_libraries(clip PUBLIC shlwapi windowscodecs)
endif()

target_include_directories(clip PUBLIC include/libs/clip)
target_compile_definitions(clip PUBLIC CLIP_ENABLE_IMAGE=1)

# -----------------------------
# tinygettext library (integrated from its own CMakeLists.txt)
# -----------------------------
file(GLOB TINYGETTEXT_SOURCES src/libs/tinygettext/*.cpp)
add_library(tinygettext STATIC ${TINYGETTEXT_SOURCES})

set_target_properties(
    tinygettext
    PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
)
target_include_directories(tinygettext PUBLIC include/libs/tinygettext)
# The .po files are UTF-8 (xgettext, msgmerge and Weblate write them so), and the
# strings go to ImGui as UTF-8: no charset conversion, so no iconv to ship on Windows/macOS.
# PUBLIC: it changes a type in iconv.hpp, which i18n.cpp includes through po_parser.hpp
target_compile_definitions(tinygettext PUBLIC TINYGETTEXT_UTF8_ONLY)

if(WIN32)
    target_compile_definitions(tinygettext PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# -----------------------------
# dylib library (integrated from its own CMakeLists.txt)
# -----------------------------
if(WIN32 OR NOT DISABLE_PLUGINS)
    file(GLOB DYLIB_SOURCES src/libs/dylib/*.cpp)
    add_library(dylib STATIC ${DYLIB_SOURCES})

    if(UNIX)
        target_link_libraries(dylib PRIVATE ${CMAKE_DL_LIBS})
    endif()

    target_include_directories(dylib PUBLIC include/libs/)
endif()

# -----------------------------
# nvdialog (integrated from its own CMakeLists.txt)
# -----------------------------
file(GLOB NVD_COMMON_SOURCES src/libs/nvdialog/*.c)

if(WIN32)
    file(GLOB NVD_BACKEND_SOURCES src/libs/nvdialog/backend/win32/*.c)
elseif(APPLE)
    file(GLOB NVD_BACKEND_SOURCES src/libs/nvdialog/backend/cocoa/*.m)
else()
    file(GLOB NVD_BACKEND_SOURCES src/libs/nvdialog/backend/gtk/*.c)
endif()

add_library(nvdialog STATIC ${NVD_COMMON_SOURCES} ${NVD_BACKEND_SOURCES})
enable_lto(nvdialog)

target_compile_definitions(
    nvdialog
    PUBLIC NVDIALOG_MAXBUF=4096 NVD_STATIC_LINKAGE=1
    PRIVATE NVD_EXPORT_SYMBOLS
)

target_include_directories(
    nvdialog
    PUBLIC include/libs/nvdialog/ include/libs
    PRIVATE src/libs/nvdialog/impl/
)

set_target_properties(nvdialog PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

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

# -----------------------------
# tiny-process-library (integrated from its own CMakeLists.txt)
# -----------------------------
add_library(
    tiny-process-library
    STATIC
    "src/libs/tiny-process-library/process.cpp"
)
add_library(
    tiny-process-library::tiny-process-library
    ALIAS tiny-process-library
)

if(MSVC)
    target_compile_definitions(
        tiny-process-library
        PRIVATE _CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(tiny-process-library PRIVATE -Wall -Wextra)
endif()

if(WIN32)
    target_sources(
        tiny-process-library
        PRIVATE "src/libs/tiny-process-library/process_win.cpp"
    )
    # MSYS2 builds run commands with sh
    if(MSYS)
        target_compile_definitions(
            tiny-process-library
            PUBLIC MSYS_PROCESS_USE_SH
        )
    endif()
else()
    target_sources(
        tiny-process-library
        PRIVATE "src/libs/tiny-process-library/process_unix.cpp"
    )
endif()

target_link_libraries(tiny-process-library PUBLIC Threads::Threads)
target_include_directories(
    tiny-process-library
    PUBLIC include/libs/tiny-process-library
)
enable_lto(tiny-process-library)

# -----------------------------
# Plugin SDK and example plugin
# -----------------------------
if(NOT DISABLE_PLUGINS)
    # The plugin SDK in the build tree: what packages ship next to oshot and
    # what oshotpm passes to plugin builds as OSHOT_SDK. Windows plugins also
    # need cimgui's import library.
    set(OSHOT_SDK_DIR "${CMAKE_BINARY_DIR}/sdk")
    set(OSHOT_SDK_HEADERS_SRC
        "${CMAKE_SOURCE_DIR}/src/plugins/oshot_plugin.h"
        "${CMAKE_SOURCE_DIR}/include/libs/cimgui/cimgui.h"
    )
    set(OSHOT_SDK_HEADERS
        "${OSHOT_SDK_DIR}/include/oshot/oshot_plugin.h"
        "${OSHOT_SDK_DIR}/include/oshot/cimgui.h"
    )
    # A custom command with outputs, not a POST_BUILD step on cimgui: Ninja
    # compiles objects before the libraries they link are built, so the example
    # plugin would race the copy.
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

    # The example plugin, built so CI checks it compiles everywhere. Not installed.
    add_library(
        oshot_example_uppercase
        MODULE
        examples/plugins/uppercase/uppercase.c
        ${OSHOT_SDK_HEADERS}
    )
    target_include_directories(
        oshot_example_uppercase
        PRIVATE "${OSHOT_SDK_DIR}/include"
    )
    set_target_properties(
        oshot_example_uppercase
        PROPERTIES C_VISIBILITY_PRESET hidden
    )
    add_dependencies(oshot_example_uppercase cimgui)
    if(WIN32)
        target_link_libraries(oshot_example_uppercase PRIVATE cimgui)
    elseif(APPLE)
        target_link_options(
            oshot_example_uppercase
            PRIVATE -undefined dynamic_lookup
        )
    endif()
endif()

# -----------------------------
# Install
# -----------------------------
if(DISABLE_PLUGINS)
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    install(
        TARGETS ${PROJECT_NAME} imgui cimgui oshot_common oshotpm
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    # The plugin SDK headers
    install(
        FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/src/plugins/oshot_plugin.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/include/libs/cimgui/cimgui.h"
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME}
    )
endif()

if(OSHOT_INSTALL_LICENSEDIR)
    install(FILES LICENSE DESTINATION ${OSHOT_INSTALL_LICENSEDIR})
endif()

# Desktop integration for Linux and the BSDs (freedesktop.org)
if(UNIX AND NOT APPLE)
    install(
        FILES assets/linux/oshot.desktop
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications
    )
    install(
        FILES assets/linux/io.github.toni500github.oshot.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo
    )
    install(
        FILES assets/linux/oshot.svg
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
    )
    install(
        FILES assets/oshot.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/64x64/apps
    )
    install(FILES assets/linux/oshot.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
    if(NOT DISABLE_PLUGINS)
        install(
            FILES assets/linux/oshotpm.1
            DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
        )
    endif()
endif()
