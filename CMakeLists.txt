cmake_minimum_required(VERSION 3.15)

project(oshot VERSION 0.2.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# LTO helper
function(enable_lto target)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(
            ${target}
            PRIVATE $<$<CONFIG:Release>:-flto=auto;-ffat-lto-objects>
        )
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-flto=auto>)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(
            ${target}
            PRIVATE $<$<CONFIG:Release>:-flto=thin>
        )
        target_link_options(${target} PRIVATE $<$<CONFIG:Release>:-flto=thin>)
    endif()
endfunction()

# https://github.com/libcpr/cpr/blob/5f475522597b8f3721e2440daddeced7a969f24c/CMakeLists.txt#L39
macro(add_option OPTION_NAME OPTION_TEXT OPTION_DEFAULT DEFINE_OPTION)
    option(${OPTION_NAME} ${OPTION_TEXT} ${OPTION_DEFAULT})
    if(DEFINED ENV{${OPTION_NAME}})
        # Allow overriding the option through an environment variable
        set(${OPTION_NAME} $ENV{${OPTION_NAME}})
    endif()
    if(${OPTION_NAME} AND DEFINE_OPTION)
        add_definitions(-D${OPTION_NAME}=1)
    endif()
    message(STATUS "  ${OPTION_NAME}: ${${OPTION_NAME}}")
endmacro()

# Options
add_option(WINDOWS_CMD "Enable terminal support on Windows" OFF OFF)

# -----------------------------
# Dependencies
# -----------------------------

find_package(PkgConfig REQUIRED)

find_package(glfw3 REQUIRED)

if(TARGET glfw::glfw)
    set(GLFW_TARGET glfw::glfw)
elseif(TARGET glfw)
    set(GLFW_TARGET glfw)
elseif(TARGET glfw3)
    set(GLFW_TARGET glfw3)
else()
    message(FATAL_ERROR "GLFW found but no usable CMake target was exported")
endif()

find_package(OpenGL REQUIRED)
find_package(CURL REQUIRED)
pkg_check_modules(TESSERACT REQUIRED tesseract)
pkg_check_modules(LEPTONICA REQUIRED lept)
pkg_check_modules(ZBAR REQUIRED zbar)

# -----------------------------
# Main executable
# -----------------------------

file(
    GLOB SRC
    src/*.cpp
    src/libs/toml++/toml.cpp
    src/libs/getopt_port/getopt.c
    src/libs/tinyfiledialogs/tinyfiledialogs.c
)

add_executable(${PROJECT_NAME} ${SRC})
enable_lto(${PROJECT_NAME})

execute_process(
    COMMAND ./scripts/generateVersion.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

target_compile_options(
    ${PROJECT_NAME}
    PRIVATE
        $<$<CONFIG:Debug>:-DDEBUG=1
        -ggdb3
        -fno-omit-frame-pointer
        -Wall
        -Wextra
        -pedantic
        -Wno-unused-parameter>
)

target_compile_definitions(${PROJECT_NAME} PRIVATE VERSION="${PROJECT_VERSION}")

target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
        include/
        include/libs
        ${TESSERACT_INCLUDE_DIRS}
        ${LEPTONICA_INCLUDE_DIRS}
        ${APPINDICATOR_INCLUDE_DIRS}
        ${ZBAR_INCLUDE_DIRS}
)

target_compile_options(
    ${PROJECT_NAME}
    PRIVATE
        ${TESSERACT_CFLAGS_OTHER}
        ${LEPTONICA_CFLAGS_OTHER}
        ${ZBAR_CFLAGS_OTHER}
)

target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
        OpenGL::GL
        CURL::libcurl
        ${GLFW_TARGET}
        ${TESSERACT_LIBRARIES}
        ${LEPTONICA_LIBRARIES}
        ${ZBAR_LIBRARIES}
)

if(WIN32)
    target_link_libraries(
        ${PROJECT_NAME}
        PRIVATE ws2_32 shcore d3d11 dxgi)

    if(NOT WINDOWS_CMD)
        set_target_properties(${PROJECT_NAME} PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
elseif(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE X11::X11)
    pkg_check_modules(APPINDICATOR REQUIRED appindicator3-0.1)
    pkg_check_modules(GIO REQUIRED gio-2.0)
    target_include_directories(
         ${PROJECT_NAME}
         PRIVATE
         SYSTEM
         ${GIO_INCLUDE_DIRS}
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE ${GIO_LIBRARIES})
    target_compile_options(${PROJECT_NAME} PRIVATE ${GIO_CFLAGS_OTHER})
endif()

# -----------------------------
# fmt library
# -----------------------------

add_library(fmt STATIC src/libs/fmt/os.cc src/libs/fmt/format.cc)

set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
enable_lto(fmt)

target_include_directories(fmt PUBLIC include/libs)
target_compile_features(fmt PUBLIC cxx_std_20)

target_link_libraries(${PROJECT_NAME} PRIVATE fmt)

# -----------------------------
# imgui library
# -----------------------------

file(GLOB IMGUI_SOURCES src/libs/imgui/*.cpp)

add_library(imgui STATIC ${IMGUI_SOURCES})

set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
enable_lto(imgui)

target_include_directories(imgui PUBLIC include/libs/imgui)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)
target_link_libraries(imgui PUBLIC ${GLFW_TARGET})

target_link_libraries(${PROJECT_NAME} PRIVATE imgui)

# -----------------------------
# trayapp library (integrated from its own CMakeLists.txt)
# -----------------------------

file(
    GLOB TRAYAPP_SOURCES
    src/libs/trayapp/*/*.cpp
    src/libs/trayapp/*/*/*.cpp
)

