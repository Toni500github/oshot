#!/bin/sh
# Update the translation template po/oshot.pot from the sources, then merge it into every po/<code>.po.
#
#   scripts/update_translations.sh              update po/oshot.pot and po/*.po
#   scripts/update_translations.sh --init CODE  also start po/CODE.po (e.g. it, pt_BR) from the template
#   scripts/update_translations.sh --check      only check that po/oshot.pot matches the sources (for CI)
#
# Needs GNU gettext: xgettext, msguniq, msgmerge, msginit and msgfmt. See docs/translating.md.
set -e

cd "$(dirname "$0")/.."

PO_DIR="po"
POT="$PO_DIR/oshot.pot"
PLURAL_TABLE="src/libs/tinygettext/plural_forms.cpp"

usage() {
    sed -n '2,8s/^# \{0,1\}//p' "$0"
    exit "$1"
}

MODE="update"
INIT_CODE=""
case "$1" in
    "") ;;
    --check) MODE="check" ;;
    --init)
        [ -n "$2" ] || usage 1
        INIT_CODE="$2"
        ;;
    -h | --help) usage 0 ;;
    *) usage 1 ;;
esac

for tool in xgettext msguniq msgmerge msginit msgfmt; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "error: $tool not found, install GNU gettext" >&2
        exit 1
    fi
done

# Same version as scripts/generateVersion.sh's fallback, e.g. 0.5.0-rc2
VERSION="$(sed -n 's/^project(oshot VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)$(sed -n 's/^set(PROJECT_VERSION_SUFFIX "\(.*\)")/\1/p' CMakeLists.txt)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# ------------------------------
# Template
# ------------------------------

# Our sources only (the vendored libraries in */libs are left out)
find src include -path '*/libs' -prune -o \( -name '*.cpp' -o -name '*.hpp' -o -name '*.mm' \) -print |
    LC_ALL=C sort >"$TMP/sources"

# -k alone drops xgettext's default keywords (gettext() & co.), which oshot doesn't use.
# The keywords are the ones in include/i18n.hpp; --no-wrap keeps every msgid on one line for the sed below
xgettext --files-from="$TMP/sources" \
    --language=C++ --from-code=UTF-8 --no-wrap \
    --add-comments=TRANSLATORS: \
    -k -k_ -kN_ -ktr -ktr_fmt -ktr_n:1,2 -ktr_n_fmt:1,2 \
    --package-name=oshot --package-version="$VERSION" \
    --msgid-bugs-address="https://github.com/Toni500github/oshot/issues" \
    --copyright-holder="Toni500" \
    --output="$TMP/raw.pot"

# i18n::tr() looks up "Save" for "Save##save_btn" and keeps the ImGui ID suffix as it is:
# drop the suffix from the msgids, then merge the ones that became equal
sed -e '/^msgid "/s/##[^"]*"$/"/' -e '/^msgid_plural "/s/##[^"]*"$/"/' "$TMP/raw.pot" >"$TMP/stripped.pot"
msguniq --no-wrap --output-file="$TMP/oshot.pot" "$TMP/stripped.pot"

# Compare without the creation date, which changes on every run
same_template() {
    [ -f "$2" ] && [ "$(grep -v '^"POT-Creation-Date:' "$1")" = "$(grep -v '^"POT-Creation-Date:' "$2")" ]
}

if [ "$MODE" = "check" ]; then
    if same_template "$TMP/oshot.pot" "$POT"; then
        echo "$POT is up to date"
        exit 0
    fi
    echo "error: $POT is out of date, run scripts/update_translations.sh" >&2
    exit 1
fi

mkdir -p "$PO_DIR"
if same_template "$TMP/oshot.pot" "$POT"; then
    echo "$POT: no changes"
else
    cp "$TMP/oshot.pot" "$POT"
    echo "$POT: $(grep -c '^msgid "' "$POT") strings (header included)"
fi

# ------------------------------
# Catalogs
# ------------------------------

if [ -n "$INIT_CODE" ]; then
    if [ -f "$PO_DIR/$INIT_CODE.po" ]; then
        echo "error: $PO_DIR/$INIT_CODE.po already exists" >&2
        exit 1
    fi
    # .UTF-8: msginit takes the charset from the locale name
    msginit --no-translator --no-wrap --locale="$INIT_CODE.UTF-8" \
        --input="$POT" --output-file="$PO_DIR/$INIT_CODE.po" 2>/dev/null
    echo "$PO_DIR/$INIT_CODE.po: created"
    if grep -q '^"Plural-Forms: nplurals=INTEGER' "$PO_DIR/$INIT_CODE.po"; then
        echo "warning: msginit doesn't know the plural rule of $INIT_CODE: set Plural-Forms in $PO_DIR/$INIT_CODE.po" >&2
        echo "         to one in $PLURAL_TABLE, e.g. 'nplurals=1; plural=0;' for a language without plurals" >&2
    fi
fi

# tinygettext only knows the Plural-Forms in its table, compared without spaces
sed -n 's/.*plural_forms\["\(Plural-Forms:[^"]*\)"\].*/\1/p' "$PLURAL_TABLE" >"$TMP/plurals"

status=0
for po in "$PO_DIR"/*.po; do
    [ -f "$po" ] || continue

    # --previous: a fuzzy entry also shows the msgid it was translated from.
    # Fuzzy entries are never shown in oshot (see src/i18n.cpp), only offered to the translator
    msgmerge --quiet --update --backup=none --no-wrap --previous "$po" "$POT"

    # --check: a well-formed header, and the same printf placeholders in the c-format strings
    if ! stats="$(msgfmt --check --statistics --output-file=/dev/null "$po" 2>&1)"; then
        echo "$stats" >&2
        echo "error: $po has errors" >&2
        status=1
        continue
    fi

    plural="$(sed -n 's/^"\(Plural-Forms:.*\)\\n"$/\1/p' "$po" | tr -d ' \t')"
    if ! grep -qxF "$plural" "$TMP/plurals"; then
        echo "warning: $po: tinygettext doesn't know '$plural', so plural strings stay in English." >&2
        echo "         Use one of the Plural-Forms in $PLURAL_TABLE" >&2
    fi

    echo "$po: $stats"
done

exit "$status"
