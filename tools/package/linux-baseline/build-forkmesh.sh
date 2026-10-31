#!/usr/bin/env bash
set -euo pipefail
src="${FORKMESH_BASELINE_SRC:-/src}"
tools="${FORKMESH_BASELINE_TOOLS:-/tools}"
work="${FORKMESH_BASELINE_WORK:-/work}"
out="${FORKMESH_BASELINE_OUT:-/out}"
version="${FORKMESH_BASELINE_VERSION:?}"
commit="${FORKMESH_BASELINE_COMMIT:?}"
channel="${FORKMESH_BASELINE_CHANNEL:-latest}"
build_number="${FORKMESH_BASELINE_BUILD:-0}"
jobs="${FORKMESH_BASELINE_JOBS:-4}"
arch=x86_64
asset="forkmesh-linux-$arch"
build="$work/build"
install_root="$work/install"

export PATH="/opt/forkmesh/appimage-tools:$PATH"
export PKG_CONFIG_PATH="/opt/forkmesh/deps/lib/pkgconfig"
export CCACHE_DIR="$work/ccache"
export CCACHE_BASEDIR="$src"
export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-8G}"
export CCACHE_SLOPPINESS=pch_defines,time_macros
export APPIMAGE_EXTRACT_AND_RUN=1
export TMPDIR="$work/tmp"
mkdir -p "$TMPDIR" "$out"

if [ -f "$build/CMakeCache.txt" ] && ! grep -qxF "CMAKE_HOME_DIRECTORY:INTERNAL=$src/desktop" "$build/CMakeCache.txt"; then
  rm -rf "$build"
fi

cmake -S "$src/desktop" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DFORKMESH_BUILD_TESTS=OFF \
  -DFORKMESH_REQUIRE_OPENSSL_3_5=ON \
  -DFORKMESH_DISABLE_MOLD=ON \
  -DFORKMESH_PACKAGE_MODULES=OFF \
  -DCMAKE_PREFIX_PATH="/opt/forkmesh/qt;/opt/forkmesh/deps" \
  -DOPENSSL_ROOT_DIR=/opt/forkmesh/openssl -DOPENSSL_USE_STATIC_LIBS=ON \
  -DFORKMESH_WEBP_LIBRARY=/usr/lib/x86_64-linux-gnu/libwebp.a \
  -DFORKMESH_CCACHE=OFF \
  -DCMAKE_CXX_STANDARD_LIBRARIES="/usr/lib/x86_64-linux-gnu/libxcb-util.a -lxcb" \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_AUTOGEN_PARALLEL="$jobs" \
  -DFORKMESH_BUILD_COMMIT_OVERRIDE="$commit" \
  -DFORKMESH_VERSION_OVERRIDE="$version"
cmake --build "$build" -j"$jobs"
rm -rf "$install_root"
cmake --install "$build" --prefix "$install_root"

binary="$install_root/bin/forkmesh"
[ -x "$binary" ] || { echo "no forkmesh binary was installed" >&2; exit 1; }
reported="$("$binary" --version)"
[ "$reported" = "ForkMesh $version" ] || { echo "binary reports '$reported'" >&2; exit 1; }
[ "$("$binary" --build-commit)" = "$commit" ] || { echo "binary carries another build commit" >&2; exit 1; }
if readelf -d "$binary" | grep -E 'NEEDED.*(libQt6|libssl|libcrypto|libnghttp3|libwebp|libicu)'; then
  echo "the release binary must not load Qt, OpenSSL, nghttp3, libwebp or ICU from the system" >&2
  exit 1
fi

readelf -Ws "$binary" > "$work/symbols.txt"
grep -q ' nghttp3_conn_client_new' "$work/symbols.txt" || { echo "the release binary was built without HTTP/3" >&2; exit 1; }

rm -f "$out/$asset" "$out/$asset.AppImage" "$out/$asset.AppImage.zsync" "$out/$asset.deb"
install -m 0755 "$binary" "$out/$asset"
(
  cd "$src"
  FORKMESH_PKG_ARCH="$arch" FORKMESH_PKG_VERSION="$version" FORKMESH_PKG_CHANNEL="$channel" \
  FORKMESH_PKG_INSTALL_ROOT="$install_root" \
    bash "$tools/tools/package/appimage.sh" "$out/$asset" "$out" >/dev/null
)
[ -s "$out/$asset.AppImage" ] || { echo "no AppImage was produced" >&2; exit 1; }
[ -s "$out/$asset.AppImage.zsync" ] || { echo "no AppImage zsync file was produced" >&2; exit 1; }
[ "$("$out/$asset.AppImage" --version)" = "ForkMesh $version" ] || { echo "the AppImage does not report $version" >&2; exit 1; }
FORKMESH_PKG_ARCH="$arch" FORKMESH_PKG_VERSION="$version" FORKMESH_PKG_BUILD="$build_number" \
FORKMESH_PKG_INSTALL_ROOT="$install_root" \
  bash "$tools/tools/package/deb.sh" "$out/$asset" "$out" >/dev/null
[ -s "$out/$asset.deb" ] || { echo "no Debian package was produced" >&2; exit 1; }
readelf -d "$out/$asset" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | sort > "$out/$asset.needed"
ls -l "$out"
