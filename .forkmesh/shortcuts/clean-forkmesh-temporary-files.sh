#!/usr/bin/env bash
# name: Clean ForkMesh temporary files
# description: Move stale ForkMesh build, test, packaging, and diagnostic artifacts out of /tmp in one safe pass.
set -euo pipefail

temporary_root="${TMPDIR:-/tmp}"
minimum_age_minutes=5

usage() {
  cat <<'USAGE'
Usage: clean-forkmesh-temporary-files.sh [--include-recent] [--root /tmp/path]

Moves recognized ForkMesh/Codex build, test, packaging, and diagnostic
artifacts to the desktop Trash. Unrelated temporary files and live application
sockets are left alone. By default, files touched in the last five minutes are
also left alone so an active build is not interrupted.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --include-recent)
      minimum_age_minutes=0
      shift
      ;;
    --root)
      [ "$#" -ge 2 ] || { echo "--root needs a path" >&2; exit 2; }
      temporary_root="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[ -d "$temporary_root" ] || {
  echo "Temporary root does not exist: $temporary_root" >&2
  exit 1
}

temporary_root="$(realpath -e -- "$temporary_root")"
case "$temporary_root" in
  /tmp|/tmp/*) ;;
  *)
    echo "Refusing to clean outside /tmp: $temporary_root" >&2
    exit 1
    ;;
esac

command -v gio >/dev/null 2>&1 || {
  echo "gio is required so cleanup remains recoverable through Trash." >&2
  exit 1
}

# Some Linux desktops deliberately reject `gio trash` for /tmp even when the
# user's Trash is on the same filesystem. Keep the operation recoverable by
# falling back to the FreeDesktop Trash layout instead of permanently deleting
# anything. The matching .trashinfo file lets file managers restore the item.
trash_root="${XDG_DATA_HOME:-$HOME/.local/share}/Trash"
trash_files="$trash_root/files"
trash_info="$trash_root/info"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
gio_usable=true
fallback_moved=0

move_to_trash() {
  local path="$1" name destination info_name escaped_path suffix=0
  if [ "$gio_usable" = true ]; then
    if gio trash -- "$path" 2>/dev/null; then
      return 0
    fi
    gio_usable=false
    echo "Desktop Trash API does not support $temporary_root; using its recoverable Trash layout."
  fi

  mkdir -p -m 700 "$trash_files" "$trash_info"
  name="forkmesh-cleanup-$run_id-${path##*/}"
  destination="$trash_files/$name"
  while [ -e "$destination" ] || [ -L "$destination" ]; do
    suffix=$((suffix + 1))
    destination="$trash_files/$name-$suffix"
  done
  name="${destination##*/}"
  info_name="$trash_info/$name.trashinfo"
  mv -- "$path" "$destination"
  escaped_path="${path//%/%25}"
  escaped_path="${escaped_path// /%20}"
  {
    echo '[Trash Info]'
    printf 'Path=%s\n' "$escaped_path"
    printf 'DeletionDate=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%S)"
  } >"$info_name"
  fallback_moved=$((fallback_moved + 1))
}

is_forkmesh_artifact() {
  local path="$1" name="${1##*/}"
  case "$name" in
    forkmesh-single-instance|forkmesh-single-instance-*)
      # Preserve the desktop's live single-instance listener; every open
      # project has one of its own, suffixed with its name. A socket file
      # left behind after the process exits is safe to clean on the next run.
      if [ -S "$path" ] && command -v ss >/dev/null 2>&1 &&
         ! ss -H -xl 2>/dev/null | grep -Fq -- "$path"; then
        return 0
      fi
      return 1
      ;;
    forkmesh-*|fm-*|fmmcp-*|qt_temp-*|WindowResize-qt_temp-*|pytest-of-*|\
    appimage_extracted_*|playwright-transform-cache-*|codex-clipboard-*|\
    claude-[0-9]*|claude-*-cwd|cc*.s|GMfifo*|hsperfdata_*|\
    mobile_build_*|verify_guard_*|kotlin-daemon.*.log|aab-*.log|apk-*.log|\
    catalog-probe*|mirror35-*|mirror36-*|mirror37-*|\
    about.json|live-status.json|mirrors.json|projects-tree.json|\
    live-dashboard.js|resp.body|missing.out|status.json|status-headers|\
    status-h-now|status-now.json|analysis.md|xcpriv.txt|bgk.bak|\
    gradle.py|gradle.bak|wf.py|doc.py|doc2.py|tc.log|installer-check.*|\
    ws_head.txt|qterr.txt|qtout.txt|agent[0-9]*-*.log|\
    agent[0-9]*-*.txt|banner-after-app)
      return 0
      ;;
    tmp.*)
      # ForkMesh tests use either a replica response fixture or a ready/pid/FIFO
      # triplet. Do not treat an arbitrary generically named directory as ours.
      if [ -f "$path/replica.json" ] ||
         { [ -f "$path/ready" ] && [ -f "$path/pid" ] && [ -p "$path/go" ]; }; then
        return 0
      fi
      return 1
      ;;
    tmpt*)
      # QTemporaryDir socket fixtures contain only the local test socket.
      if [ -d "$path" ] &&
         [ "$(find "$path" -mindepth 1 -maxdepth 1 | wc -l)" -eq 1 ] &&
         find "$path" -mindepth 1 -maxdepth 1 -type s -print -quit | grep -q .; then
        return 0
      fi
      return 1
      ;;
    *)
      return 1
      ;;
  esac
}

candidates=()
while IFS= read -r -d '' path; do
  is_forkmesh_artifact "$path" || continue
  if [ "$minimum_age_minutes" -gt 0 ] &&
     find "$path" -maxdepth 0 -mmin "-$minimum_age_minutes" -print -quit |
       grep -q .; then
    echo "Skipping recently active artifact: $path"
    continue
  fi
  candidates+=("$path")
done < <(find "$temporary_root" -mindepth 1 -maxdepth 1 -user "$(id -u)" -print0)

if [ "${#candidates[@]}" -eq 0 ]; then
  echo "No stale ForkMesh temporary artifacts found in $temporary_root."
  exit 0
fi

total_bytes=0
for path in "${candidates[@]}"; do
  bytes="$(du -sb -- "$path" 2>/dev/null | awk '{print $1}')"
  total_bytes=$((total_bytes + ${bytes:-0}))
done

moved=0
for path in "${candidates[@]}"; do
  case "$path" in
    "$temporary_root"/*) ;;
    *)
      echo "Refusing unexpected cleanup path: $path" >&2
      exit 1
      ;;
  esac
  move_to_trash "$path"
  moved=$((moved + 1))
done

printf 'Moved %d ForkMesh temporary artifacts (%s bytes) to Trash.\n' \
  "$moved" "$total_bytes"
echo "Unrelated temporary files and live application sockets were preserved."
if [ "$fallback_moved" -gt 0 ]; then
  echo "$fallback_moved item(s) used the recoverable FreeDesktop Trash fallback."
fi
