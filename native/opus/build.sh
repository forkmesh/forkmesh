#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target="${1:-}"
case "$target" in
  ios|android|host) ;;
  *) echo "usage: native/opus/build.sh ios|android|host" >&2; exit 2 ;;
esac
# shellcheck disable=SC1091
source "$here/PIN"
# shellcheck disable=SC1091
source "$here/../../tools/forkmesh-safe-rm.sh"
work="${OPUS_WORK:-$here/build/work}"
out="${OPUS_OUT:-$here/build/out}"
source_kind="${OPUS_SOURCE:-git}"
features="${OPUS_BUILD_FEATURES:-$OPUS_FEATURES}"
[ "$features" != none ] || features=""
for f in $features; do
  case "$f" in deep_plc|dred|osce) ;; *) echo "unknown Opus feature $f (deep_plc, dred, osce or none)" >&2; exit 2 ;; esac
done
mkdir -p "$work" "$out"
work="$(cd "$work" && pwd -P)"
out="$(cd "$out" && pwd -P)"
export LC_ALL=C
export ZERO_AR_DATE=1
export SOURCE_DATE_EPOCH=0
export TZ=UTC

if command -v nproc >/dev/null 2>&1; then jobs="$(nproc)"; else jobs="$(sysctl -n hw.ncpu)"; fi

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

fail() {
  echo "$*" >&2
  exit 1
}

fetch_tag() {
  local dir="$1" url="$2" tag="$3" tag_object="$4" commit="$5" allowed="${6:-/dev/null}"
  [ -d "$dir/.git" ] || git init -q "$dir"
  local attempt=0
  if [ "$(git -C "$dir" rev-parse -q --verify "refs/tags/$tag" || true)" != "$tag_object" ]; then
    until git -C "$dir" fetch -q --depth 1 "$url" "+refs/tags/$tag:refs/tags/$tag"; do
      attempt=$((attempt + 1))
      [ "$attempt" -lt 5 ] || fail "could not fetch $tag from $url"
      sleep $((attempt * 15))
    done
  fi
  [ "$(git -C "$dir" rev-parse "refs/tags/$tag")" = "$tag_object" ] \
    || fail "$url $tag is not the pinned tag object $tag_object"
  [ "$(git -C "$dir" rev-parse "refs/tags/$tag^{commit}")" = "$commit" ] \
    || fail "$url $tag does not point at the pinned commit $commit"
  local changes
  changes="$(git -C "$dir" status --porcelain --untracked-files=all | grep -v -x -F -f <(sed 's/^/?? /' "$allowed") || true)"
  [ -z "$changes" ] || fail "$dir has local changes; move it aside and rerun: $changes"
  git -C "$dir" checkout -q --detach "$commit"
  [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || fail "$dir is not at the pinned commit $commit"
}

fetch_opus() {
  if [ "$source_kind" = tarball ]; then
    src="$work/opus-$OPUS_VERSION-tarball"
    local tarball="$work/opus-$OPUS_VERSION.tar.gz"
    if [ ! -f "$tarball" ]; then
      curl -fsSL -o "$tarball.part" "$OPUS_TARBALL_URL"
      mv "$tarball.part" "$tarball"
    fi
    [ "$(sha256 "$tarball" | cut -d' ' -f1)" = "$OPUS_TARBALL_SHA256" ] \
      || fail "$tarball does not match the pinned SHA-256 $OPUS_TARBALL_SHA256"
    fm_safe_rm_tree "$work" "$src"
    mkdir -p "$src"
    tar -xzf "$tarball" -C "$src" --strip-components=1
  elif [ "$source_kind" = git ]; then
    src="$work/opus-$OPUS_VERSION"
    local model="$work/opus_data-$OPUS_MODEL_SHA256.tar.gz" listing="$work/opus_data-$OPUS_MODEL_SHA256.files"
    if [ ! -f "$model" ]; then
      curl -fsSL -o "$model.part" "$OPUS_MODEL_URL"
      mv "$model.part" "$model"
    fi
    [ "$(sha256 "$model" | cut -d' ' -f1)" = "$OPUS_MODEL_SHA256" ] \
      || fail "$model does not match the pinned SHA-256 $OPUS_MODEL_SHA256"
    tar -tzf "$model" | grep -v -e '^dnn/models/' -e '/$' | sort > "$listing"
    fetch_tag "$src" "$OPUS_URL" "$OPUS_TAG" "$OPUS_TAG_OBJECT" "$OPUS_COMMIT" "$listing"
    grep -q "download_model.sh \"$OPUS_MODEL_SHA256\"" "$src/autogen.sh" \
      || fail "$OPUS_COMMIT does not name the model $OPUS_MODEL_SHA256 in autogen.sh"
    tar --exclude 'dnn/models/*' -xzf "$model" -C "$src"
  else
    fail "OPUS_SOURCE must be git or tarball"
  fi
}

find_cmake() {
  if [ -n "${OPUS_CMAKE:-}" ]; then cmake="$OPUS_CMAKE"; return; fi
  if command -v cmake >/dev/null 2>&1; then cmake="$(command -v cmake)"; return; fi
  local cmake_src="$work/cmake-$CMAKE_VERSION" cmake_build="$work/cmake-$CMAKE_VERSION-build"
  local cmake_prefix="$work/cmake-$CMAKE_VERSION-install"
  cmake="$cmake_prefix/bin/cmake"
  if [ -x "$cmake" ] && "$cmake" --version | grep -q "cmake version $CMAKE_VERSION\$"; then return; fi
  fetch_tag "$cmake_src" "$CMAKE_URL" "$CMAKE_TAG" "$CMAKE_TAG_OBJECT" "$CMAKE_COMMIT"
  fm_safe_rm_tree "$work" "$cmake_build" "$cmake_prefix"
  mkdir -p "$cmake_build"
  (cd "$cmake_build" && "$cmake_src/bootstrap" --prefix="$cmake_prefix" --parallel="$jobs" \
    -- -DCMAKE_BUILD_TYPE=Release -DCMAKE_USE_OPENSSL=OFF -DBUILD_TESTING=OFF)
  make -C "$cmake_build" -j "$jobs" install
  "$cmake" --version | grep -q "cmake version $CMAKE_VERSION\$" || fail "the CMake build in $cmake_prefix is not $CMAKE_VERSION"
}

feature() {
  case " $features " in *" $1 "*) echo ON ;; *) echo OFF ;; esac
}

