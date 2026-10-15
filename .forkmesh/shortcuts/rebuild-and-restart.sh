#!/usr/bin/env bash
# name: Rebuild & restart
# description: Rebuild the desktop app and the Go background engine (forkmesh-core) from this checkout and relaunch, logging every phase with timings and the modules that were compiled.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
exec python3 "$repo/tools/rebuild_restart_log.py" "$@"
