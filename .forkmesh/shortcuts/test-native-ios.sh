#!/usr/bin/env bash
# name: Test native app (iOS)
# description: Install the swift.org toolchain if needed, then build and test ForkMeshCore (everything in native/ios that is not a SwiftUI view), parse the SwiftUI sources and regenerate the Xcode project. The app itself only compiles on macOS: that is Codemagic's ios-native-build.
set -euo pipefail

readonly SWIFT_VERSION="6.3"
readonly SWIFT_PLATFORM="debian12"
readonly SWIFT_HOME="$HOME/.forkmesh/toolchain/swift"
readonly APP_ROOT="$(cd "$(dirname "$0")/../.." && pwd)/native/ios"
# Build output stays out of the worktree and off the disk.
readonly SCRATCH="${FORKMESH_SWIFT_SCRATCH:-/tmp/fm-swift/shortcut}"

die() {
  echo "$*" >&2
  exit 1
}

[ "$(uname -s)" = "Linux" ] || [ -x "$(command -v swift || true)" ] || die "Install Xcode, then run: cd native/ios/ForkMeshCore && swift test"

if [ "$(uname -s)" = "Linux" ] && [ ! -x "$SWIFT_HOME/usr/bin/swift" ]; then
  url="https://download.swift.org/swift-${SWIFT_VERSION}-release/${SWIFT_PLATFORM}/swift-${SWIFT_VERSION}-RELEASE/swift-${SWIFT_VERSION}-RELEASE-${SWIFT_PLATFORM}.tar.gz"
  echo "Installing Swift ${SWIFT_VERSION} into $SWIFT_HOME (about 1 GB)..."
  mkdir -p "$SWIFT_HOME"
  curl -sSfL --retry 3 "$url" | tar xz --strip-components=1 -C "$SWIFT_HOME"
fi
[ -x "$SWIFT_HOME/usr/bin/swift" ] && PATH="$SWIFT_HOME/usr/bin:$PATH"
swift --version

cd "$APP_ROOT/ForkMeshCore"
swift test --scratch-path "$SCRATCH" 2>&1 | grep -v -E '^Test Case .* (started|passed)' | tail -n 40

"$APP_ROOT/tools/check_swift_syntax.sh"

if ruby -e 'require "xcodeproj"' 2>/dev/null; then
  ruby "$APP_ROOT/tools/generate_project.rb"
else
  echo "Skipping the Xcode project: gem install --user-install xcodeproj"
fi
