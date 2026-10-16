#!/usr/bin/env bash
# Build Tesseract from source as a static library, for the macOS packages.
#
# Homebrew's tesseract bottles are built for a newer macOS than the one the
# packages target (the arm64_sequoia bottle needs macOS 15.7.5), and there is
# no Intel macOS bottle at all. Built here, it follows MACOSX_DEPLOYMENT_TARGET
# like oshot itself, and linking it statically leaves one less dylib to bundle.
# Only libtesseract is built: no training tools, ScrollView, TIFF, libarchive or
# curl, none of which oshot uses.
#
# usage: scripts/build_tesseract.sh [install-prefix] (default: /usr/local)
# Needs: cmake, pkg-config and leptonica

set -euo pipefail

TESSERACT_VERSION="5.5.3"
TESSERACT_COMMIT="db0ec62f81b0737fbbe184d8fea40af5738f8eef" # what the tag pointed to when pinned

prefix="${1:-/usr/local}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

git clone --quiet --depth 1 --branch "$TESSERACT_VERSION" https://github.com/tesseract-ocr/tesseract.git "$work/src"
if [ "$(git -C "$work/src" rev-parse HEAD)" != "$TESSERACT_COMMIT" ]; then
    echo "Tesseract tag $TESSERACT_VERSION no longer points to $TESSERACT_COMMIT, refusing to build it" >&2
    exit 1
fi

cmake -S "$work/src" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    ${MACOSX_DEPLOYMENT_TARGET:+-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET"} \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TRAINING_TOOLS=OFF \
    -DBUILD_TESTS=OFF \
    -DGRAPHICS_DISABLED=ON \
    -DDISABLE_TIFF=ON \
    -DDISABLE_ARCHIVE=ON \
    -DDISABLE_CURL=ON \
    -DINSTALL_CONFIGS=OFF \
    -DENABLE_CCACHE=OFF
cmake --build "$work/build" --parallel "$(getconf _NPROCESSORS_ONLN)"

if [ -w "$prefix" ] || { [ ! -e "$prefix" ] && [ -w "$(dirname "$prefix")" ]; }; then
    cmake --install "$work/build"
else
    sudo cmake --install "$work/build"
fi
