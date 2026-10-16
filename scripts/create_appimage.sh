#!/usr/bin/env bash

set -x

BASEDIR="$(dirname "${BASH_SOURCE[0]}")"

check_file() {
    if [ ! -f "$1" ]; then
        echo "File doesn't exist: $1"
        exit 1
    fi
}

download() {
    local url="$1"
    local filename="$(basename "$url")"

    if [ -f "$filename" ]; then
        echo "File already exists: $filename"
        return 0
    fi

    if [ "$DOWNLOAD" = "wget" ]; then
        wget "$url" -O "$filename"
    elif [ "$DOWNLOAD" = "curl" ]; then
        curl -L "$url" -o "$filename"
    else
        echo "Download tool not configured"
        exit 1
    fi
}

main() {
    if [ -z "$1" ]; then
        echo "Usage: $0 <binary_file> [output_name]"
        exit 1
    fi

    cd $BASEDIR/..

    if command -v wget &>/dev/null 2>&1; then
        DOWNLOAD="wget"
    elif command -v curl &>/dev/null 2>&1; then
        DOWNLOAD="curl"
    else
        echo "Please install either wget or curl"
        exit 1
    fi

    ARCH="$(uname -m)"
    case "$ARCH" in
        amd64) ARCH=x86_64 ;;
        arm64) ARCH=aarch64 ;;
    esac

    download "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
    # AppImageKit is archived, appimagetool lives in its own repo now
    download "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${ARCH}.AppImage"
    chmod +x "linuxdeploy-${ARCH}.AppImage" "appimagetool-${ARCH}.AppImage"

    # The release binary
    check_file "$1"
    BUILD_DIR="$(dirname "$1")"

    rm -rf AppDir
    mkdir -p AppDir/usr/bin/
    check_file assets/linux/oshot.svg
    check_file assets/linux/oshot-256.png
    check_file assets/linux/oshot.desktop
    check_file assets/linux/io.github.toni500github.oshot.metainfo.xml

    # A plugin build links liboshot_common: ship the plugin manager and libcimgui
    # (which C plugins link against) inside the AppImage too
    DEPLOY_ARGS=()
    if readelf -d "$1" | grep -q 'liboshot_common'; then
        check_file "$BUILD_DIR/oshotpm"
        check_file "$BUILD_DIR/libcimgui.so.1"
        cp -f "$BUILD_DIR/oshotpm" AppDir/usr/bin/
        DEPLOY_ARGS+=(--executable AppDir/usr/bin/oshotpm --library "$BUILD_DIR/libcimgui.so.1")
    fi

    # "<AppImage> oshotpm ..." or a symlink named oshotpm runs the plugin manager.
    # oshotpm finds the bundled libs through its RUNPATH, so the build steps it runs
    # (git, bash, compilers) keep the host's LD_LIBRARY_PATH
    cat > AppDir/AppRun << 'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
if [ -x "$HERE/usr/bin/oshotpm" ]; then
    if [ "$(basename "${ARGV0:-$0}")" = oshotpm ]; then
        exec "$HERE/usr/bin/oshotpm" "$@"
    elif [ "$1" = oshotpm ]; then
        shift
        exec "$HERE/usr/bin/oshotpm" "$@"
    fi
fi
export APPIMAGE_ORIG_LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="$HERE/usr/lib:$LD_LIBRARY_PATH"
exec "$HERE/usr/bin/oshot" "$@"
EOF
    chmod +x AppDir/AppRun

    cp -f "$1" AppDir/usr/bin/

    # The SVG for desktops that scale icons, a 256px PNG for the AppImage's own icon
    install -Dm644 assets/linux/oshot.svg AppDir/usr/share/icons/hicolor/scalable/apps/oshot.svg
    install -Dm644 assets/linux/oshot-256.png AppDir/usr/share/icons/hicolor/256x256/apps/oshot.png

    # AppStream metadata, read by software centers and the AppImage catalog.
    # appimagetool only checks <desktop file name>.appdata.xml and warns that it's
    # missing, but renaming it would no longer match the component id
    install -Dm644 assets/linux/io.github.toni500github.oshot.metainfo.xml -t AppDir/usr/share/metainfo/

    ./linuxdeploy-${ARCH}.AppImage \
        --appdir AppDir \
        --executable AppDir/usr/bin/oshot \
        "${DEPLOY_ARGS[@]}" \
        --desktop-file assets/linux/oshot.desktop

    # linuxdeploy prefers the SVG for the top-level icon, but .DirIcon (which
    # appimagetool links to it) should be a PNG
    rm -f AppDir/oshot.svg AppDir/oshot.png
    ln -s usr/share/icons/hicolor/256x256/apps/oshot.png AppDir/oshot.png

    # With UPDATE_INFORMATION set (e.g. "gh-releases-zsync|user|repo|latest|app-*.AppImage.zsync"),
    # embed it and write <output>.zsync next to the AppImage, for AppImageUpdate and similar tools
    TOOL_ARGS=()
    if [ -n "$UPDATE_INFORMATION" ]; then
        TOOL_ARGS+=(-u "$UPDATE_INFORMATION")
    fi

    # Use second argument as output name or default
    OUTPUT_NAME="${2:-oshot-${ARCH}.AppImage}"
    ARCH="$ARCH" ./appimagetool-${ARCH}.AppImage "${TOOL_ARGS[@]}" AppDir "$OUTPUT_NAME"

    # appimagetool writes the .zsync into the current directory. It refers to the
    # AppImage by this name, so neither can be renamed afterwards
    ZSYNC="$(basename "$OUTPUT_NAME").zsync"
    if [ -f "$ZSYNC" ] && [ "$(dirname "$OUTPUT_NAME")" != . ]; then
        mv -f "$ZSYNC" "$(dirname "$OUTPUT_NAME")/"
    fi

    echo "AppImage created: $OUTPUT_NAME"
}

main "$@"
