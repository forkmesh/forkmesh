#!/usr/bin/env bash
set -euo pipefail
OVERLAY="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$OVERLAY/../.." && pwd)"
ROOT="${FORKMESH_GO_NEW_BUILD_ROOT:-$HOME/projects/forkmesh/builds/desktop-go-new}"
STAGE="$ROOT/stage"
mkdir -p "$STAGE"
for dir in desktop marketplace www; do
  if [ -L "$STAGE/$dir" ]; then rm -f "$STAGE/$dir"; fi
  rsync -a --exclude '/build' --exclude '/build-*' --exclude '__pycache__' "$REPO/$dir" "$STAGE/"
done
for entry in "$REPO"/* "$REPO"/.forkmesh; do
  [ -e "$entry" ] || continue
  name="$(basename "$entry")"
  case "$name" in desktop|marketplace|www|desktop_go_new) continue ;; esac
  if [ -e "$STAGE/$name" ] && [ ! -L "$STAGE/$name" ]; then
    mv "$STAGE/$name" "$ROOT/stale-$name-$(date +%Y%m%d%H%M%S)"
  fi
  ln -sfn "$entry" "$STAGE/$name"
done
cp -a "$OVERLAY/files/." "$STAGE/"
for patch in "$OVERLAY"/patches/[0-9]*.patch; do
  (cd "$STAGE" && GIT_CEILING_DIRECTORIES="$ROOT" git apply --whitespace=nowarn "$patch") || {
    echo "stage: $(basename "$patch") does not apply to this checkout; rebase the overlay (see desktop_go_new/docs/README.md)" >&2
    exit 1
  }
done
ln -sfn "$REPO/desktop_go_new" "$STAGE/desktop_go_new"
echo "$STAGE"
