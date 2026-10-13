#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/image.env"
downloads="${FORKMESH_BASELINE_DOWNLOADS:-/var/cache/forkmesh-downloads}"
jobs="${FORKMESH_BASELINE_JOBS:-8}"
version="$FORKMESH_BASELINE_QT_VERSION"
series="${version%.*}"
prefix=/opt/forkmesh/qt
openssl=/opt/forkmesh/openssl

for module in qtbase qtsvg; do
  var="FORKMESH_BASELINE_$(echo "$module" | tr '[:lower:]' '[:upper:]')_SHA256"
  "$here/fetch.sh" \
    "https://download.qt.io/archive/qt/$series/$version/submodules/$module-everywhere-src-$version.tar.xz" \
    "${!var}" "$downloads/$module-everywhere-src-$version.tar.xz"
done

work="${FORKMESH_BASELINE_QT_WORK:-$(mktemp -d)}"
mkdir -p "$work"
for module in qtbase qtsvg; do
  rm -rf "${work:?}/$module"
  mkdir -p "$work/$module"
  tar --no-same-owner -xJf "$downloads/$module-everywhere-src-$version.tar.xz" -C "$work/$module" --strip-components=1
done

cmake -S "$work/qtbase" -B "$work/qtbase-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DBUILD_SHARED_LIBS=OFF \
  -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DQT_BUILD_BENCHMARKS=OFF \
  -DQT_QPA_PLATFORMS="xcb;offscreen;minimal" \
  -DQT_QPA_DEFAULT_PLATFORM=xcb \
  -DINPUT_opengl=desktop \
  -DINPUT_openssl=linked \
  -DINPUT_dbus=runtime \
  -DOPENSSL_ROOT_DIR="$openssl" -DOPENSSL_USE_STATIC_LIBS=ON \
  -DFEATURE_icu=OFF -DFEATURE_glib=OFF -DFEATURE_libproxy=OFF -DFEATURE_gssapi=OFF \
  -DFEATURE_brotli=OFF -DFEATURE_zstd=OFF -DFEATURE_libudev=OFF -DFEATURE_mtdev=OFF \
  -DFEATURE_tslib=OFF -DFEATURE_evdev=OFF -DFEATURE_libinput=OFF -DFEATURE_vulkan=OFF \
  -DFEATURE_eglfs=OFF -DFEATURE_linuxfb=OFF -DFEATURE_vnc=OFF -DFEATURE_kms=OFF \
  -DFEATURE_sql=OFF -DFEATURE_gtk3=OFF -DFEATURE_sctp=OFF \
  -DFEATURE_system_pcre2=OFF -DFEATURE_system_harfbuzz=OFF -DFEATURE_system_png=OFF \
  -DFEATURE_system_jpeg=OFF -DFEATURE_system_doubleconversion=OFF \
  -DFEATURE_system_md4c=OFF -DFEATURE_system_libb2=OFF
for feature in xcb xkbcommon_x11 fontconfig opengl openssl_linked dbus xcb_glx_plugin xcb_egl_plugin; do
  grep -Eq "^QT_FEATURE_$feature:[A-Z]+=ON" "$work/qtbase-build/CMakeCache.txt" || {
    echo "Qt configure left feature $feature off" >&2
    exit 1
  }
done
cmake --build "$work/qtbase-build" -j"$jobs"
cmake --install "$work/qtbase-build"

"$prefix/bin/qt-cmake" -S "$work/qtsvg" -B "$work/qtsvg-build" -G Ninja \
  -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF
cmake --build "$work/qtsvg-build" -j"$jobs"
cmake --install "$work/qtsvg-build"

rm -rf "$work"
grep -q "QT_VERSION_STR \"$version\"" "$prefix/include/QtCore/qconfig.h"
[ -f "$prefix/lib/libQt6Core.a" ] && [ -f "$prefix/lib/libQt6Svg.a" ] && [ ! -e "$prefix/lib/libQt6Core.so" ]
