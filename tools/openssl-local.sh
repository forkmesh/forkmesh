#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$REPO_ROOT/tools/forkmesh-paths.sh"
. "$REPO_ROOT/tools/forkmesh-safe-rm.sh"

version="${FORKMESH_OPENSSL_VERSION:-3.5.8}"
sha256="${FORKMESH_OPENSSL_SHA256:-a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2}"
root="${FORKMESH_OPENSSL_DIR:-$(forkmesh_toolchain_dir)/openssl}"
prefix="$root/openssl-$version-$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"

log() { echo "[openssl-local] $*" >&2; }

if [ -f "$prefix/.complete" ]; then
  printf '%s\n' "$prefix"
  exit 0
fi

mkdir -p "$root/downloads"
tarball="$root/downloads/openssl-$version.tar.gz"
if [ ! -f "$tarball" ] && [ -n "${FORKMESH_OPENSSL_TARBALL:-}" ]; then
  cp "$FORKMESH_OPENSSL_TARBALL" "$tarball.part"
  mv "$tarball.part" "$tarball"
fi
if [ ! -f "$tarball" ]; then
  log "downloading OpenSSL $version"
  curl -fsSL --connect-timeout 10 --max-time 600 -o "$tarball.part" \
    "https://github.com/openssl/openssl/releases/download/openssl-$version/openssl-$version.tar.gz"
  mv "$tarball.part" "$tarball"
fi
if ! echo "$sha256  $tarball" | sha256sum -c --quiet - >/dev/null 2>&1 &&
   ! echo "$sha256  $tarball" | shasum -a 256 -c --quiet - >/dev/null 2>&1; then
  log "openssl-$version.tar.gz does not match the pinned SHA-256; refusing it"
  rm -f "$tarball"
  exit 1
fi

src="$(mktemp -d "${TMPDIR:-/tmp}/forkmesh-openssl.XXXXXX")"
trap 'fm_safe_rm_tree "${TMPDIR:-/tmp}" "$src"' EXIT
tar -xzf "$tarball" -C "$src" --strip-components=1
jobs="$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)"
[ "$jobs" -gt 4 ] && jobs=4
log "building OpenSSL $version into $prefix ($jobs jobs)"
openssldir="$prefix/ssl"
if command -v openssl >/dev/null 2>&1; then
  system_dir="$(openssl version -d 2>/dev/null | sed -n 's/^OPENSSLDIR: "\(.*\)"$/\1/p')"
  [ -n "$system_dir" ] && [ -d "$system_dir" ] && openssldir="$system_dir"
fi
fm_safe_rm_tree "$root" "$prefix"
(
  cd "$src"
  ./Configure shared no-docs no-tests \
    --prefix="$prefix" --libdir=lib --openssldir="$openssldir" >&2
  make -j"$jobs" >/dev/null
  make install_sw >/dev/null
)
touch "$prefix/.complete"
printf '%s\n' "$prefix"
