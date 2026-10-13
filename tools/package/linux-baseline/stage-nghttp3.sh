#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/image.env"
downloads="${FORKMESH_BASELINE_DOWNLOADS:-/var/cache/forkmesh-downloads}"
prefix=/opt/forkmesh/deps
version="$FORKMESH_BASELINE_NGHTTP3_VERSION"
tarball="$downloads/nghttp3-$version.tar.xz"
"$here/fetch.sh" \
  "https://github.com/ngtcp2/nghttp3/releases/download/v$version/nghttp3-$version.tar.xz" \
  "$FORKMESH_BASELINE_NGHTTP3_SHA256" "$tarball"
src="$(mktemp -d)"
tar --no-same-owner -xJf "$tarball" -C "$src" --strip-components=1
cmake -S "$src" -B "$src/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON -DENABLE_LIB_ONLY=ON -DBUILD_TESTING=OFF
cmake --build "$src/build" -j"${FORKMESH_BASELINE_JOBS:-8}"
cmake --install "$src/build"
rm -rf "$src"
[ -f "$prefix/lib/libnghttp3.a" ] && [ ! -e "$prefix/lib/libnghttp3.so" ]
