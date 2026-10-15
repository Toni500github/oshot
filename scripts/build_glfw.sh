#!/usr/bin/env bash
# Build GLFW from source as a static library with both X11 and Wayland support.
#
# Ubuntu 22.04, which the AppImage and .deb are built on, only has GLFW 3.3
# built for X11, so oshot would need XWayland on a Wayland session. GLFW 3.4+
# picks X11 or Wayland at runtime and loads their libraries itself, so linking
# it statically makes oshot run natively on both without new dependencies.
#
# usage: scripts/build_glfw.sh [install-prefix] (default: /usr/local)
# Needs: cmake, pkg-config, libwayland-dev, libxkbcommon-dev, wayland-protocols
# and the X11 headers (libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev).

set -euo pipefail

GLFW_VERSION="3.5.1"
GLFW_COMMIT="d9d6f0f1f967807ffade6598ea9a631ebaf37a56" # what the tag pointed to when pinned

prefix="${1:-/usr/local}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

git clone --quiet --depth 1 --branch "$GLFW_VERSION" https://github.com/glfw/glfw.git "$work/src"
if [ "$(git -C "$work/src" rev-parse HEAD)" != "$GLFW_COMMIT" ]; then
    echo "GLFW tag $GLFW_VERSION no longer points to $GLFW_COMMIT, refusing to build it" >&2
    exit 1
fi

cmake -S "$work/src" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF \
    -DGLFW_BUILD_X11=ON \
    -DGLFW_BUILD_WAYLAND=ON \
    -DGLFW_BUILD_EXAMPLES=OFF \
    -DGLFW_BUILD_TESTS=OFF \
    -DGLFW_BUILD_DOCS=OFF
cmake --build "$work/build" --parallel "$(nproc)"

if [ -w "$prefix" ] || { [ ! -e "$prefix" ] && [ -w "$(dirname "$prefix")" ]; }; then
    cmake --install "$work/build"
else
    sudo cmake --install "$work/build"
fi
