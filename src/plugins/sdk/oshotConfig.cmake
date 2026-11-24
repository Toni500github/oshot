# CMake package for oshot's plugin SDK:
#
#   find_package(oshot REQUIRED CONFIG HINTS "$ENV{OSHOT_SDK}")
#   oshot_add_plugin(my_plugin src/plugin.c)
#
# oshotpm passes the SDK folder to plugin builds as OSHOT_SDK. The package is
# also found in an oshot install prefix (e.g. /usr) and in the SDK of an oshot
# build tree (build/<type>/sdk).
#
# Targets:
#   oshot::plugin  The headers, oshot/oshot_plugin.h and oshot/oshot_mock_host.h.
#                  Plugins link nothing from oshot.
#   oshot::cimgui  oshot::plugin plus what a plugin that calls cimgui needs:
#                  nothing on Linux and the BSDs, -undefined dynamic_lookup on
#                  macOS, the libcimgui import library from the SDK on Windows.
#
# oshot_add_plugin(<target> [CIMGUI] [OUTPUT_DIRECTORY <dir>] <sources>...)
#   Adds a plugin library, a MODULE library linked with oshot::plugin (or
#   oshot::cimgui with CIMGUI) that exports only oshot_plugin_entry(), so build
#   it with OSHOT_PLUGIN_EXPORT on that function as the header says. With
#   OUTPUT_DIRECTORY, the library is written there for every generator and
#   configuration, which is what a manifest's output-dir needs.
#
# This file doesn't depend on where it's installed, since it finds the SDK's root by
# looking for include/oshot/oshot_plugin.h above itself.

if(CMAKE_VERSION VERSION_LESS 3.13)
    set(oshot_FOUND FALSE)
    set(oshot_NOT_FOUND_MESSAGE "oshot's plugin SDK needs CMake 3.13 or newer")
    return()
endif()

set(_oshot_root "${CMAKE_CURRENT_LIST_DIR}")
foreach(_oshot_level RANGE 4)
    get_filename_component(_oshot_root "${_oshot_root}" DIRECTORY)
    if(EXISTS "${_oshot_root}/include/oshot/oshot_plugin.h")
        break()
    endif()
endforeach()
if(NOT EXISTS "${_oshot_root}/include/oshot/oshot_plugin.h")
    set(oshot_FOUND FALSE)
    set(oshot_NOT_FOUND_MESSAGE
        "oshot's plugin SDK headers weren't found above ${CMAKE_CURRENT_LIST_DIR}"
    )
    return()
endif()

set(OSHOT_SDK_ROOT "${_oshot_root}")
set(OSHOT_INCLUDE_DIR "${_oshot_root}/include")
unset(_oshot_root)
unset(_oshot_level)

# OSHOT_API_VERSION and OSHOT_IMGUI_VERSION, as numbers
file(STRINGS "${OSHOT_INCLUDE_DIR}/oshot/oshot_plugin.h" _oshot_defines
     REGEX "^#define OSHOT_(API|IMGUI)_VERSION [0-9]+u?$"
)
foreach(_oshot_define IN LISTS _oshot_defines)
    if(_oshot_define MATCHES "^#define (OSHOT_[A-Z]+_VERSION) ([0-9]+)")
        set(${CMAKE_MATCH_1} ${CMAKE_MATCH_2})
    endif()
endforeach()
unset(_oshot_defines)
unset(_oshot_define)

if(NOT TARGET oshot::plugin)
    add_library(oshot::plugin INTERFACE IMPORTED)
    set_target_properties(
        oshot::plugin
        PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${OSHOT_INCLUDE_DIR}"
    )

    add_library(oshot::cimgui INTERFACE IMPORTED)
    set_property(
        TARGET oshot::cimgui
        PROPERTY INTERFACE_LINK_LIBRARIES oshot::plugin
    )
    if(WIN32)
        # A MinGW import library, so plugins that call cimgui are built with
        # MinGW-w64 like oshot
        find_library(
            OSHOT_CIMGUI_IMPORT_LIB
            NAMES cimgui libcimgui
            PATHS "${OSHOT_SDK_ROOT}/lib"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
        )
        if(OSHOT_CIMGUI_IMPORT_LIB)
            set_property(
                TARGET oshot::cimgui
                APPEND
                PROPERTY INTERFACE_LINK_LIBRARIES "${OSHOT_CIMGUI_IMPORT_LIB}"
            )
        endif()
    elseif(APPLE)
        # cimgui's symbols are resolved when oshot loads the plugin
        set_property(
            TARGET oshot::cimgui
            PROPERTY INTERFACE_LINK_OPTIONS "LINKER:-undefined,dynamic_lookup"
        )
    endif()
endif()

function(oshot_add_plugin target)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "CIMGUI" "OUTPUT_DIRECTORY" "")
    if(NOT _arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "oshot_add_plugin(${target}): no sources given")
    endif()

    add_library(${target} MODULE ${_arg_UNPARSED_ARGUMENTS})
    if(_arg_CIMGUI)
        if(WIN32 AND NOT OSHOT_CIMGUI_IMPORT_LIB)
            message(
                FATAL_ERROR
                "oshot_add_plugin(${target} CIMGUI): the SDK in ${OSHOT_SDK_ROOT} has no lib/libcimgui.dll.a"
            )
        endif()
        target_link_libraries(${target} PRIVATE oshot::cimgui)
    else()
        target_link_libraries(${target} PRIVATE oshot::plugin)
    endif()

    # Only oshot_plugin_entry() is exported
    set_target_properties(
        ${target}
        PROPERTIES
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
    )

    if(_arg_OUTPUT_DIRECTORY)
        set_target_properties(
            ${target}
            PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${_arg_OUTPUT_DIRECTORY}"
        )
        foreach(_config IN LISTS CMAKE_CONFIGURATION_TYPES)
            string(TOUPPER "${_config}" _config)
            set_target_properties(
                ${target}
                PROPERTIES
                    LIBRARY_OUTPUT_DIRECTORY_${_config}
                        "${_arg_OUTPUT_DIRECTORY}"
            )
        endforeach()
    endif()
endfunction()