add_library(trayapp STATIC ${TRAYAPP_SOURCES})

if(UNIX)
    target_include_directories(
        trayapp
        PUBLIC
        SYSTEM
        ${APPINDICATOR_INCLUDE_DIRS}
    )
    target_link_libraries(trayapp PUBLIC ${APPINDICATOR_LIBRARIES})
endif()

target_include_directories(trayapp PUBLIC include/libs/trayapp)
target_compile_features(trayapp PRIVATE cxx_std_20)
set_target_properties(
    trayapp
    PROPERTIES
        CXX_STANDARD 20
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD_REQUIRED ON
)

target_link_libraries(${PROJECT_NAME} PRIVATE trayapp)

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
elseif(UNIX AND NOT APPLE)
    target_sources(clip PRIVATE src/libs/clip/clip_x11.cpp)
else()
    target_sources(clip PRIVATE src/libs/clip/clip_none.cpp)
endif()

if(MSVC)
    target_compile_definitions(clip PRIVATE /D_CRT_SECURE_NO_WARNINGS)
# MinGW requires the windowscodecs just because CLSIDs are defined
# in the windowscodecs.a file instead of the wincodec.h file (?!)
elseif (MINGW)
  target_link_libraries(clip PUBLIC shlwapi windowscodecs)
endif()

if(UNIX AND NOT APPLE)
    find_package(Threads REQUIRED)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
    pkg_check_modules(LIBPNG REQUIRED IMPORTED_TARGET libpng)
    target_link_libraries(
        clip
        PRIVATE PkgConfig::XCB Threads::Threads PkgConfig::LIBPNG
    )
endif()

target_include_directories(clip PUBLIC include/libs/clip)
target_compile_features(clip PRIVATE cxx_std_20)
target_compile_definitions(
    clip
    PUBLIC -DCLIP_ENABLE_IMAGE=1 -DCLIP_X11_WITH_PNG=1 -DHAVE_PNG_H=1
)
set_target_properties(
    clip
    PROPERTIES
        CXX_STANDARD 20
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
        CXX_STANDARD_REQUIRED ON
)

target_link_libraries(${PROJECT_NAME} PRIVATE clip)

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
        PRIVATE /D_CRT_SECURE_NO_WARNINGS
    )
else()
    target_compile_options(tiny-process-library PRIVATE -Wall -Wextra)
    target_compile_features(tiny-process-library PRIVATE cxx_std_20)
endif()

if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_sources(
        tiny-process-library
        PRIVATE "src/libs/tiny-process-library/process_win.cpp"
    )
    # If compiled using MSYS2, use sh to run commands
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

set_target_properties(
    tiny-process-library
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)

find_package(Threads REQUIRED)
target_link_libraries(tiny-process-library Threads::Threads)
target_include_directories(
    tiny-process-library
    PUBLIC include/libs/tiny-process-library
)
enable_lto(tiny-process-library)
target_link_libraries(${PROJECT_NAME} PRIVATE tiny-process-library)

# -----------------------------
# Install
# -----------------------------

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    COMPONENT runtime
)

install(
    PROGRAMS launcher/${PROJECT_NAME}_launcher.py
    DESTINATION bin
    RENAME ${PROJECT_NAME}-launcher
)

install(
    FILES LICENSE
    DESTINATION share/licenses/${PROJECT_NAME}
    COMPONENT documentation
)
