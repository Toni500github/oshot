cmake_minimum_required(VERSION 3.15)

project(oshot VERSION 0.2.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_DEBUG
    "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG=1 -ggdb3 -fno-omit-frame-pointer -Wall -Wextra -pedantic -Wno-unused-parameter"
)
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include_directories(include)
include_directories(include/libs)
include_directories(include/libs/imgui)

# LTO helper
function(enable_lto target)
    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -flto=auto -ffat-lto-objects)
            target_link_options(${target} PRIVATE -flto=auto)
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -flto=thin)
            target_link_options(${target} PRIVATE -flto=thin)
        endif()
    endif()
endfunction()

# -----------------------------
# Dependencies
# -----------------------------

find_package(PkgConfig REQUIRED)

find_package(glfw3 REQUIRED)

if (TARGET glfw::glfw)
    set(GLFW_TARGET glfw::glfw)
elseif (TARGET glfw)
    set(GLFW_TARGET glfw)
elseif (TARGET glfw3)
    set(GLFW_TARGET glfw3)
else()
    message(FATAL_ERROR "GLFW found but no usable CMake target was exported")
endif()

find_package(OpenGL REQUIRED)
pkg_check_modules(TESSERACT REQUIRED tesseract)
pkg_check_modules(LEPTONICA REQUIRED lept)

# -----------------------------
# Main executable
# -----------------------------

file(GLOB SRC
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

target_compile_definitions(${PROJECT_NAME}
    PRIVATE VERSION="${PROJECT_VERSION}"
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${TESSERACT_INCLUDE_DIRS}
        ${LEPTONICA_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        OpenGL::GL
	${GLFW_TARGET}
        ${TESSERACT_LIBRARIES}
        ${LEPTONICA_LIBRARIES}
)

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32 shcore d3d11 dxgi)
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT DEFINED WINDOWS_CMD)
        target_link_options(${PROJECT_NAME} PRIVATE "-mwindows")
        target_compile_options(${PROJECT_NAME} PRIVATE "-mwindows")
    else()
        target_compile_options(${PROJECT_NAME} PRIVATE "-DWINDOWS_CMD=1")
    endif()
elseif (UNIX AND NOT APPLE)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address -fsanitize=undefined")
    endif()
    find_package(X11 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE X11::X11)
endif()

# -----------------------------
# fmt library
# -----------------------------

add_library(fmt STATIC
    src/libs/fmt/os.cc
    src/libs/fmt/format.cc
)

set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
enable_lto(fmt)

target_include_directories(fmt PUBLIC src/libs/fmt)
target_compile_features(fmt PUBLIC cxx_std_20)

target_link_libraries(${PROJECT_NAME} PRIVATE fmt)

# -----------------------------
# imgui library
# -----------------------------

file(GLOB IMGUI_SOURCES
    src/libs/imgui/*.cpp
)

add_library(imgui STATIC ${IMGUI_SOURCES})

set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
enable_lto(imgui)

target_compile_definitions(imgui PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD)
target_link_libraries(imgui PUBLIC ${GLFW_TARGET})

target_link_libraries(${PROJECT_NAME} PRIVATE imgui)

# -----------------------------
# Install
# -----------------------------

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION bin
    COMPONENT runtime
)

install(PROGRAMS launcher/${PROJECT_NAME}_launcher.py
    DESTINATION bin
    RENAME ${PROJECT_NAME}-launcher
)

install(FILES LICENSE
    DESTINATION share/licenses/${PROJECT_NAME}
    COMPONENT documentation
)