osce_bwe() {
  [ "$(feature osce)" = OFF ] || echo " -DENABLE_OSCE_BWE"
}

write_sums() {
  (cd "$dest" && find "$@" -type f ! -name '*.pc' | sort | while read -r f; do sha256 "$f"; done) > "$dest/SHA256SUMS"
}

build_one() {
  local name="$1"
  shift
  local bdir="$work/build-$name" prefix="$stage/$name"
  fm_safe_rm_tree "$work" "$bdir" "$prefix"
  "$cmake" -S "$src" -B "$bdir" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$work=work${prefix_maps:-}$(osce_bwe)" \
    -DOPUS_FIXED_POINT=OFF \
    -DOPUS_ENABLE_FLOAT_API=ON \
    -DOPUS_CUSTOM_MODES=OFF \
    -DOPUS_DEEP_PLC="$(feature deep_plc)" \
    -DOPUS_DRED="$(feature dred)" \
    -DOPUS_OSCE="$(feature osce)" \
    -DOPUS_HARDENING=ON \
    -DOPUS_ASSERTIONS=OFF \
    -DOPUS_BUILD_TESTING=OFF \
    -DOPUS_BUILD_PROGRAMS=OFF \
    -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF \
    "$@"
  "$cmake" --build "$bdir" -j "$jobs"
  "$cmake" --install "$bdir" --strip
}

build_host() {
  build_one host-static -DOPUS_BUILD_SHARED_LIBRARY=OFF
  build_one host-shared -DOPUS_BUILD_SHARED_LIBRARY=ON
  mkdir -p "$dest"
  cp -R "$stage/host-static/include" "$dest/"
  mkdir -p "$dest/lib/pkgconfig"
  cp "$stage/host-static/lib/libopus.a" "$dest/lib/"
  cp -P "$stage/host-shared/lib/"libopus.so* "$dest/lib/"
  sed "s#^prefix=.*#prefix=$dest#" "$stage/host-shared/lib/pkgconfig/opus.pc" > "$dest/lib/pkgconfig/opus.pc"
  write_sums include lib
}

