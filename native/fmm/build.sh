#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
target="${1:-}"
case "$target" in
  ios|android) ;;
  *) echo "usage: native/fmm/build.sh ios|android" >&2; exit 2 ;;
esac
# shellcheck disable=SC1091
source "$here/PIN"
# shellcheck disable=SC1091
source "$here/../../tools/forkmesh-safe-rm.sh"
work="${FMM_WORK:-$here/build/work}"
out="${FMM_OUT:-$here/build/out}"
mkdir -p "$work" "$out"
work="$(cd "$work" && pwd -P)"
out="$(cd "$out" && pwd -P)"
export LC_ALL=C TZ=UTC ZERO_AR_DATE=1 SOURCE_DATE_EPOCH=0

fail() {
  echo "$*" >&2
  exit 1
}

sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi
}

host_os="$(uname -s | tr '[:upper:]' '[:lower:]')"
case "$(uname -m)" in
  x86_64|amd64) host_arch=amd64 ;;
  arm64|aarch64) host_arch=arm64 ;;
  *) fail "unsupported host $(uname -m)" ;;
esac

go_matches() {
  [ -x "$1" ] && [ "$(GOTOOLCHAIN=local "$1" env GOVERSION 2>/dev/null)" = "go$GO_VERSION" ]
}

pick_go() {
  local candidate
  for candidate in "${FMM_GO:-}" "$(command -v go 2>/dev/null || true)" "$work/go-$GO_VERSION/bin/go"; do
    [ -n "$candidate" ] || continue
    if go_matches "$candidate"; then
      echo "$candidate"
      return
    fi
  done
  local sum_var="GO_SHA256_${host_os}_${host_arch}"
  local want="${!sum_var:-}"
  [ -n "$want" ] || fail "no pinned Go archive for $host_os/$host_arch"
  local archive="$work/go$GO_VERSION.$host_os-$host_arch.tar.gz"
  if [ ! -f "$archive" ] || [ "$(sha256 "$archive" | cut -d' ' -f1)" != "$want" ]; then
    curl -fsSL --retry 5 -o "$archive.part" "$GO_URL_BASE/go$GO_VERSION.$host_os-$host_arch.tar.gz"
    mv "$archive.part" "$archive"
  fi
  [ "$(sha256 "$archive" | cut -d' ' -f1)" = "$want" ] || fail "$archive is not the pinned Go $GO_VERSION archive"
  fm_safe_rm_tree "$work" "$work/go-$GO_VERSION" "$work/go-unpack"
  mkdir -p "$work/go-unpack"
  tar -xzf "$archive" -C "$work/go-unpack"
  mv "$work/go-unpack/go" "$work/go-$GO_VERSION"
  fm_safe_rm_tree "$work" "$work/go-unpack"
  go_matches "$work/go-$GO_VERSION/bin/go" || fail "the unpacked Go is not $GO_VERSION"
  echo "$work/go-$GO_VERSION/bin/go"
}

gobin="$(pick_go)"
export GOROOT="$(GOTOOLCHAIN=local "$gobin" env GOROOT)"
export PATH="$work/bin:$GOROOT/bin:$PATH"
export GOTOOLCHAIN=local GOFLAGS=-mod=readonly GOWORK=off
export GOCACHE="${FMM_GOCACHE:-$work/gocache}"
export TMPDIR="$work/tmp"
mkdir -p "$TMPDIR" "$work/bin"

cd "$here"
grep -qxF "golang.org/x/mobile $X_MOBILE_VERSION $X_MOBILE_SUM" go.sum || fail "go.sum does not pin golang.org/x/mobile $X_MOBILE_VERSION $X_MOBILE_SUM"
grep -qxF "golang.org/x/mobile $X_MOBILE_VERSION/go.mod $X_MOBILE_MOD_SUM" go.sum || fail "go.sum does not pin the golang.org/x/mobile go.mod"
[ "$(go list -m -f '{{.Version}}' golang.org/x/mobile)" = "$X_MOBILE_VERSION" ] || fail "go.mod does not require golang.org/x/mobile $X_MOBILE_VERSION"
go mod download golang.org/x/mobile
go mod verify
go build -trimpath -o "$work/bin/" golang.org/x/mobile/cmd/gomobile golang.org/x/mobile/cmd/gobind

stamp() {
  {
    echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "go=$(go env GOVERSION)"
    echo "x_mobile=$X_MOBILE_VERSION"
    echo "source=$(git -C "$here" rev-parse HEAD 2>/dev/null || echo unknown)$(git -C "$here" diff --quiet -- ../../server 2>/dev/null || echo -dirty)"
  } > "$1/BUILD"
  cp "$here/PIN" "$1/PIN"
}

