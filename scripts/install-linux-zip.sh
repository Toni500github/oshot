#!/bin/sh
# Installs the generic Linux zip into PREFIX (default /usr/local).
# Shipped as oshot/install.sh in oshot-linux-<version>.zip, usually run with sudo.
#   ./install.sh               install
#   ./install.sh --uninstall   remove the files this zip installed

set -e

PREFIX="${PREFIX:-/usr/local}"
cd "$(dirname "$0")"

# Best effort: menus and icon themes pick up the changes without a re-login
refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database -q "$PREFIX/share/applications" 2>/dev/null || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1; then
        gtk-update-icon-cache -q -t -f "$PREFIX/share/icons/hicolor" 2>/dev/null || true
    fi
}

if [ "$1" = "--uninstall" ]; then
    for dir in bin lib include share; do
        [ -d "$dir" ] || continue
        find "$dir" ! -type d | while IFS= read -r f; do
            rm -f "$PREFIX/$f"
        done
    done
    rmdir "$PREFIX/include/oshot" "$PREFIX/share/licenses/oshot" 2>/dev/null || true
    refresh_caches
    echo "oshot removed from $PREFIX"
    exit 0
fi

for dir in bin lib include share; do
    [ -d "$dir" ] || continue
    mkdir -p "$PREFIX/$dir"
    cp -a "$dir/." "$PREFIX/$dir/"
done
refresh_caches

echo "oshot installed into $PREFIX"
