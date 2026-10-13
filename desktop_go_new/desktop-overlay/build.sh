#!/usr/bin/env bash
set -euo pipefail
OVERLAY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${FORKMESH_GO_NEW_BUILD_ROOT:-$HOME/projects/forkmesh/builds/desktop-go-new}"
BUILD="${FORKMESH_GO_NEW_BUILD_DIR:-$ROOT/build}"
STAGE="$("$OVERLAY/stage.sh")"
mkdir -p "$ROOT/tmp"
launcher=()
if command -v ccache >/dev/null 2>&1; then launcher=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache); fi
if [ ! -f "$BUILD/build.ninja" ]; then
  TMPDIR="$ROOT/tmp" cmake -S "$STAGE/desktop" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS_DEBUG="-O0 -g0" "${launcher[@]}" "$@"
fi
TMPDIR="$ROOT/tmp" cmake --build "$BUILD" --target forkmesh -j"${FORKMESH_GO_NEW_JOBS:-8}"
echo "built $BUILD/forkmesh and $BUILD/forkmesh-core"
