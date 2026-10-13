#!/usr/bin/env bash
set -euo pipefail
FORKMESH_LAUNCH_DIR="${FORKMESH_START_DIRECTORY:-$PWD}"
export FORKMESH_START_DIRECTORY="$FORKMESH_LAUNCH_DIR"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${FORKMESH_GO_NEW_BUILD_ROOT:-$HOME/projects/forkmesh/builds/desktop-go-new}"
BUILD="${FORKMESH_GO_NEW_BUILD_DIR:-$ROOT/build}"

launch() {
  if [ ! -x "$BUILD/forkmesh" ]; then
    echo "run.sh: $BUILD/forkmesh is missing; run $0 build first" >&2
    exit 1
  fi
  if [ -x "$BUILD/forkmesh-core" ]; then
    export FORKMESH_CORE_BIN="$BUILD/forkmesh-core"
  fi
  exec "$BUILD/forkmesh" "$@"
}

case "${1:-run}" in
  run)
    "$HERE/desktop-overlay/build.sh"
    launch "${@:2}"
    ;;
  build)
    exec "$HERE/desktop-overlay/build.sh" "${@:2}"
    ;;
  start)
    launch "${@:2}"
    ;;
  test)
    cd "$HERE"
    exec env -u FORKMESH_HOME -u FORKMESH_PROJECTS_DIR -u FORKMESH_DATA_DIR \
      -u FORKMESH_CONFIG_DIR -u FORKMESH_PROJECT -u FORKMESH_PROJECT_DIR go test ./... "${@:2}"
    ;;
  *)
    echo "Usage: $0 [run|build|start|test]" >&2
    echo "  run    stage, build and launch the desktop with forkmesh-core (default)" >&2
    echo "  build  stage and build only" >&2
    echo "  start  launch the last build without rebuilding" >&2
    echo "  test   go test ./... with the ForkMesh path variables dropped" >&2
    exit 2
    ;;
esac
