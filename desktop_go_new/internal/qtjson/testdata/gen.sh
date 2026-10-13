#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../../../desktop/src" && pwd)
work=$(mktemp -d)
trap 'rm -f "$work"/golden "$work"/*.o; rmdir "$work"' EXIT
flags=$(pkg-config --cflags Qt6Core)
c++ -std=c++17 -fPIC -O1 $flags -I"$src" -o "$work/golden" \
  "$here/golden.cpp" "$src/AgentStore.cpp" "$src/CoreAgentStore.cpp" "$src/AgentLogText.cpp" "$src/AgentRuntimeSession.cpp" "$src/AppPaths.cpp" \
  $(pkg-config --libs Qt6Core)
"$work/golden" "$here/golden.json"
