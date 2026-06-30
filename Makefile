# Thin Makefile wrapper around CMake.
#
# This file does NOT compile anything itself. All build logic (sources,
# per-platform deps, flags, library targets) lives in CMakeLists.txt. This
# exists purely so `make`/`make clean`/`make distclean`/etc. keep working
# for people and CI used to that interface, without maintaining a second,
# independently-drifting set of compile rules alongside CMakeLists.txt.

CMAKE       ?= cmake
PREFIX      ?= /usr
DEBUG       ?= 0
WINDOWS_CMD ?= 0
JOBS        := $(shell echo $(MAKEFLAGS) | grep -oP '(?<=-j)\d+')
JOBS        ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

NAME       := oshot
VERSION    := 0.4.6
OLDVERSION := 0.4.5
TARGET     ?= $(NAME)

ifeq ($(DEBUG),1)
    BUILDDIR   := build/debug
    BUILD_TYPE := Debug
else
    BUILDDIR   := build/release
    BUILD_TYPE := Release
endif

CMAKE_CONFIGURE_FLAGS := \
    -S . -B $(BUILDDIR) \
    -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
    -DWINDOWS_CMD=$(WINDOWS_CMD) \
    -DCMAKE_INSTALL_PREFIX=$(PREFIX)

.PHONY: all configure build clean distclean dist genver updatever install

all: build

# Re-configure whenever CMakeLists.txt changes or the build dir doesn't
# exist yet. CMake re-configures itself as needed beyond this too (e.g. if
# a referenced file changes), this just guarantees a fresh checkout works
# with a single `make`.
$(BUILDDIR)/CMakeCache.txt: CMakeLists.txt
	$(CMAKE) $(CMAKE_CONFIGURE_FLAGS)

configure: $(BUILDDIR)/CMakeCache.txt

build: configure
	$(CMAKE) --build $(BUILDDIR) --parallel $(JOBS)

# Generates version info ahead of time; CMakeLists.txt also runs this at
# configure time, so this target is mainly for manual/CI use outside a
# full configure+build cycle.
genver:
	./scripts/generateVersion.sh

# Removes built artifacts but keeps the CMake cache/config, so the next
# `make` is a fast incremental rebuild rather than a full re-configure.
clean:
	$(CMAKE) --build $(BUILDDIR) --target clean

distclean:
	rm -rf build
	find . -type f -name "*.tar.gz" -delete
	find src -type f \( -name "*.o" -o -name "*.a" -o -name "*.so" -o -name "*.dylib" \) -delete

dist: build
	zip -j $(NAME)-v$(VERSION).zip LICENSE README.md $(BUILDDIR)/$(TARGET)

install: build
	$(CMAKE) --install $(BUILDDIR) --prefix $(PREFIX)

updatever:
	sed -i "s#$(OLDVERSION)#$(VERSION)#g" $(wildcard .github/workflows/*.yml scripts/*) CMakeLists.txt compile_flags.txt
