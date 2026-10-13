#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: FORKMESH_BUILD_COMMIT=<sha> $0 <source-dir> [work-dir]" >&2
  exit 2
}

[ $# -ge 1 ] || usage
src="$(cd "$1" && pwd)"
work="${2:-$HOME/desktop-release}"
commit="${FORKMESH_BUILD_COMMIT:-}"
[ -n "$commit" ] || usage
[ "$(uname -s)" = "Darwin" ] || { echo "run this on macOS" >&2; exit 1; }
# shellcheck source=tools/forkmesh-safe-rm.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/forkmesh-safe-rm.sh"

qt_version="${FORKMESH_QT_VERSION:-6.11.3}"
qt_modules="${FORKMESH_QT_MODULES:-qtmultimedia qtconnectivity qtimageformats}"
deployment_target="${FORKMESH_MACOS_DEPLOYMENT_TARGET:-13.0}"
openssl_version="3.5.8"
openssl_sha256="a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2"
webp_version="1.6.0"
webp_sha256="e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"
nghttp3_version="1.18.0"
nghttp3_sha256="aad782c23d3f01bd4bb52c8bac7a553b631ef8115fd1612703df6183449fef19"
archs=(arm64 x86_64)

mkdir -p "$work"
work="$(cd "$work" && pwd)"
dl="$work/dl"
deps="$work/deps"
venv="$work/venv"
qt_root="$work/Qt"
qt_prefix="$qt_root/$qt_version/macos"
build="$work/build-$(basename "$src")"
stage="$work/stage-$(basename "$src")"
dist="$work/dist"
export MACOSX_DEPLOYMENT_TARGET="$deployment_target"

log() { echo "[macos-universal] $(date '+%H:%M:%S') $*" >&2; }

fetch() {
  local url="$1" out="$2" sum="$3"
  mkdir -p "$dl"
  [ -f "$dl/$out" ] || curl -fsSL --retry 3 -o "$dl/$out.part" "$url"
  [ -f "$dl/$out" ] || mv "$dl/$out.part" "$dl/$out"
  echo "$sum  $dl/$out" | shasum -a 256 -c --quiet - || { rm -f "$dl/$out"; exit 1; }
}

if [ ! -x "$venv/bin/aqt" ] || [ ! -x "$venv/bin/ninja" ]; then
  log "python venv: aqtinstall cmake ninja pkgconf"
  /usr/bin/python3 -m venv "$venv"
  "$venv/bin/pip" install -q --upgrade pip
  "$venv/bin/pip" install -q aqtinstall cmake ninja pkgconf
fi
export PATH="$venv/bin:$PATH"

if [ ! -x "$qt_prefix/bin/macdeployqt" ]; then
  log "Qt $qt_version (universal) with $qt_modules"
  read -r -a qt_module_list <<<"$qt_modules"
  aqt install-qt mac desktop "$qt_version" clang_64 -O "$qt_root" -m "${qt_module_list[@]}"
fi
lipo "$qt_prefix/lib/QtCore.framework/QtCore" -verify_arch arm64 x86_64

openssl_prefix="$deps/openssl-$openssl_version"
if [ ! -f "$openssl_prefix/.complete" ]; then
  fetch "https://github.com/openssl/openssl/releases/download/openssl-$openssl_version/openssl-$openssl_version.tar.gz" \
    "openssl-$openssl_version.tar.gz" "$openssl_sha256"
  fm_safe_rm_tree "$work" "$openssl_prefix"
  for arch in "${archs[@]}"; do
    log "OpenSSL $openssl_version for $arch"
    tree="$work/obj/openssl-$arch"
    fm_safe_rm_tree "$work" "$tree" "$openssl_prefix-$arch"
    mkdir -p "$tree"
    tar -xzf "$dl/openssl-$openssl_version.tar.gz" -C "$tree" --strip-components=1
    (
      cd "$tree"
      ./Configure "darwin64-$arch-cc" shared no-docs no-tests \
        "-mmacosx-version-min=$deployment_target" \
        --prefix="$openssl_prefix-$arch" --libdir=lib --openssldir=/private/etc/ssl >/dev/null
      make -j"$(sysctl -n hw.ncpu)" >/dev/null
      make install_sw >/dev/null
    )
  done
  mkdir -p "$openssl_prefix/lib"
  cp -R "$openssl_prefix-arm64/include" "$openssl_prefix/include"
  diff -r "$openssl_prefix-arm64/include" "$openssl_prefix-x86_64/include" >/dev/null
  cp -R "$openssl_prefix-arm64/lib/pkgconfig" "$openssl_prefix/lib/pkgconfig"
  sed -i '' "s#$openssl_prefix-arm64#$openssl_prefix#g" "$openssl_prefix"/lib/pkgconfig/*.pc
  for lib in libcrypto.3.dylib libssl.3.dylib libcrypto.a libssl.a; do
    lipo -create "$openssl_prefix-arm64/lib/$lib" "$openssl_prefix-x86_64/lib/$lib" \
      -output "$openssl_prefix/lib/$lib"
  done
  ln -sf libcrypto.3.dylib "$openssl_prefix/lib/libcrypto.dylib"
  ln -sf libssl.3.dylib "$openssl_prefix/lib/libssl.dylib"
  install_name_tool -id "$openssl_prefix/lib/libcrypto.3.dylib" "$openssl_prefix/lib/libcrypto.3.dylib"
  install_name_tool -id "$openssl_prefix/lib/libssl.3.dylib" "$openssl_prefix/lib/libssl.3.dylib"
  install_name_tool -change "$openssl_prefix-arm64/lib/libcrypto.3.dylib" \
    "$openssl_prefix/lib/libcrypto.3.dylib" "$openssl_prefix/lib/libssl.3.dylib"
  install_name_tool -change "$openssl_prefix-x86_64/lib/libcrypto.3.dylib" \
    "$openssl_prefix/lib/libcrypto.3.dylib" "$openssl_prefix/lib/libssl.3.dylib"
  codesign --force --sign - "$openssl_prefix/lib/libcrypto.3.dylib" "$openssl_prefix/lib/libssl.3.dylib"
  touch "$openssl_prefix/.complete"
fi

static_prefix="$deps/static"
cmake_dep() {
  local name="$1" tree="$2" arch prefix lib
  shift 2
  for arch in "${archs[@]}"; do
    log "$name (static, $arch)"
    prefix="$deps/static-$arch"
    cmake -S "$tree" -B "$tree/_build-$arch" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES="$arch" \
      -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
      -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
      -DBUILD_SHARED_LIBS=OFF "$@" >"$tree/_build-$arch.log" 2>&1 \
      && cmake --build "$tree/_build-$arch" >>"$tree/_build-$arch.log" 2>&1 \
      && cmake --install "$tree/_build-$arch" >>"$tree/_build-$arch.log" 2>&1 \
      || { tail -n 40 "$tree/_build-$arch.log" >&2; exit 1; }
  done
  mkdir -p "$static_prefix/lib"
  cp -R "$deps/static-arm64/include" "$static_prefix/"
  for lib in "$deps/static-arm64"/lib/*.a; do
    lipo -create "$lib" "$deps/static-x86_64/lib/$(basename "$lib")" \
      -output "$static_prefix/lib/$(basename "$lib")"
  done
  if [ -d "$deps/static-arm64/lib/pkgconfig" ]; then
    mkdir -p "$static_prefix/lib/pkgconfig"
    for pc in "$deps/static-arm64"/lib/pkgconfig/*.pc; do
      sed "s#$deps/static-arm64#$static_prefix#g" "$pc" >"$static_prefix/lib/pkgconfig/$(basename "$pc")"
    done
  fi
}
if [ ! -f "$static_prefix/.webp-$webp_version" ]; then
  fetch "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$webp_version.tar.gz" \
    "libwebp-$webp_version.tar.gz" "$webp_sha256"
  tree="$work/obj/libwebp"
  fm_safe_rm_tree "$work" "$tree"; mkdir -p "$tree"
  tar -xzf "$dl/libwebp-$webp_version.tar.gz" -C "$tree" --strip-components=1
  cmake_dep "libwebp $webp_version" "$tree" -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF \
    -DWEBP_BUILD_DWEBP=OFF -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF \
    -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF -DWEBP_BUILD_WEBPMUX=OFF \
    -DWEBP_BUILD_EXTRAS=OFF -DWEBP_USE_THREAD=ON
  touch "$static_prefix/.webp-$webp_version"
fi
if [ ! -f "$static_prefix/.nghttp3-$nghttp3_version" ]; then
  fetch "https://github.com/ngtcp2/nghttp3/releases/download/v$nghttp3_version/nghttp3-$nghttp3_version.tar.xz" \
    "nghttp3-$nghttp3_version.tar.xz" "$nghttp3_sha256"
  tree="$work/obj/nghttp3"
  fm_safe_rm_tree "$work" "$tree"; mkdir -p "$tree"
  tar -xJf "$dl/nghttp3-$nghttp3_version.tar.xz" -C "$tree" --strip-components=1
  cmake_dep "nghttp3 $nghttp3_version" "$tree" -DENABLE_LIB_ONLY=ON \
    -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON -DBUILD_TESTING=OFF
  touch "$static_prefix/.nghttp3-$nghttp3_version"
fi
for lib in "$static_prefix"/lib/*.a; do lipo "$lib" -verify_arch arm64 x86_64; done

cores="$(sysctl -n hw.ncpu)"
mem_mb=$(( $(sysctl -n hw.memsize) / 1048576 ))
jobs="${FORKMESH_JOBS:-$(( (mem_mb - 2048) / 2048 ))}"
[ "$jobs" -lt 1 ] && jobs=1
[ "$jobs" -gt "$cores" ] && jobs="$cores"

log "configure $src (commit $commit, macOS $deployment_target, $jobs jobs)"
PKG_CONFIG_PATH="$static_prefix/lib/pkgconfig" cmake -S "$src/desktop" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="$deployment_target" \
  -DFORKMESH_BUILD_TESTS=OFF \
  -DFORKMESH_REQUIRE_OPENSSL_3_5=ON \
  -DFORKMESH_BUILD_COMMIT_OVERRIDE="$commit" \
  -DFORKMESH_HEAVY_JOBS_OVERRIDE="$jobs" \
  -DCMAKE_PREFIX_PATH="$qt_prefix;$static_prefix" \
  -DCMAKE_FIND_FRAMEWORK=LAST \
  -DOPENSSL_ROOT_DIR="$openssl_prefix" \
  -DOPENSSL_USE_STATIC_LIBS=OFF

log "build"
ninja -C "$build" -k 0 -j "$jobs" forkmesh forkmesh-term

log "install + package"
fm_safe_rm_tree "$work" "$stage"
cmake --install "$build" --prefix "$stage"
version="$(cat "$build/forkmesh-version.txt")"
mkdir -p "$dist"
FORKMESH_PKG_VERSION="$version" FORKMESH_PKG_ARCH=universal \
MACDEPLOYQT="$qt_prefix/bin/macdeployqt" \
OPENSSL_ROOT_DIR="$openssl_prefix" \
FORKMESH_PKG_FOREIGN_PREFIXES="$work:$qt_root" \
  "$(dirname "${BASH_SOURCE[0]}")/macos.sh" "$stage/ForkMesh.app" "$dist"

dmg="$dist/forkmesh-macos-universal.dmg"
final="$dist/ForkMesh-$version-macos-universal.dmg"
mv "$dmg" "$final"

log "verify $final"
mnt="$(mktemp -d)"
hdiutil attach -nobrowse -readonly -mountpoint "$mnt" "$final" >/dev/null
trap 'hdiutil detach "$mnt" >/dev/null 2>&1 || true' EXIT
app="$mnt/ForkMesh.app"
codesign --verify --deep --strict "$app"
bad=0
while IFS= read -r -d '' f; do
  lipo -archs "$f" >/dev/null 2>&1 || continue
  if ! lipo "$f" -verify_arch arm64 x86_64 2>/dev/null; then
    echo "not universal: ${f#"$mnt"/} ($(lipo -archs "$f" 2>/dev/null))" >&2
    bad=1
  fi
  refs="$( { otool -L "$f" | tail -n +2 | awk '{print $1}'; otool -l "$f" | awk '/cmd LC_RPATH/{getline; getline; print $2}'; } \
    | grep -E "^(/opt/homebrew|/usr/local|$work|$qt_root)" || true)"
  if [ -n "$refs" ]; then
    echo "foreign reference in ${f#"$mnt"/}: $refs" >&2
    bad=1
  fi
done < <(find "$app/Contents" -type f -print0)
[ "$bad" = 0 ]
/usr/libexec/PlistBuddy -c "Print :LSMinimumSystemVersion" "$app/Contents/Info.plist" 2>/dev/null || true
vtool -show-build "$app/Contents/MacOS/ForkMesh" | awk '/minos/{print "minos", $2}'
QT_QPA_PLATFORM=offscreen "$app/Contents/MacOS/ForkMesh" --version
shasum -a 256 "$final"
echo "$final"
