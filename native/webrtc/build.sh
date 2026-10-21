#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target="${1:-}"
case "$target" in
  ios|android) ;;
  *) echo "usage: native/webrtc/build.sh ios|android" >&2; exit 2 ;;
esac
# shellcheck disable=SC1091
source "$here/PIN"
work="${WEBRTC_WORK:-$PWD/webrtc-work}"
out="${WEBRTC_OUT:-$PWD/webrtc-out}"
mkdir -p "$work" "$out"
cd "$work"
if [ ! -d depot_tools ]; then
  git clone --depth 1 "$DEPOT_TOOLS_URL" depot_tools
fi
export PATH="$work/depot_tools:$PATH"
export DEPOT_TOOLS_UPDATE=0
if [ ! -f .gclient ]; then
  cat > .gclient <<GCLIENT
solutions = [{
  "name": "src",
  "url": "$WEBRTC_URL@$WEBRTC_COMMIT",
  "deps_file": "DEPS",
  "managed": False,
  "custom_deps": {},
}]
target_os = ["$target"]
GCLIENT
fi
gclient sync --no-history --shallow --with_branch_heads --revision "src@$WEBRTC_COMMIT" --force --reset -D
cd src
if [ "$(git rev-parse HEAD)" != "$WEBRTC_COMMIT" ]; then
  echo "libwebrtc checkout is not at the pinned commit $WEBRTC_COMMIT" >&2
  exit 1
fi
for patch in "$here"/patches/*.patch; do
  [ -e "$patch" ] || continue
  git apply --check "$patch"
  git apply "$patch"
done
gn_args='is_debug=false rtc_include_tests=false rtc_build_examples=false rtc_enable_protobuf=false use_rtti=true'
if [ "$target" = ios ]; then
  python3 tools_webrtc/ios/build_ios_libs.py \
    --build_config release \
    --arch device:arm64 simulator:arm64 simulator:x64 \
    --output-dir "$out/ios" \
    --extra-gn-args "$gn_args enable_dsyms=false enable_stripping=true"
  (cd "$out/ios" && rm -f WebRTC.xcframework.zip && zip -qry WebRTC.xcframework.zip WebRTC.xcframework)
  shasum -a 256 "$out/ios/WebRTC.xcframework.zip" | tee "$out/ios/WebRTC.xcframework.zip.sha256"
else
  python3 tools_webrtc/android/build_aar.py \
    --build-dir "$out/android-build" \
    --output "$out/android/libwebrtc.aar" \
    --arch arm64-v8a armeabi-v7a x86_64 \
    --extra-gn-args "$gn_args"
  sha256sum "$out/android/libwebrtc.aar" | tee "$out/android/libwebrtc.aar.sha256"
fi
echo "libwebrtc $WEBRTC_BRANCH ($WEBRTC_COMMIT) built for $target into $out"