case "$target" in
  ios)
    [ "$host_os" = darwin ] || fail "the iOS framework is built on macOS with Xcode"
    dest="$out/ios"
    fm_safe_rm_tree "$out" "$dest"
    mkdir -p "$dest"
    gomobile bind -target "$IOS_TARGETS" -iosversion "$IOS_DEPLOYMENT_TARGET" -trimpath -ldflags="-s -w" \
      -o "$dest/Fmm.xcframework" "$FMM_PACKAGE"
    stamp "$dest"
    (cd "$dest" && find Fmm.xcframework -type f | sort | while read -r f; do sha256 "$f"; done > SHA256SUMS)
    echo "slices (archive bytes; __TEXT and __DATA of the objects):"
    for lib in "$dest"/Fmm.xcframework/*/Fmm.framework/Fmm; do
      slice="$(basename "$(dirname "$(dirname "$lib")")")"
      for arch in $(lipo -archs "$lib"); do
        thin="$work/tmp/fmm-$slice-$arch.a"
        lipo "$lib" -thin "$arch" -output "$thin" 2>/dev/null || cp "$lib" "$thin"
        echo "  $slice $arch: $(wc -c < "$thin" | tr -d ' ') bytes, $(xcrun size "$thin" | awk 'NR>1 {t+=$1; d+=$2} END {print t " + " d}')"
        rm -f "$thin"
      done
    done
    probe="$work/tmp/probe"
    mkdir -p "$probe"
    printf '@import Foundation;\n#import <Fmm/Fmm.h>\nint main(void) { FmmmobileCall *c = [[FmmmobileCall alloc] init:[[FmmmobileConfig alloc] init] l:nil]; NSLog(@"%%@", c); return 0; }\n' > "$probe/with.m"
    printf '@import Foundation;\nint main(void) { NSLog(@"%%@", [NSDate date]); return 0; }\n' > "$probe/without.m"
    for m in with without; do
      xcrun -sdk iphoneos clang -fmodules -w -arch arm64 -miphoneos-version-min="$IOS_DEPLOYMENT_TARGET" \
        -F "$dest/Fmm.xcframework/ios-arm64" $([ "$m" = with ] && echo "-framework Fmm -lresolv") -framework Foundation \
        -dead_strip "$probe/$m.m" -o "$probe/$m"
      xcrun strip -x "$probe/$m"
    done
    echo "linked into a stripped arm64 device executable: $(($(wc -c < "$probe/with") - $(wc -c < "$probe/without"))) bytes added"
    xcrun size -m "$probe/with" | awk '/^Segment __(TEXT|DATA_CONST|DATA):/ {print "  " $0}'
    ;;
  android)
    ndk="${ANDROID_NDK_HOME:-${ANDROID_HOME:-$HOME/Android/Sdk}/ndk/$ANDROID_NDK_VERSION}"
    [ -f "$ndk/source.properties" ] || fail "no NDK at $ndk"
    grep -q "Pkg.Revision = $ANDROID_NDK_VERSION" "$ndk/source.properties" || fail "$ndk is not NDK $ANDROID_NDK_VERSION"
    export ANDROID_HOME="${ANDROID_HOME:-$(cd "$ndk/../.." && pwd)}" ANDROID_NDK_HOME="$ndk"
    command -v javac >/dev/null 2>&1 || { [ -n "${JAVA_HOME:-}" ] && export PATH="$JAVA_HOME/bin:$PATH"; }
    command -v javac >/dev/null 2>&1 || fail "javac not found; set JAVA_HOME"
    dest="$out/android"
    fm_safe_rm_tree "$out" "$dest"
    mkdir -p "$dest"
    gomobile bind -target "$ANDROID_TARGETS" -androidapi "$ANDROID_API" -javapkg "$ANDROID_JAVA_PACKAGE" -trimpath -ldflags="-s -w" \
      -o "$dest/fmm.aar" "$FMM_PACKAGE"
    stamp "$dest"
    (cd "$dest" && sha256 fmm.aar fmm-sources.jar 2>/dev/null > SHA256SUMS || true)
    echo "per-ABI libgojni.so bytes (uncompressed, then compressed in the aar):"
    unzip -v "$dest/fmm.aar" 'jni/*' | awk '/libgojni.so/ {print "  " $8 ": " $1 " / " $3}'
    ;;
esac
fm_safe_rm_tree "$work" "$TMPDIR"
echo "done: $dest"
