#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build a host-side libmbedcrypto.a for the rt-carplay unit tests.
#
# Usage:
#   RTCP_MBEDTLS_DIR=/path/to/mbedtls scripts/build-mbedtls-host.sh [source-dir]
#
# The source directory must be an mbedTLS 2.28 checkout (the RT-Thread SDK
# ships one under packages/third-party/mbedtls/mbedtls).  If no source is
# available, scripts/fetch-mbedtls.sh clones the pinned release.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
src=${1:-${RTCP_MBEDTLS_DIR:-$repo_root/third_party/mbedtls}}
out=$repo_root/build/mbedtls
cc=${CC:-cc}

if [ ! -f "$src/include/mbedtls/chachapoly.h" ] || [ ! -d "$src/library" ]; then
    echo "build-mbedtls-host: no mbedTLS 2.28 source at '$src'" >&2
    echo "  set RTCP_MBEDTLS_DIR, or run scripts/fetch-mbedtls.sh" >&2
    exit 2
fi

version=$(sed -n 's/^#define MBEDTLS_VERSION_STRING *"\(.*\)"/\1/p' \
    "$src/include/mbedtls/version.h" 2>/dev/null || true)
if [ -z "$version" ]; then
    version=$(awk '/MBEDTLS_VERSION_MAJOR/{m=$3} /MBEDTLS_VERSION_MINOR/{n=$3} \
        /MBEDTLS_VERSION_PATCH/{p=$3} END{printf "%s.%s.%s", m, n, p}' \
        "$src/include/mbedtls/version.h")
fi
case "$version" in
    2.28.*) ;;
    *)
        echo "build-mbedtls-host: expected mbedTLS 2.28.x, found $version" >&2
        exit 2
        ;;
esac

mkdir -p "$out/obj"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
cflags="-std=c99 -O2 -g -fno-strict-aliasing -fPIC -Wall -Wextra -Wno-unused-parameter"
cflags="$cflags -I$src/include -I$src/library -I$repo_root/config"
cflags="$cflags -DMBEDTLS_CONFIG_FILE='\"mbedtls_rtcp_host.h\"'"

# Compile in parallel; the loop is written for POSIX sh (no make dependency).
: > "$out/files.list.tmp"
for source in "$src"/library/*.c; do
    object="$out/obj/$(basename "$source" .c).o"
    printf '%s\n' "$object" >> "$out/files.list.tmp"
    if [ ! -f "$object" ] || [ "$source" -nt "$object" ]; then
        printf '%s\n' "$source"
    fi
done > "$out/rebuild.list"

if [ -s "$out/rebuild.list" ]; then
    xargs -P "$jobs" -n 1 sh -c '
        source=$1
        object='"$out"'/obj/$(basename "$source" .c).o
        exec '"$cc $cflags"' -c "$source" -o "$object"
    ' sh < "$out/rebuild.list"
fi

rm -f "$out/libmbedcrypto.a"
ar rcs "$out/libmbedcrypto.a" $(cat "$out/files.list.tmp")
rm -f "$out/files.list.tmp" "$out/rebuild.list"

echo "build-mbedtls-host: mbedTLS $version -> $out/libmbedcrypto.a"
