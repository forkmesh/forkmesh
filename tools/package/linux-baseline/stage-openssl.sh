#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/image.env"
downloads="${FORKMESH_BASELINE_DOWNLOADS:-/var/cache/forkmesh-downloads}"
prefix=/opt/forkmesh/openssl
version="$FORKMESH_BASELINE_OPENSSL_VERSION"
tarball="$downloads/openssl-$version.tar.gz"
"$here/fetch.sh" \
  "https://github.com/openssl/openssl/releases/download/openssl-$version/openssl-$version.tar.gz" \
  "$FORKMESH_BASELINE_OPENSSL_SHA256" "$tarball"
src="$(mktemp -d)"
tar --no-same-owner -xzf "$tarball" -C "$src" --strip-components=1
cd "$src"
./Configure linux-x86_64 no-shared no-docs no-tests no-module -fPIC \
  --prefix="$prefix" --libdir=lib --openssldir=/usr/lib/ssl
make -j"${FORKMESH_BASELINE_JOBS:-8}" >/dev/null
make install_sw >/dev/null
cd /
rm -rf "$src"
"$prefix/bin/openssl" version | grep -q "^OpenSSL $version "
