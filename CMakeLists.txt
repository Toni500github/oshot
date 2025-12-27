cmake_minimum_required(VERSION 3.15)

project(oshot VERSION 0.0.1)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -ggdb3 -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -Wall -Wextra -pedantic -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

include_directories(include)
include_directories(include/libs)
include_directories(include/libs/imgui)

function(enable_lto target)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "enable_lto called on non-existent target: ${target}")
    endif()

    if (CMAKE_BUILD_TYPE STREQUAL "Release")
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE -flto=auto -ffat-lto-objects)
            target_link_options(${target} PRIVATE -flto=auto)
        elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE -flto=thin)
            target_link_options(${target} PRIVATE -flto=thin)
        else()
            message(WARNING "LTO not configured for compiler: ${CMAKE_CXX_COMPILER_ID}")
        endif()
    endif()
endfunction()

find_package(PkgConfig REQUIRED)

# Find GLFW with multiple methods
find_package(glfw3 REQUIRED)
if(NOT glfw3_FOUND)
    pkg_check_modules(GLFW_PKG REQUIRED glfw3)
    if(GLFW_PKG_FOUND)
        set(GLFW_LIBRARIES ${GLFW_PKG_LIBRARIES})
        set(GLFW_INCLUDE_DIRS ${GLFW_PKG_INCLUDE_DIRS})
        set(GLFW_FOUND TRUE)
        message(STATUS "Found GLFW via pkg-config")
    else()
        # Try to find GLFW manually
        find_path(GLFW_INCLUDE_DIR
            NAMES GLFW/glfw3.h
            PATHS
                /usr/include
                /usr/local/include
                /opt/local/include
                /opt/homebrew/include
                ${CMAKE_INSTALL_PREFIX}/include
        )

        find_library(GLFW_LIBRARY
            NAMES glfw glfw3
            PATHS
                /usr/lib
                /usr/local/lib
                /opt/local/lib
                /opt/homebrew/lib
                ${CMAKE_INSTALL_PREFIX}/lib
        )

        if(GLFW_INCLUDE_DIR AND GLFW_LIBRARY)
            set(GLFW_INCLUDE_DIRS ${GLFW_INCLUDE_DIR})
            set(GLFW_LIBRARIES ${GLFW_LIBRARY})
            set(GLFW_FOUND TRUE)
            message(STATUS "Found GLFW manually")
        endif()
    endif()
else()
    # When find_package succeeds via config mode
    # The targets are usually glfw or glfw::glfw
    if(TARGET glfw)
        set(GLFW_LIBRARIES glfw)
    elseif(TARGET glfw::glfw)
        set(GLFW_LIBRARIES glfw::glfw)
    endif()
    get_target_property(GLFW_INCLUDE_DIRS glfw INTERFACE_INCLUDE_DIRECTORIES)
    set(GLFW_FOUND TRUE)
    message(STATUS "Found GLFW via CMake config (glfw3)")
endif()

if(NOT GLFW_FOUND)
    message(FATAL_ERROR "GLFW not found. Please install glfw3.")
endif()

# Find OpenGL
find_package(OpenGL REQUIRED)

# Find Tesseract and Leptonica
pkg_check_modules(TESSERACT QUIET tesseract)
if(NOT TESSERACT_FOUND)
    message(WARNING "Tesseract not found via pkg-config. Trying manual search...")
    find_path(TESSERACT_INCLUDE_DIR
        NAMES tesseract/baseapi.h
        PATHS
            /usr/include
            /usr/local/include
            /opt/local/include
            /opt/homebrew/include
    )
    find_library(TESSERACT_LIBRARY
        NAMES tesseract
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/local/lib
            /opt/homebrew/lib
    )
    if(TESSERACT_INCLUDE_DIR AND TESSERACT_LIBRARY)
        set(TESSERACT_INCLUDE_DIRS ${TESSERACT_INCLUDE_DIR})
        set(TESSERACT_LIBRARIES ${TESSERACT_LIBRARY})
        set(TESSERACT_FOUND TRUE)
    endif()
endif()

if(NOT TESSERACT_FOUND)
    message(FATAL_ERROR "Tesseract OCR library not found. Install tesseract-dev/tesseract-devel package.")
endif()

pkg_check_modules(LEPTONICA QUIET lept)
if(NOT LEPTONICA_FOUND)
    message(WARNING "Leptonica not found via pkg-config. Trying manual search...")
    find_path(LEPTONICA_INCLUDE_DIR
        NAMES leptonica/allheaders.h
        PATHS
            /usr/include
            /usr/local/include
            /opt/local/include
            /opt/homebrew/include
    )
    find_library(LEPTONICA_LIBRARY
        NAMES lept
        PATHS
            /usr/lib
            /usr/local/lib
            /opt/local/lib
            /opt/homebrew/lib
    )
    if(LEPTONICA_INCLUDE_DIR AND LEPTONICA_LIBRARY)
        set(LEPTONICA_INCLUDE_DIRS ${LEPTONICA_INCLUDE_DIR})
        set(LEPTONICA_LIBRARIES ${LEPTONICA_LIBRARY})
        set(LEPTONICA_FOUND TRUE)
    endif()
