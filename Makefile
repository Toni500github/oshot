# Thin Makefile wrapper around CMake.
#
# This file does NOT compile anything itself. All build logic (sources,
# per-platform deps, flags, library targets) lives in CMakeLists.txt. This
# exists purely so `make`/`make clean`/`make distclean`/etc. keep working
# for people and CI used to that interface, without maintaining a second,
# independently-drifting set of compile rules alongside CMakeLists.txt.
#
# GNU make only: on the BSDs run it as `gmake`.

CMAKE       ?= cmake
# /usr/local outside Linux, where the base system owns /usr
PREFIX      ?= $(if $(filter Linux,$(shell uname -s)),/usr,/usr/local)
# -j<N> from the command line, else the number of CPUs
JOBS        := $(or $(patsubst -j%,%,$(filter -j%,$(MAKEFLAGS))),$(shell nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4))

NAME       := oshot
OLDVERSION := 0.5.0-rc1
VERSION    := 0.5.0-rc2
TARGET     ?= $(NAME)

DEBUG           ?= 0
DISABLE_PLUGINS ?= 0

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
    -DDISABLE_PLUGINS=$(DISABLE_PLUGINS) \
    -DCMAKE_INSTALL_PREFIX=$(PREFIX)

.PHONY: all configure build clean distclean dist genver updatever install uninstall

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

# Honors DESTDIR for staged installs, e.g. `make install DESTDIR=pkgdir`
install: build
	$(CMAKE) --install $(BUILDDIR) --prefix $(PREFIX)

# Removes what the last `make install` installed
uninstall:
	@test -f $(BUILDDIR)/install_manifest.txt || { echo "Nothing to uninstall: $(BUILDDIR)/install_manifest.txt not found"; exit 1; }
	xargs rm -f < $(BUILDDIR)/install_manifest.txt

# -i.bak + rm instead of plain -i, which BSD and macOS sed don't take
updatever:
	sed -i.bak "s#$(OLDVERSION)#$(VERSION)#g" $(wildcard .github/workflows/*.yml scripts/*) flake.nix CMakeLists.txt compile_flags.txt
	rm -f $(addsuffix .bak,$(wildcard .github/workflows/*.yml scripts/*) flake.nix CMakeLists.txt compile_flags.txt)
