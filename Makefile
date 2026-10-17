# Wrapper around CMake, so `make`, `make install` and friends keep working.
# The build itself is defined in CMakeLists.txt.
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

# Configure on the first build and when CMakeLists.txt changes. CMake
# re-configures itself for anything else
$(BUILDDIR)/CMakeCache.txt: CMakeLists.txt
	$(CMAKE) $(CMAKE_CONFIGURE_FLAGS)

configure: $(BUILDDIR)/CMakeCache.txt

build: configure
	$(CMAKE) --build $(BUILDDIR) --parallel $(JOBS)

# Generates include/version.h, which the build also does before compiling
genver:
	./scripts/generateVersion.sh

# Keeps the CMake cache, so the next build doesn't re-configure
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
