#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=$(cd "$here/../../../../desktop/src" && pwd)
work=$(mktemp -d)
trap 'rm -f "$work"/*; rmdir "$work"' EXIT
{
  echo '#include <QList>'
  echo '#include <QString>'
  echo '#include <QVector>'
  echo '#include <algorithm>'
  awk '/^struct MergeVelocity \{/,/^\};/' "$src/MainWindowInternal.h"
  for fn in 'QString mergedBranchFromMergeSubject' 'void mergeVelocityPeriodStats' 'MergeVelocity mergeVelocityForTimestamps'; do
    awk -v start="$fn" 'index($0, start) == 1 {on=1} on {print} on && /^}/ {on=0; print ""}' "$src/MainWindowInternal3.cpp"
  done
} > "$work/extracted.h"
flags=$(pkg-config --cflags Qt6Core)
c++ -std=c++17 -fPIC -O1 $flags -I"$src" -I"$work" -o "$work/ops_golden" \
  "$here/ops_golden.cpp" "$src/LocalControlAgentOps.cpp" "$src/AgentLogText.cpp" \
  $(pkg-config --libs Qt6Core)
"$work/ops_golden" "$here/ops_cases.json" "$here/ops_golden.json"
