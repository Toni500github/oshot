#!/bin/sh
# Writes the Flathub manifest for a release tag: the in-repo one, with the
# local checkout source replaced by that tag, plus the patches it needs.
# Copy the output into the flathub/io.github.toni500github.oshot repo.
#   ./scripts/flathub_manifest.sh v0.5.0 [output dir, default build/flathub]

set -e

TAG="$1"
OUT="${2:-build/flathub}"
APP_ID="io.github.toni500github.oshot"

if [ -z "$TAG" ]; then
    echo "Usage: $0 <tag> [output dir]" >&2
    exit 1
fi

cd "$(dirname "$0")/.."

COMMIT="$(git rev-parse --verify "$TAG^{commit}")"
VERSION="${TAG#v}"
# AppStream versions use ~ for pre-releases (0.5.0-rc2 -> 0.5.0~rc2)
APPSTREAM_VERSION="$(echo "$VERSION" | sed 's/-/~/')"

# Flathub builds the metainfo of the tag, so it must already list this release
if ! git show "$TAG:assets/linux/$APP_ID.metainfo.xml" | grep -q "<release version=\"$APPSTREAM_VERSION\""; then
    echo "The metainfo at $TAG has no <release version=\"$APPSTREAM_VERSION\"> entry" >&2
    exit 1
fi

mkdir -p "$OUT"
cp -R flatpak/patches "$OUT/"

# x-checker-data lets Flathub's bot open a PR for each new stable vX.Y.Z tag
awk -v tag="$TAG" -v commit="$COMMIT" '
    /# flathub-source-begin/ {
        print "      - type: git"
        print "        url: https://github.com/Toni500github/oshot.git"
        print "        tag: " tag
        print "        commit: " commit
        print "        x-checker-data:"
        print "          type: git"
        print "          tag-pattern: ^v([\\d.]+)$"
        skip = 1
        next
    }
    /# flathub-source-end/ { skip = 0; next }
    !skip
' "flatpak/$APP_ID.yml" > "$OUT/$APP_ID.yml"

echo "Flathub manifest for $TAG ($COMMIT) written to $OUT"
