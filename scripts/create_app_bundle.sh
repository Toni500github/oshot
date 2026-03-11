#!/usr/bin/env bash

set -x

BASEDIR="$(dirname "${BASH_SOURCE[0]}")"

check_file() {
    if [ ! -f "$1" ]; then
        echo "File doesn't exist: $1"
        exit 1
    fi
}

install_dylibbundler() {
    if command -v dylibbundler &>/dev/null 2>&1; then
        return 0
    fi

    if command -v brew &>/dev/null 2>&1; then
        brew install dylibbundler
    else
        echo "Please install dylibbundler (brew install dylibbundler)"
        exit 1
    fi
}

main() {
    if [ -z "$1" ]; then
        echo "Usage: $0 <binary_file> [output_name]"
        exit 1
    fi

    cd "$BASEDIR/.."

    install_dylibbundler

    # The release binary
    check_file "$1"
    check_file oshot.png

    APP_NAME="oshot"
    APP_DIR="${APP_NAME}.app"
    CONTENTS="${APP_DIR}/Contents"
    MACOS="${CONTENTS}/MacOS"
    FRAMEWORKS="${CONTENTS}/Frameworks"

    # Clean previous build
    rm -rf "$APP_DIR"
    mkdir -p "$MACOS" "$FRAMEWORKS"

    # Copy binary
    cp -f "$1" "$MACOS/${APP_NAME}"
    chmod +x "$MACOS/${APP_NAME}"

    # Bundle all dylib dependencies and rewrite their load paths
    dylibbundler -od -b \
        -x "${MACOS}/${APP_NAME}" \
        -d "${FRAMEWORKS}/" \
        -p @executable_path/../Frameworks/

    # Convert oshot.png -> oshot.icns for the Dock icon
    if command -v sips &>/dev/null 2>&1 && command -v iconutil &>/dev/null 2>&1; then
        ICONSET=$(mktemp -d)/oshot.iconset
        mkdir -p "$ICONSET"
        for SIZE in 16 32 64 128 256 512; do
            sips -z $SIZE $SIZE oshot.png --out "${ICONSET}/icon_${SIZE}x${SIZE}.png" &>/dev/null
            sips -z $((SIZE*2)) $((SIZE*2)) oshot.png --out "${ICONSET}/icon_${SIZE}x${SIZE}@2x.png" &>/dev/null
        done
        iconutil -c icns "$ICONSET" -o "${CONTENTS}/Resources/oshot.icns"
        mkdir -p "${CONTENTS}/Resources"
        iconutil -c icns "$ICONSET" -o "${CONTENTS}/Resources/oshot.icns"
        ICON_KEY="<key>CFBundleIconFile</key><string>oshot</string>"
        rm -rf "$(dirname "$ICONSET")"
    else
        ICON_KEY=""
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
  <string>0.4.0</string>
  <key>CFBundleShortVersionString</key>
  <string>0.4.0</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleSignature</key>
  <string>????</string>
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

    # Use second argument as output name or default
    OUTPUT_NAME="${2:-oshot-macos.dmg}"

    # Remove quarantine flag so Gatekeeper doesn't block the app on first launch.
    # Without this macOS shows "damaged and can't be opened" for unsigned apps
    # downloaded via a browser.
    xattr -cr "$APP_DIR"

    # Ad-hoc code signature — satisfies Gatekeeper on the same architecture.
    # (A real Developer ID signature would be needed for notarization / other Macs)
    codesign --force --deep --sign - "$APP_DIR"

    # Pack into a .dmg disk image
    hdiutil create \
        -volname "oshot" \
        -srcfolder "$APP_DIR" \
        -ov \
        -format UDZO \
        "$OUTPUT_NAME"

    echo "App bundle created: ${APP_DIR}"
    echo "Disk image created: ${OUTPUT_NAME}"
}

main "$@"
