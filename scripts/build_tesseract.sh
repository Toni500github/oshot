#!/usr/bin/env bash
# Build Tesseract from source as a static library, for the macOS, AppImage and .deb packages.
# Homebrew's bottles need a newer macOS than the packages target and there's none for Intel, and
# Ubuntu 22.04's 4.1.1 is slower and makes the .deb depend on libtesseract4, which newer releases lack.
# Only libtesseract is built. On Linux a static Leptonica comes first, since the .deb would otherwise depend
# on Ubuntu 22.04's liblept5, which Debian 13 and Ubuntu 26.04 don't have. It reads PNG, TIFF and JPEG, the
# last two from static libraries built here: Tesseract's debug fonts are TIFFs (it logs errors on every OCR
# without them), and Leptonica's TIFF code is only built together with JPEG.
#
# usage: scripts/build_tesseract.sh [install-prefix] (default: /usr/local)
# Needs: cmake, pkg-config, and leptonica on macOS or zlib and libpng on Linux

set -euo pipefail

TESSERACT_VERSION="5.5.3"
TESSERACT_COMMIT="db0ec62f81b0737fbbe184d8fea40af5738f8eef" # what the tag pointed to when pinned
JPEG_VERSION="3.1.4.1"
JPEG_COMMIT="9217719d3a58633923b096af4c1d50d304768a64"
TIFF_VERSION="v4.7.2"
TIFF_COMMIT="d01a94be176f5f6a87f7ee1c0b32e65416aa2b4d"
LEPTONICA_VERSION="1.87.0"
LEPTONICA_COMMIT="13275a278eb55b5746e33f95fbf5a2c8f604b3ab"

prefix="${1:-/usr/local}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# Runs a command that writes to the prefix, with sudo when it isn't writable
as_prefix_owner() {
    if [ -w "$prefix" ] || { [ ! -e "$prefix" ] && [ -w "$(dirname "$prefix")" ]; }; then
        "$@"
    else
        sudo "$@"
    fi
}

install_build() {
    as_prefix_owner cmake --install "$1"
}

# usage: fetch <url> <tag> <commit> <dir>
fetch() {
    git clone --quiet --depth 1 --branch "$2" "$1" "$4"
    if [ "$(git -C "$4" rev-parse HEAD)" != "$3" ]; then
        echo "$1: tag $2 no longer points to $3, refusing to build it" >&2
        exit 1
    fi
}

if [ "$(uname -s)" = Linux ]; then
    fetch https://github.com/libjpeg-turbo/libjpeg-turbo.git "$JPEG_VERSION" "$JPEG_COMMIT" "$work/jpeg"
    cmake -S "$work/jpeg" -B "$work/jpeg-build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DENABLE_SHARED=OFF \
        -DENABLE_STATIC=ON \
        -DWITH_SIMD=OFF \
        -DWITH_TURBOJPEG=OFF
    cmake --build "$work/jpeg-build" --parallel "$(getconf _NPROCESSORS_ONLN)"
    install_build "$work/jpeg-build"

    fetch https://gitlab.com/libtiff/libtiff.git "$TIFF_VERSION" "$TIFF_COMMIT" "$work/tiff"
    cmake -S "$work/tiff" -B "$work/tiff-build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -Dtiff-tools=OFF \
        -Dtiff-tests=OFF \
        -Dtiff-contrib=OFF \
        -Dtiff-docs=OFF \
        -Dcxx=OFF \
        -Dzlib=ON \
        -Dlibdeflate=OFF \
        -Djpeg=OFF \
        -Dold-jpeg=OFF \
        -Djbig=OFF \
        -Dlerc=OFF \
        -Dlzma=OFF \
        -Dzstd=OFF \
        -Dwebp=OFF
    cmake --build "$work/tiff-build" --parallel "$(getconf _NPROCESSORS_ONLN)"
    install_build "$work/tiff-build"

    fetch https://github.com/DanBloomberg/leptonica.git "$LEPTONICA_VERSION" "$LEPTONICA_COMMIT" "$work/lept"
    cmake -S "$work/lept" -B "$work/lept-build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_PREFIX_PATH="$prefix" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_PROG=OFF \
        -DSW_BUILD=OFF \
        -DENABLE_ZLIB=ON \
        -DENABLE_PNG=ON \
        -DENABLE_GIF=OFF \
        -DENABLE_JPEG=ON \
        -DENABLE_TIFF=ON \
        -DENABLE_WEBP=OFF \
        -DENABLE_OPENJPEG=OFF
    cmake --build "$work/lept-build" --parallel "$(getconf _NPROCESSORS_ONLN)"
    install_build "$work/lept-build"
    # Its package config doesn't look up what a static build links, and Tesseract's configure
    # check fails on the missing targets with CMake 3.25+
    as_prefix_owner sed -i 's/^include(CMakeFindDependencyMacro)$/&\nfind_dependency(ZLIB)\nfind_dependency(PNG)\nfind_dependency(TIFF)\nfind_dependency(JPEG)/' \
        "$prefix/lib/cmake/leptonica/LeptonicaConfig.cmake"
fi

fetch https://github.com/tesseract-ocr/tesseract.git "$TESSERACT_VERSION" "$TESSERACT_COMMIT" "$work/src"

cmake -S "$work/src" -B "$work/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_PREFIX_PATH="$prefix" \
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

install_build "$work/build"
