#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../../../desktop/src" && pwd)
work=$(mktemp -d)
trap 'rm -f "$work"/golden; rmdir "$work"' EXIT
flags=$(pkg-config --cflags Qt6Core Qt6Network)
c++ -std=c++20 -fPIC -O1 $flags -I"$src" -o "$work/golden" \
  "$here/golden.cpp" "$src/AgentGuardrailLedger.cpp" "$src/AgentToolCatalog.cpp" \
  "$src/AgentToolEfficiencyLedger.cpp" "$src/AgentToolImprovementLog.cpp" "$src/AgentToolPolicy.cpp" \
  "$src/BackgroundFileWriter.cpp" "$src/LocalControlInsights.cpp" \
  $(pkg-config --libs Qt6Core Qt6Network)
"$work/golden" "$here/golden.json"
