#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch the pinned mbedTLS source used by the host test build.
# mbedTLS is Apache-2.0 and is not vendored in this repository.
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
dest=${1:-$repo_root/third_party/mbedtls}
tag=${RTCP_MBEDTLS_TAG:-v2.28.1}

if [ -f "$dest/include/mbedtls/chachapoly.h" ]; then
    echo "fetch-mbedtls: $dest already populated"
    exit 0
fi

git clone --depth 1 --branch "$tag" \
    https://github.com/Mbed-TLS/mbedtls.git "$dest"
echo "fetch-mbedtls: $tag -> $dest"
