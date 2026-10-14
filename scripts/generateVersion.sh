#!/bin/sh
set -e

OUT="./include/version.h"
TMP="$(mktemp)"

# Fallback values if git is missing or not a git repository
# rev-parse rather than testing ./.git, which is a file in worktrees and submodules
IS_GIT_REPO=true
if ! command -v git >/dev/null 2>&1 || ! git rev-parse --git-dir >/dev/null 2>&1; then
    IS_GIT_REPO=false
fi

if [ "$IS_GIT_REPO" = false ]; then
    HASH=${HASH:-"unknown"}
    BRANCH=${BRANCH:-"unknown"}
    MESSAGE=${MESSAGE:-"standalone release"}
    DATE=${DATE:-"unknown"}
    DIRTY=${DIRTY:-"clean"}
    TAG=${TAG:-"v0.5.0"}
    COMMITS=${COMMITS:-"0"}
else
    HASH=${HASH-$(git rev-parse HEAD)}
    BRANCH=${BRANCH-$(git branch --show-current)}
    MESSAGE=${MESSAGE-$(git log -1 --pretty=%B | head -n1 | sed -e 's/#//g' -e 's/\"//g')}
    DATE=${DATE-$(git show --no-patch --format=%cd --date=local)}
    DIRTY=${DIRTY-$(git diff-index --quiet HEAD && echo clean || echo dirty)}
    TAG=${TAG-$(git describe --tags 2>/dev/null || echo "v0.5.0")}
    COMMITS=${COMMITS-$(git rev-list --count HEAD 2>/dev/null || echo "0")}
fi

# No sed -i: BSD and macOS sed take its argument as a backup suffix
sed -e "s#@HASH@#$HASH#" \
    -e "s#@BRANCH@#$BRANCH#" \
    -e "s#@MESSAGE@#$MESSAGE#" \
    -e "s#@DATE@#$DATE#" \
    -e "s#@DIRTY@#$DIRTY#" \
    -e "s#@TAG@#$TAG#" \
    -e "s#@COMMITS@#$COMMITS#" \
    ./include/version.h.in > "$TMP"

if [ ! -f "$OUT" ] || ! cmp -s "$TMP" "$OUT"; then
    mv "$TMP" "$OUT"
else
    rm "$TMP"
fi
