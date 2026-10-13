#!/usr/bin/env bash
# Locate the rootless Windows cross-build toolchain that
# tools/package/install-windows-toolchain.sh provisions: MinGW-w64 GCC, an
# official Qt win64_mingw kit, OpenSSL cross-built for MinGW, NSIS and
# osslsigncode. No Windows machine is part of the release fleet, so the
# Windows installer is cross-built on the Linux release host with these.
#
# Source this file rather than executing it. It exports:
#   FORKMESH_WIN_TOOLCHAIN_READY   1 when every piece is present, else 0
#   FORKMESH_WIN_TOOLCHAIN_REASON  explanation when not ready
# and, when ready, the variables desktop/cmake/mingw-w64-x86_64.cmake and
# tools/package/windows.sh read:
#   FORKMESH_WIN_MINGW_ROOT FORKMESH_WIN_QT_ROOT FORKMESH_WIN_OPENSSL_ROOT
#   FORKMESH_WIN_OPENSSL_BIN QT_HOST_PATH QT_HOST_PATH_CMAKE_DIR NSISDIR
#   FORKMESH_WIN_CMAKE_TOOLCHAIN
# plus PATH entries for makensis and osslsigncode.

FORKMESH_WIN_TOOLCHAIN_READY=0
FORKMESH_WIN_TOOLCHAIN_REASON=""

# shellcheck source=tools/forkmesh-paths.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/forkmesh-paths.sh"
FORKMESH_WIN_TOOLCHAIN="${FORKMESH_WIN_TOOLCHAIN:-$(forkmesh_shared_dir toolchain-windows "$HOME/.cache/forkmesh-win-toolchain")}"
FORKMESH_WIN_QT_VERSION="${FORKMESH_WIN_QT_VERSION:-6.8.2}"
FORKMESH_WIN_OPENSSL_VERSION="${FORKMESH_WIN_OPENSSL_VERSION:-3.5.8}"

_fm_win_here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_fm_win_sysroot="$FORKMESH_WIN_TOOLCHAIN/sysroot"
_fm_win_qt="$FORKMESH_WIN_TOOLCHAIN/Qt/$FORKMESH_WIN_QT_VERSION/mingw_64"
_fm_win_openssl="$FORKMESH_WIN_TOOLCHAIN/openssl-$FORKMESH_WIN_OPENSSL_VERSION"

_fm_win_missing=""
[ -x "$_fm_win_sysroot/usr/bin/x86_64-w64-mingw32-g++-posix" ] || _fm_win_missing="$_fm_win_missing mingw-w64"
[ -x "$_fm_win_sysroot/usr/bin/makensis" ] || _fm_win_missing="$_fm_win_missing nsis"
[ -x "$_fm_win_sysroot/usr/bin/osslsigncode" ] || _fm_win_missing="$_fm_win_missing osslsigncode"
[ -f "$_fm_win_qt/lib/cmake/Qt6/Qt6Config.cmake" ] || _fm_win_missing="$_fm_win_missing qt-$FORKMESH_WIN_QT_VERSION-win64_mingw"
[ -f "$_fm_win_openssl/bin/libcrypto-3-x64.dll" ] || _fm_win_missing="$_fm_win_missing openssl-$FORKMESH_WIN_OPENSSL_VERSION-mingw64"
# Host moc/rcc/uic must match the kit's major.minor (Qt refuses mismatched
# host tools). Debian trixie ships 6.8.2, the same as the kit.
# Qt finds the host package under PATHS "$QT_HOST_PATH" "$QT_HOST_PATH_CMAKE_DIR"
# with NO_DEFAULT_PATH. When cross-compiling, CMAKE_LIBRARY_ARCHITECTURE names
# the *target*, so the prefix alone never reaches Debian's multiarch
# lib/x86_64-linux-gnu/cmake: the cmake directory has to be named outright, or
# Qt silently falls back to the kit's Windows moc/rcc/uic and generation fails.
_fm_win_host_qt=""
_fm_win_host_qt_cmake=""
for _fm_win_candidate in /usr /usr/lib/qt6 "${QT_HOST_PATH:-}"; do
  [ -n "$_fm_win_candidate" ] || continue
  for _fm_win_libdir in "lib/$(uname -m)-linux-gnu" lib lib64; do
    if [ -f "$_fm_win_candidate/$_fm_win_libdir/cmake/Qt6HostInfo/Qt6HostInfoConfig.cmake" ]; then
      _fm_win_host_qt="$_fm_win_candidate"
      _fm_win_host_qt_cmake="$_fm_win_candidate/$_fm_win_libdir/cmake"
      break
    fi
  done
  if [ -n "$_fm_win_host_qt" ]; then break; fi
done
[ -n "$_fm_win_host_qt" ] || _fm_win_missing="$_fm_win_missing host-qt6-tools"

if [ -n "$_fm_win_missing" ]; then
  FORKMESH_WIN_TOOLCHAIN_REASON="missing:$_fm_win_missing (run tools/package/install-windows-toolchain.sh)"
else
  export FORKMESH_WIN_MINGW_ROOT="$_fm_win_sysroot"
  export FORKMESH_WIN_QT_ROOT="$_fm_win_qt"
  export FORKMESH_WIN_OPENSSL_ROOT="$_fm_win_openssl"
  export FORKMESH_WIN_OPENSSL_BIN="$_fm_win_openssl/bin"
  export QT_HOST_PATH="$_fm_win_host_qt"
  export QT_HOST_PATH_CMAKE_DIR="$_fm_win_host_qt_cmake"
  export NSISDIR="$_fm_win_sysroot/usr/share/nsis"
  export FORKMESH_WIN_CMAKE_TOOLCHAIN="$_fm_win_here/../../desktop/cmake/mingw-w64-x86_64.cmake"
  case ":$PATH:" in
    *":$_fm_win_sysroot/usr/bin:"*) ;;
    *) export PATH="$_fm_win_sysroot/usr/bin:$PATH" ;;
  esac
  FORKMESH_WIN_TOOLCHAIN_READY=1
fi
export FORKMESH_WIN_TOOLCHAIN_READY FORKMESH_WIN_TOOLCHAIN_REASON
unset _fm_win_here _fm_win_sysroot _fm_win_qt _fm_win_openssl _fm_win_missing
unset _fm_win_host_qt _fm_win_host_qt_cmake _fm_win_candidate _fm_win_libdir
