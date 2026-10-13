#!/bin/sh
# Installs the generic Linux zip into PREFIX (default /usr/local).
# Shipped as oshot/install.sh in oshot-linux-<version>.zip, usually run with sudo.

set -e

PREFIX="${PREFIX:-/usr/local}"
cd "$(dirname "$0")"

for dir in bin lib include share; do
    [ -d "$dir" ] || continue
    mkdir -p "$PREFIX/$dir"
    cp -a "$dir/." "$PREFIX/$dir/"
done

echo "oshot installed into $PREFIX"