build_android() {
  local ndk="${OPUS_ANDROID_NDK:-$HOME/Android/Sdk/ndk/$ANDROID_NDK_VERSION}"
  [ -f "$ndk/build/cmake/android.toolchain.cmake" ] || fail "no Android NDK at $ndk (set OPUS_ANDROID_NDK)"
  grep -q "^Pkg.Revision *= *$ANDROID_NDK_VERSION\$" "$ndk/source.properties" \
    || fail "$ndk is not NDK $ANDROID_NDK_VERSION"
  ndk="$(cd "$ndk" && pwd -P)"
  prefix_maps=" -ffile-prefix-map=$ndk=ndk"
  local abi
  for abi in $ANDROID_ABIS; do
    local ndk_args=(-DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake"
      -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$ANDROID_API" -DANDROID_STL=none)
    [ "$abi" != armeabi-v7a ] || ndk_args+=(-DOPUS_PRESUME_NEON=ON)
    build_one "android-$abi-static" "${ndk_args[@]}" -DOPUS_BUILD_SHARED_LIBRARY=OFF
    build_one "android-$abi-shared" "${ndk_args[@]}" -DOPUS_BUILD_SHARED_LIBRARY=ON
    mkdir -p "$dest/lib/$abi"
    cp "$stage/android-$abi-static/lib/libopus.a" "$dest/lib/$abi/"
    cp "$stage/android-$abi-shared/lib/libopus.so" "$dest/lib/$abi/"
  done
  cp -R "$stage/android-arm64-v8a-static/include" "$dest/"
  write_sums include lib
}

build_ios() {
  command -v xcodebuild >/dev/null 2>&1 || fail "the iOS build needs macOS with Xcode"
  [ -x /usr/bin/python3 ] || fail "the iOS build needs /usr/bin/python3 (Xcode's command line tools)"
  local apple_args=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET"
    -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF)
  build_one ios-device-arm64 "${apple_args[@]}" -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64
  build_one ios-simulator-arm64 "${apple_args[@]}" -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64
  build_one ios-simulator-x86_64 "${apple_args[@]}" -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=x86_64
  /usr/bin/python3 -I "$here/ios/normalize.py" archive "$stage"/ios-device-arm64/lib/libopus.a \
    "$stage"/ios-simulator-arm64/lib/libopus.a "$stage"/ios-simulator-x86_64/lib/libopus.a
  local slices="$stage/ios-slices" headers="$stage/ios-headers"
  fm_safe_rm_tree "$work" "$slices" "$headers"
  mkdir -p "$slices/device" "$slices/simulator" "$headers/Opus"
  cp "$stage/ios-device-arm64/lib/libopus.a" "$slices/device/libopus.a"
  xcrun lipo -create "$stage/ios-simulator-arm64/lib/libopus.a" "$stage/ios-simulator-x86_64/lib/libopus.a" \
    -output "$slices/simulator/libopus.a"
  cp "$stage/ios-device-arm64/include/opus/"*.h "$headers/Opus/"
  cp "$here/ios/module.modulemap" "$here/ios/fm_opus.h" "$headers/Opus/"
  mkdir -p "$dest"
  xcodebuild -create-xcframework \
    -library "$slices/device/libopus.a" -headers "$headers" \
    -library "$slices/simulator/libopus.a" -headers "$headers" \
    -output "$dest/Opus.xcframework"
  /usr/bin/python3 -I "$here/ios/normalize.py" plist "$dest/Opus.xcframework/Info.plist"
  find "$dest/Opus.xcframework" -exec touch -h -t 200001010000 {} +
  (cd "$dest" && find Opus.xcframework | sort | zip -qX -@ Opus.xcframework.zip)
  write_sums Opus.xcframework Opus.xcframework.zip
}

find_cmake
fetch_opus
stage="$work/stage"
dest="$out/$target"
fm_safe_rm_tree "$out" "$dest"
"build_$target"
cp "$here/PIN" "$dest/PIN"
cat "$dest/SHA256SUMS"
echo "opus $OPUS_VERSION ($OPUS_COMMIT, from $source_kind, features: ${features:-none}) built for $target into $dest with $("$cmake" --version | head -1)"
