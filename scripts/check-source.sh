#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Source tree checks that run in CI and locally:
#   1. Source files and build scripts contain only ASCII characters.
#   2. Banned C library functions are not used.
#   3. Every C source and header starts with an SPDX license identifier.
#   4. Modules under src/ include only headers from their own layer or layers below.
#
# Usage: scripts/check-source.sh [repository root]

set -euo pipefail

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$root"

failures=0

report() {
    echo "check-source: $*" >&2
    failures=$((failures + 1))
}

list_c_files() {
    find src include tools tests -type f \( -name '*.c' -o -name '*.h' \) \
        -not -path 'tests/data/*' 2>/dev/null | sort
}

list_ascii_checked_files() {
    {
        list_c_files
        find . -maxdepth 1 -type f -name 'CMakeLists.txt'
        find src include tools tests cmake scripts -type f \
            \( -name 'CMakeLists.txt' -o -name '*.cmake' -o -name '*.sh' \) \
            -not -path 'tests/data/*' 2>/dev/null
    } | sort -u
}

# 1. ASCII only.
while IFS= read -r file; do
    if LC_ALL=C grep -n $'[\x80-\xff]' "$file" >/dev/null; then
        line="$(LC_ALL=C grep -n $'[\x80-\xff]' "$file" | head -1 | cut -d: -f1)"
        report "$file:$line: non-ASCII character"
    fi
done < <(list_ascii_checked_files)

# 2. Banned functions.
banned='\b(strcpy|strcat|strncpy|strncat|sprintf|vsprintf|gets|strtok)[[:space:]]*\('
while IFS= read -r file; do
    if grep -n -E "$banned" "$file" >/dev/null; then
        while IFS= read -r hit; do
            report "$file:${hit%%:*}: banned function"
        done < <(grep -n -E "$banned" "$file")
    fi
done < <(list_c_files)

# 3. SPDX header within the first two lines.
while IFS= read -r file; do
    if ! head -2 "$file" | grep -q 'SPDX-License-Identifier:'; then
        report "$file: missing SPDX-License-Identifier in the first two lines"
    fi
done < <(list_c_files)

# 4. Layering. A module may include headers from its own rank or lower ranks only.
layer_rank() {
    case "$1" in
        os | common) echo 0 ;;
        mem) echo 1 ;;
        encoding) echo 2 ;;
        storage) echo 3 ;;
        sql | exec | arrow) echo 4 ;;
        backup | server) echo 5 ;;
        *) echo -1 ;;
    esac
}

while IFS= read -r file; do
    layer="$(echo "$file" | cut -d/ -f2)"
    own_rank="$(layer_rank "$layer")"
    if [ "$own_rank" -lt 0 ]; then
        report "$file: unknown layer '$layer' (add it to scripts/check-source.sh)"
        continue
    fi
    while IFS= read -r included; do
        inc_layer="${included%%/*}"
        inc_rank="$(layer_rank "$inc_layer")"
        if [ "$inc_rank" -gt "$own_rank" ]; then
            report "$file: layer '$layer' must not include '$included'"
        fi
    done < <(sed -n -E 's/^#include "([a-z_]+\/[^"]+)".*/\1/p' "$file")
done < <(find src -type f \( -name '*.c' -o -name '*.h' \) | sort)

if [ "$failures" -gt 0 ]; then
    echo "check-source: $failures problem(s) found" >&2
    exit 1
fi
echo "check-source: ok"
