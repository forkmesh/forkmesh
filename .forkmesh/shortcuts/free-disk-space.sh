#!/usr/bin/env bash
# name: Free disk space
# description: Delete agent scratch, build and test leftovers whose worktree is gone, stranded Actions sandboxes, old Claude versions, backups beyond the 3 newest, and the pip/npm caches; anything in use or changed in the last hour is kept.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tool=(python3 "$repo/tools/free_disk_space.py")
if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
  exec "${tool[@]}" --help
fi
mode=(--execute)
rest=()
for arg in "$@"; do
  if [ "$arg" = "--dry-run" ]; then
    mode=()
  else
    rest+=("$arg")
  fi
done
set -- ${rest[@]+"${rest[@]}"}
sweep() { "${tool[@]}" ${mode[@]+"${mode[@]}"} "$@"; }

# step: Measure the disk
echo "::step::Measure the disk"
sweep --step usage "$@"

# step: Remove agent leftovers
echo "::step::Remove agent leftovers"
sweep --step agents "$@"

# step: Remove test and Actions leftovers
echo "::step::Remove test and Actions leftovers"
sweep --step tests "$@"

# step: Prune old Claude versions, backups and Trash
echo "::step::Prune old Claude versions, backups and Trash"
sweep --step versions "$@"

# step: Clear the pip and npm caches
echo "::step::Clear the pip and npm caches"
sweep --step caches "$@"

# step: Report what is left
echo "::step::Report what is left"
sweep --step report "$@"
