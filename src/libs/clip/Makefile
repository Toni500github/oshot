LIBKIND    := static
LIBNAME    := libclip
VISIBILITY := default

CXXFLAGS += -I../../../include/libs/clip -DCLIP_ENABLE_IMAGE=1

UNAME_S := $(shell uname -s)
ifeq ($(findstring NT,$(UNAME_S)),NT)
    CXXFLAGS += -D_CRT_SECURE_NO_WARNINGS
    SRC      += $(wildcard clip_win*.cpp)
    # Check if we're in MSYS environment
    ifneq (,$(findstring MSYS,$(shell uname -o)))
        CXXFLAGS += -DMSYS_PROCESS_USE_SH
    endif
else
    SRC += clip_none.cpp
endif
SRC += clip.cpp image.cpp

include ../../../mk/common.mk
