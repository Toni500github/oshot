LIBKIND   := static
LIBNAME   := libtray
PKG_FLAGS := `pkg-config --cflags appindicator3-0.1`

CXXFLAGS += $(PKG_FLAGS) -I../../../include/libs

ifeq ($(shell uname -s),Darwin)
    SRC := tray_darwin.mm
else
    SRC := $(wildcard ./*.cpp)
endif

include ../../../mk/common.mk
