#!/bin/env bash

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

    download "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    download "https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage"
    chmod +x *.AppImage

    # The release binary
    check_file "$1"

    mkdir -p AppDir/usr/bin/
    check_file oshot.png
    check_file oshot.desktop

    cat > AppDir/AppRun << 'EOF'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:$LD_LIBRARY_PATH"
exec "$HERE/usr/bin/oshot" "$@"
EOF
    chmod +x AppDir/AppRun

    cp -f "$1" AppDir/usr/bin/
    cp -f oshot.png AppDir/
    cp -f oshot.desktop AppDir/

    ./linuxdeploy-x86_64.AppImage \
        --appdir AppDir \
        --executable AppDir/usr/bin/oshot \
        --desktop-file AppDir/oshot.desktop \
        --icon-file AppDir/oshot.png

    # Use second argument as output name or default
    OUTPUT_NAME="${2:-oshot-x86_64.AppImage}"
    ./appimagetool-x86_64.AppImage AppDir "$OUTPUT_NAME"

    echo "AppImage created: $OUTPUT_NAME"
}

main "$@"
