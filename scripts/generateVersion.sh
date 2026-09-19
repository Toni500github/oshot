#!/bin/sh
set -e

OUT="./include/version.h"
TMP="$(mktemp)"

# Fallback values if git is missing or not a git repository
IS_GIT_REPO=true
if ! command -v git >/dev/null 2>&1 || [ ! -d ./.git ]; then
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

cp ./include/version.h.in "$TMP"

sed -i -e "s#@HASH@#$HASH#" "$TMP"
sed -i -e "s#@BRANCH@#$BRANCH#" "$TMP"
sed -i -e "s#@MESSAGE@#$MESSAGE#" "$TMP"
sed -i -e "s#@DATE@#$DATE#" "$TMP"
sed -i -e "s#@DIRTY@#$DIRTY#" "$TMP"
sed -i -e "s#@TAG@#$TAG#" "$TMP"
sed -i -e "s#@COMMITS@#$COMMITS#" "$TMP"

if [ ! -f "$OUT" ] || ! cmp -s "$TMP" "$OUT"; then
    mv "$TMP" "$OUT"
else
    rm "$TMP"
fi
