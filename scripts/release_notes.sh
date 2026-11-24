#!/usr/bin/env bash
#
# Print the GitHub release notes for a tag: which file to download, then the
# changes from the matching debian/changelog entry.
#
# Usage: release_notes.sh <tag>      e.g. release_notes.sh v0.5.0-rc2

set -euo pipefail

TAG="${1:?usage: $0 <tag>}"
cd "$(dirname "${BASH_SOURCE[0]}")/.."

# v0.5.0-rc2 -> 0.5.0~rc2, how pre-releases are versioned in debian/changelog
DEB_VERSION="${TAG#v}"
DEB_VERSION="${DEB_VERSION//-/\~}"

cat << EOF
## Which file do I need?

| System | File |
| ------ | ---- |
| Windows (Intel/AMD) | \`oshot-windows-x64-setup-${TAG}.exe\` (installer) or \`oshot-windows-x64-${TAG}.zip\` (portable) |
| Windows on ARM | \`oshot-windows-arm64-setup-${TAG}.exe\` (installer) or \`oshot-windows-arm64-${TAG}.zip\` (portable) |
| macOS, Apple Silicon | \`oshot-macos-arm64-${TAG}.dmg\` |
| macOS, Intel | \`oshot-macos-x86_64-${TAG}.dmg\` |
| Linux (Intel/AMD) | \`oshot-x86_64-${TAG}.AppImage\`, or \`oshot-debian-package-amd64-${TAG}.deb\` on Debian/Ubuntu |
| Linux on ARM64 (e.g. Raspberry Pi OS 64-bit) | \`oshot-aarch64-${TAG}.AppImage\`, or \`oshot-debian-package-arm64-${TAG}.deb\` on Debian/Ubuntu |

Files ending in \`-plugins\` add plugin support and the \`oshotpm\` plugin manager. If you don't use plugins, you don't need them.

See the [installation instructions](https://github.com/Toni500github/oshot#installation) for each system.

## Changes

EOF

# Bullets of the "oshot (<version>-<revision>)" entry, up to its " -- " trailer
CHANGES="$(awk -v ver="$DEB_VERSION" '
    /^oshot \(/ {
        v = $2
        gsub(/[()]/, "", v)
        sub(/-[0-9]+$/, "", v)
        in_entry = (v == ver)
        next
    }
    in_entry && /^ -- / { exit }
    in_entry && /^  \* / { sub(/^  \* /, "- "); print }
' debian/changelog)"

if [ -n "$CHANGES" ]; then
    echo "$CHANGES"
else
    echo "::warning::No debian/changelog entry for ${DEB_VERSION}" >&2
    echo "See the commit history for the changes in this release."
fi
