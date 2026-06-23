# Shared build logic for src/libs/*/Makefile.
#
# Each lib's Makefile sets a handful of variables and then does:
#   include ../../../mk/common.mk
#
# Required variables (set by the including Makefile):
#   LIBKIND   - object | static | shared
#   SRC       - source files, e.g. SRC = toml.cpp, or SRC := $(wildcard *.cpp)
#
# Variables required for LIBKIND = static or shared, ignored for object:
#   LIBNAME   - output name without the shared extension, e.g. liboshot_imgui
#               (the .a/.so/.dylib/.dll suffix is added here)
#
# Variables required for LIBKIND = object only:
#   TARGET    - single output object file, e.g. toml.o
#
# Optional variables:
#   CXX, CC, CXXSTD     - default to g++/cc/c++20
#   CXXFLAGS, CFLAGS    - extra per-lib flags, appended after the common ones
#   EXTRA_LIBS          - extra -L/-l flags for the link step (LIBKIND = shared only)
#   LTO_FLAGS, DEBUG_CXXFLAGS, DEBUG, BUILDDIR - passed down from the top-level Makefile

CXX     ?= g++
CC      ?= cc
CXXSTD  ?= c++20

# Override VISIBILITY := default in the including Makefile for libs that need
# to export symbols (toml++, cimgui currently do; most static/internal libs don't).
VISIBILITY ?= hidden

# No default -I path here: every lib states its own include dirs explicitly
# before the include line (some use the bare include/libs/ dir, some use
# their own include/libs/<name>/ subdir - it varies per lib, so it's not
# safe to assume one or the other generically).
CSTD ?= c11

CXXFLAGS += -std=$(CXXSTD) $(LTO_FLAGS) -fvisibility=$(VISIBILITY) -fvisibility-inlines-hidden -fPIC
CFLAGS   += -std=$(CSTD) $(LTO_FLAGS) -fvisibility=$(VISIBILITY) -fPIC

ifeq ($(DEBUG),1)
    CXXFLAGS := -ggdb3 -Wall -Wextra -pedantic -Wno-unused-parameter \
                -DDEBUG=1 -fno-omit-frame-pointer $(DEBUG_CXXFLAGS) $(CXXFLAGS)
else
    CXXFLAGS += -O3 -DNDEBUG
endif

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    SHAREDEXT   = dylib
    SHAREDFLAGS = -dynamiclib -install_name @rpath/$(LIBNAME).$(SHAREDEXT)
else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)
    SHAREDEXT   = dll
    SHAREDFLAGS = -shared
    CXXFLAGS   += -D_CRT_SECURE_NO_WARNINGS
else
    CXXFLAGS   += -Wall -Wextra
    SHAREDEXT   = so
    SHAREDFLAGS = -shared -Wl,-soname,$(LIBNAME).$(SHAREDEXT)
endif

OBJ := $(addsuffix .o,$(basename $(SRC)))

# Output name for static/shared kinds; unused for object (TARGET is used instead there).
OUT_static = $(LIBNAME).a
OUT_shared = $(LIBNAME).$(SHAREDEXT)
OUT        = $(OUT_$(LIBKIND))

all: $(if $(filter object,$(LIBKIND)),$(TARGET),$(OUT))

# --- object: compile a single source file straight to ../../../$(BUILDDIR)/$(TARGET) ---
# Picks CC/CFLAGS for a .c source, CXX/CXXFLAGS otherwise.
ifeq ($(LIBKIND),object)
ifeq ($(suffix $(SRC)),.c)
$(TARGET):
	$(CC) $(SRC) $(CFLAGS) -c -o ../../../$(BUILDDIR)/$@
else
$(TARGET):
	$(CXX) $(SRC) $(CXXFLAGS) -c -o ../../../$(BUILDDIR)/$@
endif
endif

# --- static: archive OBJ into $(LIBNAME).a ---
ifeq ($(LIBKIND),static)
$(OUT): $(OBJ)
	ar rcs $@ $^
	ranlib $@
	mv -f $@ ../../../$(BUILDDIR)/$@
endif

# --- shared: link OBJ into a platform shared library ---
ifeq ($(LIBKIND),shared)
$(OUT): $(OBJ)
	$(CXX) $(CXXFLAGS) $(SHAREDFLAGS) -o $@ $^ $(EXTRA_LIBS)
	mv -f $@ ../../../$(BUILDDIR)/$@
endif

%.o: %.cpp
	$(CXX) $(CXX_DEFINES) $(CXXFLAGS) -c -o $@ $<

%.o: %.cc
	$(CXX) $(CXX_DEFINES) $(CXXFLAGS) -c -o $@ $<

%.o: %.mm
	$(CXX) $(CXX_DEFINES) $(CXXFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.m
	$(CC) $(CFLAGS) -c -o $@ $<

# Where `clean` looks for the installed artifact. Mirrors exactly what the
# build rules above write to, so clean never points at a path `all` doesn't
# actually produce (the previous per-lib Makefiles had drifted on this).
CLEANTARGET_object = ../../../$(BUILDDIR)/$(TARGET)
CLEANTARGET_static  = ../../../$(BUILDDIR)/$(OUT)
CLEANTARGET_shared  = ../../../$(BUILDDIR)/$(OUT)

clean:
	rm -f $(OBJ) $(TARGET) $(OUT) $(CLEANTARGET_$(LIBKIND))

.PHONY: all clean
