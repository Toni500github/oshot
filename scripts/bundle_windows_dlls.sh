#!/usr/bin/env bash
# Copy the MSYS2 DLLs that the .exe and .dll files in a folder need into that
# folder, following the imports of every DLL it copies, so the folder runs
# without MSYS2 installed.
#
# Only DLLs found in the search dir are copied; any other import is taken to
# be part of Windows (KERNEL32.dll, api-ms-win-crt-*.dll, ...). Reads the
# import tables with objdump, so it works for every MSYS2 environment
# (MINGW64, CLANGARM64, ...) and doesn't need to run the binaries like ldd.
#
# usage: scripts/bundle_windows_dlls.sh <dir> [search-dir]
#   search-dir defaults to $MINGW_PREFIX/bin, the active MSYS2 environment
#   OBJDUMP overrides the objdump to use (GNU or llvm-objdump)

set -euo pipefail

dir="${1:?usage: $0 <dir> [search-dir]}"
search="${2:-${MINGW_PREFIX:?MINGW_PREFIX is not set, pass the search dir}/bin}"
objdump="${OBJDUMP:-$(command -v objdump || command -v llvm-objdump)}"

# DLL names are case-insensitive, and import tables don't always match the file's case
declare -A available
for f in "$search"/*.dll; do
    name="$(basename "$f")"
    available["${name,,}"]="$f"
done

shopt -s nullglob
todo=("$dir"/*.exe "$dir"/*.dll)
declare -A seen
while [ ${#todo[@]} -gt 0 ]; do
    file="${todo[-1]}"
    unset 'todo[-1]'
    # Not a process substitution: an objdump failure must stop the script, not give a partial bundle
    imports="$("$objdump" -p "$file" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr -d '\r')"
    while read -r dll; do
        if [ -z "$dll" ]; then continue; fi
        key="${dll,,}"
        if [ -n "${seen[$key]:-}" ]; then continue; fi
        seen[$key]=1
        src="${available[$key]:-}"
        if [ -z "$src" ]; then continue; fi
        dest="$dir/$(basename "$src")"
        if [ ! -e "$dest" ]; then
            cp "$src" "$dest"
            echo "bundled $(basename "$src")"
        fi
        todo+=("$dest")
    done <<< "$imports"
done
