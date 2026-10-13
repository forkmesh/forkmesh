#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../../../desktop/src" && pwd)
work=$(mktemp -d)
trap 'rm -f "$work"/*; rmdir "$work"' EXIT
moc=/usr/lib/qt6/libexec/moc
for h in AgentRuntimeSession CodexAppServerSession AgentProcess; do
  "$moc" "$src/$h.h" -o "$work/moc_$h.cpp"
done
flags=$(pkg-config --cflags Qt6Core)
c++ -std=c++17 -fPIC -O1 $flags -I"$src" -o "$work/codex_golden" \
  "$here/codex_golden.cpp" "$work"/moc_*.cpp \
  "$src/CodexAppServerSession.cpp" "$src/AgentRuntimeSession.cpp" "$src/AgentProcess.cpp" \
  "$src/AgentJsonlFramer.cpp" "$src/AgentGuardrails.cpp" "$src/AgentResources.cpp" \
  "$src/AgentToolPolicy.cpp" "$src/AgentToolCatalog.cpp" "$src/VirtualMachineRuntime.cpp" \
  "$src/AgentCapabilityBroker.cpp" "$src/AgentCapabilityStore.cpp" \
  $(pkg-config --libs Qt6Core)
"$work/codex_golden" "$here/codex_scenarios.json" "$here/codex_golden.json"
