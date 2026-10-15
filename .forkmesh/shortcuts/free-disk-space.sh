#!/usr/bin/env bash
# name: Free disk space
# description: Delete agent scratch, build and test leftovers whose worktree is gone, stranded Actions sandboxes, old Claude versions, backups beyond the 3 newest, and the pip/npm caches; anything in use or changed in the last hour is kept.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  exec python3 "$repo/tools/free_disk_space.py" --help
fi
if [ "${1:-}" = "--dry-run" ]; then
  shift
  exec python3 "$repo/tools/free_disk_space.py" "$@"
fi
exec python3 "$repo/tools/free_disk_space.py" --execute "$@"
