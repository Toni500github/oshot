#!/usr/bin/env bash
#
# Build oshot.app and pack it into a drag-to-install .dmg.
#
# Usage: create_app_bundle.sh <oshot_binary> [output.dmg] [lib_search_dir]
#
# Environment:
#   OSHOT_VERSION     version string for Info.plist (default below, kept in sync by `make updatever`)
#   OSHOT_MODELS_DIR  directory of *.traineddata to bundle into Contents/Resources/tessdata
#   OSHOT_EXTRA_BINS  space-separated extra executables to put in Contents/MacOS (e.g. oshotpm)

set -euo pipefail
set -x

BASEDIR="$(dirname "${BASH_SOURCE[0]}")"

check_file() {
    if [ ! -f "$1" ]; then
        echo "File doesn't exist: $1"
        exit 1
    fi
}

install_dylibbundler() {
    if command -v dylibbundler &>/dev/null; then
        return 0
    fi

    if command -v brew &>/dev/null; then
        brew install dylibbundler
    else
        echo "Please install dylibbundler (brew install dylibbundler)"
        exit 1
    fi
}

main() {
    if [ -z "${1:-}" ]; then
        echo "Usage: $0 <binary_file> [output_name] [lib_search_dir]"
        exit 1
    fi

    # Resolve inputs before changing directory
    BINARY="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
    LIB_SEARCH_DIR="$(cd "${3:-$(dirname "$1")}" && pwd)"
    OUTPUT_NAME="${2:-oshot-macos.dmg}"
    case "$OUTPUT_NAME" in
        /*) ;;
        *) OUTPUT_NAME="$(pwd)/$OUTPUT_NAME" ;;
    esac

    cd "$BASEDIR/.."

    install_dylibbundler

    check_file "$BINARY"
    check_file oshot.png

    VERSION="${OSHOT_VERSION:-0.5.0-rc2}"
    # CFBundle*Version must be numeric (no "-rc2")
    NUM_VERSION="${VERSION%%-*}"
    NUM_VERSION="${NUM_VERSION#v}"

    APP_NAME="oshot"
    APP_DIR="${APP_NAME}.app"
    CONTENTS="${APP_DIR}/Contents"
    MACOS="${CONTENTS}/MacOS"
    FRAMEWORKS="${CONTENTS}/Frameworks"
    RESOURCES="${CONTENTS}/Resources"

    # Clean previous build
    rm -rf "$APP_DIR"
    mkdir -p "$MACOS" "$FRAMEWORKS" "$RESOURCES"

    # Copy binaries
    cp -f "$BINARY" "$MACOS/${APP_NAME}"
    chmod +x "$MACOS/${APP_NAME}"
    BUNDLER_ARGS=(-x "${MACOS}/${APP_NAME}")
    for extra in ${OSHOT_EXTRA_BINS:-}; do
        check_file "$extra"
        cp -f "$extra" "$MACOS/"
        chmod +x "$MACOS/$(basename "$extra")"
        BUNDLER_ARGS+=(-x "${MACOS}/$(basename "$extra")")
    done

    # Bundle all dylib dependencies and rewrite their load paths
    dylibbundler -od -b \
        "${BUNDLER_ARGS[@]}" \
        -d "${FRAMEWORKS}/" \
        -p @executable_path/../Frameworks/ \
        -s "$LIB_SEARCH_DIR"

    # OCR models: copied into ~/.config/oshot/models on first launch
    if [ -n "${OSHOT_MODELS_DIR:-}" ]; then
        mkdir -p "${RESOURCES}/tessdata"
        cp -f "${OSHOT_MODELS_DIR}"/*.traineddata "${RESOURCES}/tessdata/"
    fi

    # Convert oshot.png -> oshot.icns for the Dock/Finder icon
    ICON_KEY=""
    if command -v sips &>/dev/null && command -v iconutil &>/dev/null; then
        TMP_ICON_DIR=$(mktemp -d)
        ICONSET="${TMP_ICON_DIR}/oshot.iconset"
        mkdir -p "$ICONSET"
        for SIZE in 16 32 64 128 256 512; do
            sips -z $SIZE $SIZE oshot.png --out "${ICONSET}/icon_${SIZE}x${SIZE}.png" &>/dev/null
            sips -z $((SIZE*2)) $((SIZE*2)) oshot.png --out "${ICONSET}/icon_${SIZE}x${SIZE}@2x.png" &>/dev/null
        done
        iconutil -c icns "$ICONSET" -o "${RESOURCES}/oshot.icns"
        ICON_KEY="<key>CFBundleIconFile</key><string>oshot</string>"
        rm -rf "$TMP_ICON_DIR"
    fi

    # Write Info.plist
    cat > "${CONTENTS}/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>${APP_NAME}</string>
  <key>CFBundleIdentifier</key>
  <string>com.toni.oshot</string>
  <key>CFBundleName</key>
  <string>oshot</string>
  <key>CFBundleDisplayName</key>
  <string>oshot</string>
  <key>CFBundleVersion</key>
  <string>${NUM_VERSION}</string>
  <key>CFBundleShortVersionString</key>
  <string>${NUM_VERSION}</string>
  <key>CFBundleGetInfoString</key>
  <string>oshot ${VERSION}</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleSignature</key>
  <string>????</string>
  <key>LSApplicationCategoryType</key>
  <string>public.app-category.utilities</string>
  <key>NSHighResolutionCapable</key>
  <true/>
  <key>LSUIElement</key>
  <true/>
  <key>NSScreenCaptureUsageDescription</key>
  <string>oshot needs screen recording permission to take screenshots.</string>
  ${ICON_KEY}
</dict>
</plist>
EOF

    # Ad-hoc code signature. Required to run at all on Apple Silicon; a Developer ID
    # signature + notarization is needed to open without the Gatekeeper prompt.
    codesign --force --deep --sign - "$APP_DIR"

    # Stage the .dmg with an /Applications shortcut so users can drag-and-drop install
    DMG_STAGE=$(mktemp -d)
    cp -R "$APP_DIR" "$DMG_STAGE/"
    ln -s /Applications "$DMG_STAGE/Applications"

    hdiutil create \
        -volname "oshot ${VERSION}" \
        -srcfolder "$DMG_STAGE" \
        -ov \
        -format UDZO \
        "$OUTPUT_NAME"
    rm -rf "$DMG_STAGE"

    echo "App bundle created: ${APP_DIR}"
    echo "Disk image created: ${OUTPUT_NAME}"
}

main "$@"
