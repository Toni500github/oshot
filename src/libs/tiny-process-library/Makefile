LIBKIND    := static
LIBNAME    := libtiny-process-library
VISIBILITY := default

CXXFLAGS += -I../../../include/libs/tiny-process-library

UNAME_S := $(shell uname -s)
ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)
    CXXFLAGS += -D_CRT_SECURE_NO_WARNINGS
    SRC      += process_win.cpp
    # Check if we're in MSYS environment
    ifneq (,$(findstring MSYS,$(shell uname -o)))
        CXXFLAGS += -DMSYS_PROCESS_USE_SH
    endif
else
    SRC += process_unix.cpp
endif
SRC += process.cpp

include ../../../mk/common.mk
