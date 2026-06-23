# Variables taken from {fmt} cmake build files
# I don't wanna use cmake
# cmd: cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF

LIBKIND := static
LIBNAME := libfmt
SRC      = format.cc os.cc
# CXX_DEFINES = -DFMT_LIB_EXPORT -Dfmt_EXPORTS

CXXFLAGS += -I../../../include/libs

include ../../../mk/common.mk
