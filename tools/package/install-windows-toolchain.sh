#!/usr/bin/env bash
# Provision, without root, everything needed to cross-build and sign the
# Windows installer on a Debian/Ubuntu release host (see
# tools/package/windows-toolchain.sh for what gets exported afterwards):
#
#   * Debian's MinGW-w64 GCC (posix threads), NSIS and osslsigncode, fetched
#     with `apt-get download` from the configured apt sources and unpacked with
#     dpkg-deb -x into <toolchain>/sysroot. The GCC driver is relocatable.
#   * The official Qt win64_mingw kit, downloaded with aqtinstall into
#     <toolchain>/Qt. Host moc/rcc/uic come from the distribution's
#     qt6-base-dev-tools, which must match the kit's major.minor (Debian
#     trixie: 6.8.2).
#   * OpenSSL, cross-built with that MinGW GCC into <toolchain>/openssl-<ver>.
#     Qt's tools_opensslv3_x64 is 3.0, which lacks the Argon2id KDF and
#     ML-KEM the Password Vault page needs (desktop/CMakeLists.txt asks 3.5).
#     The tarball is pinned by SHA-256.
#
# Idempotent: pieces already present are skipped. Override the location with
# FORKMESH_WIN_TOOLCHAIN and the kit with FORKMESH_WIN_QT_VERSION.
set -euo pipefail

# shellcheck source=tools/forkmesh-paths.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/forkmesh-paths.sh"
# shellcheck source=tools/forkmesh-safe-rm.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/forkmesh-safe-rm.sh"
root="${FORKMESH_WIN_TOOLCHAIN:-$(forkmesh_shared_dir toolchain-windows "$HOME/.cache/forkmesh-win-toolchain")}"
qt_version="${FORKMESH_WIN_QT_VERSION:-6.8.2}"
openssl_version="${FORKMESH_WIN_OPENSSL_VERSION:-3.5.8}"
openssl_sha256="${FORKMESH_WIN_OPENSSL_SHA256:-a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2}"
log() { echo "[windows-toolchain] $*" >&2; }

mkdir -p "$root/debs" "$root/sysroot"

if [ ! -x "$root/sysroot/usr/bin/x86_64-w64-mingw32-g++-posix" ] \
   || [ ! -x "$root/sysroot/usr/bin/makensis" ] \
   || [ ! -x "$root/sysroot/usr/bin/osslsigncode" ]; then
  log "downloading MinGW-w64, NSIS and osslsigncode from apt"
  (
    cd "$root/debs"
    apt-get download \
      g++-mingw-w64-x86-64-posix gcc-mingw-w64-x86-64-posix \
      gcc-mingw-w64-x86-64-posix-runtime binutils-mingw-w64-x86-64 \
      mingw-w64-x86-64-dev mingw-w64-common gcc-mingw-w64-base \
      nsis nsis-common osslsigncode
  )
  for deb in "$root"/debs/*.deb; do
    dpkg-deb -x "$deb" "$root/sysroot"
  done
fi
"$root/sysroot/usr/bin/x86_64-w64-mingw32-g++-posix" --version | head -n1 >&2

if [ ! -x "$root/aqt-venv/bin/aqt" ]; then
  log "creating the aqtinstall virtualenv"
  python3 -m venv "$root/aqt-venv"
  "$root/aqt-venv/bin/pip" -q install aqtinstall
fi
if [ ! -f "$root/Qt/$qt_version/mingw_64/lib/cmake/Qt6/Qt6Config.cmake" ]; then
  log "downloading Qt $qt_version win64_mingw"
  "$root/aqt-venv/bin/aqt" install-qt windows desktop "$qt_version" win64_mingw -O "$root/Qt"
fi
openssl_prefix="$root/openssl-$openssl_version"
if [ ! -f "$openssl_prefix/bin/libcrypto-3-x64.dll" ]; then
  log "cross-building OpenSSL $openssl_version for MinGW"
  tarball="$root/debs/openssl-$openssl_version.tar.gz"
  if [ ! -f "$tarball" ]; then
    curl -fsSL -o "$tarball.part" \
      "https://github.com/openssl/openssl/releases/download/openssl-$openssl_version/openssl-$openssl_version.tar.gz"
    mv "$tarball.part" "$tarball"
  fi
  if ! echo "$openssl_sha256  $tarball" | sha256sum -c --quiet -; then
    log "openssl-$openssl_version.tar.gz does not match the pinned SHA-256"
    rm -f "$tarball"
    exit 1
  fi
  src="$root/openssl-$openssl_version-src"
  fm_safe_rm_tree "$root" "$src"
  mkdir -p "$src"
  tar -xzf "$tarball" -C "$src" --strip-components=1
  # windres preprocesses .rc files with the unsuffixed x86_64-w64-mingw32-gcc,
  # which Debian only ships as the -posix/-win32 alternatives.
  mkdir -p "$src/.bin"
  ln -s "$root/sysroot/usr/bin/x86_64-w64-mingw32-gcc-posix" "$src/.bin/x86_64-w64-mingw32-gcc"
  (
    cd "$src"
    PATH="$src/.bin:$root/sysroot/usr/bin:$PATH"
    ./Configure mingw64 shared no-docs no-tests \
      --cross-compile-prefix=x86_64-w64-mingw32- \
      --prefix="$openssl_prefix" --libdir=lib \
      CC=gcc-posix CXX=g++-posix \
      --sysroot="$root/sysroot/usr/x86_64-w64-mingw32"
    make -j"$(nproc)" >/dev/null
    make install_sw >/dev/null
  )
  fm_safe_rm_tree "$root" "$src"
fi

# shellcheck source=windows-toolchain.sh
. "$(cd "$(dirname "$0")" && pwd)/windows-toolchain.sh"
if [ "$FORKMESH_WIN_TOOLCHAIN_READY" != 1 ]; then
  log "toolchain still incomplete: $FORKMESH_WIN_TOOLCHAIN_REASON"
  exit 1
fi
log "ready under $root"
