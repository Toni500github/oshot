#!/usr/bin/env bash
# Build SDL3 from source, as both a static and a shared library, for the
# packages built where no usable SDL3 is installed.
#
# Ubuntu 22.04 (the AppImage and .deb) and Debian 12 have no SDL3 package, and
# Homebrew's bottles are built for a newer macOS than MACOSX_DEPLOYMENT_TARGET.
# Builds without plugins link the static library. Plugin builds need the
# shared one, since SDL hides its internal symbols and libimgui, liboshot_common
# and oshot would each get their own copy of SDL's state. The packages bundle
# it (BUNDLE_SDL3 for the zip and .deb, linuxdeploy and dylibbundler for the
# AppImage and the .app).
# Only video, render, dialog and tray are built: no audio, camera, joystick,
# haptic, HIDAPI, sensor, power, GPU, Vulkan or KMSDRM (fullscreen without a
# display server), none of which oshot uses.
# X11, Wayland, libdecor and D-Bus are loaded at runtime, so the result runs on
# either session without depending on their libraries.
#
# usage: scripts/build_sdl3.sh [install-prefix] (default: /usr/local)
# Needs: cmake, pkg-config and, on Linux/BSD, the X11, Wayland, EGL and D-Bus
# headers (libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev
# libxfixes-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols
# libegl-dev libgl-dev libdbus-1-dev, libdecor-0-dev for Wayland decorations).

set -euo pipefail

SDL_VERSION="3.4.20"
SDL_COMMIT="f00dc919ca35b9756316e86c4777392de442f9b2" # what the tag pointed to when pinned

prefix="${1:-/usr/local}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

git clone --quiet --depth 1 --branch "release-$SDL_VERSION" https://github.com/libsdl-org/SDL.git "$work/src"
if [ "$(git -C "$work/src" rev-parse HEAD)" != "$SDL_COMMIT" ]; then
    echo "SDL tag release-$SDL_VERSION no longer points to $SDL_COMMIT, refusing to build it" >&2
    exit 1
fi

cmake -S "$work/src" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    ${MACOSX_DEPLOYMENT_TARGET:+-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"} \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=ON \
    -DSDL_AUDIO=OFF \
    -DSDL_CAMERA=OFF \
    -DSDL_JOYSTICK=OFF \
    -DSDL_HAPTIC=OFF \
    -DSDL_HIDAPI=OFF \
    -DSDL_SENSOR=OFF \
    -DSDL_POWER=OFF \
    -DSDL_GPU=OFF \
    -DSDL_VULKAN=OFF \
    -DSDL_KMSDRM=OFF \
    -DSDL_LIBURING=OFF \
    -DSDL_TEST_LIBRARY=OFF \
    -DSDL_TESTS=OFF \
    -DSDL_EXAMPLES=OFF \
    -DSDL_INSTALL_DOCS=OFF \
    -DSDL_CCACHE=OFF
cmake --build "$work/build" --parallel "$(getconf _NPROCESSORS_ONLN)"

if [ -w "$prefix" ] || { [ ! -e "$prefix" ] && [ -w "$(dirname "$prefix")" ]; }; then
    cmake --install "$work/build"
else
    sudo cmake --install "$work/build"
fi
