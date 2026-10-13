#!/usr/bin/env bash
# Parses every Swift file of the app target, the extension, Shared/ and the
# watch app and its widgets.
#
# Linux has no SwiftUI or UIKit, so these sources cannot be type-checked
# outside macOS; parsing still catches unbalanced braces, bad string literals
# and malformed declarations before a Codemagic build spends ten minutes
# finding them. ForkMeshCore is not covered here: `swift test` compiles it.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
swiftc="${SWIFTC:-$(command -v swiftc || true)}"
if [ -z "$swiftc" ] && [ -x "$HOME/.forkmesh/toolchain/swift/usr/bin/swiftc" ]; then
  swiftc="$HOME/.forkmesh/toolchain/swift/usr/bin/swiftc"
fi
[ -n "$swiftc" ] || { echo "swiftc not found; set SWIFTC or install the swift.org toolchain." >&2; exit 1; }

dirs=()
for dir in ForkMesh NotificationService Shared SharedSurfaces ForkMeshWatch ForkMeshWatchWidgets ForkMeshWidgets ForkMeshShare ForkMeshAutoFill; do
  [ -d "$root/$dir" ] && dirs+=("$root/$dir")
done
failed=0
count=0
while IFS= read -r -d '' file; do
  count=$((count + 1))
  if ! output="$("$swiftc" -frontend -parse "$file" 2>&1)" || [ -n "$output" ]; then
    printf '%s\n' "$output"
    failed=$((failed + 1))
  fi
done < <(find "${dirs[@]}" -name '*.swift' -print0 | sort -z)

echo "Parsed $count Swift files; $failed with errors."
[ "$failed" -eq 0 ]