endif()

if(NOT LEPTONICA_FOUND)
    message(FATAL_ERROR "Leptonica library not found. Install leptonica-dev/leptonica-devel package.")
endif()

# System-specific dependencies
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(X11 REQUIRED x11)
elseif (APPLE)
    find_library(COCOA_LIB Cocoa)
    find_library(IOKIT_LIB IOKit)
    find_library(COREVIDEO_LIB CoreVideo)
    find_library(OPENGL_LIB OpenGL)

    if(NOT COCOA_LIB OR NOT IOKIT_LIB OR NOT COREVIDEO_LIB OR NOT OPENGL_LIB)
        message(FATAL_ERROR "Required macOS frameworks not found")
    endif()

    # On macOS, GLFW might be linked differently
    if(NOT GLFW_FOUND)
        find_library(GLFW_LIBRARY
            NAMES glfw glfw3
            PATHS
                /usr/local/lib
                /opt/local/lib
                /opt/homebrew/lib
        )
        if(GLFW_LIBRARY)
            set(GLFW_LIBRARIES ${GLFW_LIBRARY})
            set(GLFW_FOUND TRUE)
        endif()
    endif()
endif()

# Main executable
file(GLOB SRC "src/*.cpp")
add_executable(${PROJECT_NAME} ${SRC})
enable_lto(${PROJECT_NAME})

target_compile_definitions(${PROJECT_NAME} PRIVATE VERSION="${PROJECT_VERSION}")

target_include_directories(${PROJECT_NAME} PUBLIC
    ${GLFW_INCLUDE_DIRS}
    ${TESSERACT_INCLUDE_DIRS}
    ${LEPTONICA_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PUBLIC
    OpenGL::GL
    ${GLFW_LIBRARIES}
    ${TESSERACT_LIBRARIES}
    ${LEPTONICA_LIBRARIES}
)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_include_directories(${PROJECT_NAME} PUBLIC ${X11_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PUBLIC ${X11_LIBRARIES})
elseif (APPLE)
    target_link_libraries(${PROJECT_NAME} PUBLIC
        ${COCOA_LIB}
        ${IOKIT_LIB}
        ${COREVIDEO_LIB}
        ${OPENGL_LIB}
        ${GLFW_LIBRARIES}
    )
endif()

# fmt library
add_library(fmt STATIC
    "src/libs/fmt/os.cc"
    "src/libs/fmt/format.cc"
)
set_target_properties(fmt PROPERTIES POSITION_INDEPENDENT_CODE ON)
enable_lto(fmt)
target_compile_options(fmt PRIVATE -std=c++20)
target_include_directories(fmt PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libs/fmt
)
target_link_libraries(${PROJECT_NAME} PUBLIC fmt)

# tiny-process-library
add_library(tiny-process-library STATIC
    "src/libs/tiny-process-library/process.cpp"
)

if(MSVC)
    target_compile_definitions(tiny-process-library PRIVATE /D_CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(tiny-process-library PRIVATE -std=c++11 -Wall -Wextra)
endif()

if(WIN32)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
    target_sources(tiny-process-library PRIVATE "src/libs/tiny-process-library/process_win.cpp")
else()
    target_sources(tiny-process-library PRIVATE "src/libs/tiny-process-library/process_unix.cpp")
endif()

set_target_properties(tiny-process-library PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

find_package(Threads REQUIRED)
target_link_libraries(tiny-process-library PRIVATE Threads::Threads)
target_include_directories(tiny-process-library PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libs/tiny-process-library
)
enable_lto(tiny-process-library)
target_link_libraries(${PROJECT_NAME} PUBLIC tiny-process-library)

# imgui library
file(GLOB IMGUI_SOURCES
    "src/libs/imgui/*.cpp"
)

add_library(imgui STATIC ${IMGUI_SOURCES})
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
enable_lto(imgui)
target_compile_definitions(imgui PRIVATE IMGUI_IMPL_OPENGL_LOADER_GLAD)
target_link_libraries(${PROJECT_NAME} PUBLIC imgui)

# toml++ library
add_library(toml STATIC "src/libs/toml++/toml.cpp")
set_target_properties(toml PROPERTIES POSITION_INDEPENDENT_CODE ON)
enable_lto(toml)
target_compile_options(toml PRIVATE -std=c++20)
target_include_directories(toml PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/libs/toml++
)
target_link_libraries(${PROJECT_NAME} PUBLIC toml++)

# Installation
install(TARGETS ${PROJECT_NAME}
        RUNTIME DESTINATION bin
        COMPONENT runtime)

install(FILES LICENSE
        DESTINATION share/licenses/${PROJECT_NAME}
        COMPONENT documentation)
