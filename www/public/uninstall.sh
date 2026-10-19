#!/usr/bin/env bash
# ForkMesh desktop client uninstaller.
#   curl -fsSL https://forkmesh.com/uninstall.sh | bash
#   curl -fsSL https://forkmesh.com/uninstall.sh | bash -s -- --purge --yes
# By default this removes the PROGRAM — the binary, the desktop launcher + icons, the login-autostart entry, and caches —
# and PRESERVES the user's data: settings, the node identity key, every mirrored
# repository, and chat history. Uninstalling must never be the way an identity
# or a repository is lost (issue #500); a later install.sh run picks the same
# identity and repositories back up.
#
# --purge additionally erases every byte of user data so a fresh install starts
# from a clean slate (no old node name, no stale chat). Because the identity key
# cannot be recovered, --purge asks for confirmation: type DELETE, or pass --yes
# (or set FORKMESH_ASSUME_YES=1) for a non-interactive curl | bash run.
# FORKMESH_PURGE=1 is equivalent to --purge. This is the same routine install.sh
# runs for `--uninstall [--purge]`, packaged as a standalone script.
set -euo pipefail

FORKMESH_HOST="${FORKMESH_HOST:-https://forkmesh.com}"
# Paths must match install.sh so we remove exactly what it created.
if [ -z "${FORKMESH_DIR:-}" ] && [ -d "$HOME/.local/share/forkmesh/src" ] &&
   [ ! -e "$HOME/projects/forkmesh/src" ]; then
  SRC="$HOME/.local/share/forkmesh/src"
else
  SRC="${FORKMESH_DIR:-$HOME/projects/forkmesh/src}"
fi
BIN_DIR="${FORKMESH_BIN_DIR:-$HOME/.local/bin}"
BIN="$BIN_DIR/forkmesh"

say()  { printf '\033[32m==>\033[0m %s\n' "$1"; }
warn() { printf '\033[33mWarning:\033[0m %s\n' "$1" >&2; }
die()  { printf '\033[31mError:\033[0m %s\n' "$1" >&2; exit 1; }

# ============================================================================
# DESTRUCTIVE — AI AGENTS MUST NOT RUN THIS SCRIPT.
# --purge (and install.sh --reinstall / --uninstall --purge) permanently erases
# the node identity key (the account cannot be recovered), settings, SSH keys,
# mirrored repositories, chat and agent history. Never run it, and never run a
# test that runs it, on a real machine. Ask the user to run it themselves.
#
# 2026-09-22: a test that sandboxed only HOME inherited the desktop's
# FORKMESH_HOME (AppPaths.cpp exports it to every agent) and a reinstall erased
# the owner's real ~/projects/forkmesh/{data,config,ssh,state,logs,cache}.
# guard_user_data_wipe below is the backstop; keep it in sync with install.sh.
# ============================================================================
agent_shell() {
  [ -n "${CLAUDECODE:-}${CLAUDE_CODE_ENTRYPOINT:-}${FORKMESH_AGENT_PROVIDER:-}${CODEX_SANDBOX:-}${CODEX_SANDBOX_NETWORK_DISABLED:-}" ]
}
account_home() {
  local user entry
  user="$(id -un 2>/dev/null)" || return 0
  entry="$(getent passwd "$user" 2>/dev/null)" || return 0
  printf '%s' "$entry" | cut -d: -f6
}
# guard_user_data_wipe <purge 0|1> <dir>... — refuses before anything is deleted.
guard_user_data_wipe() {
  local wipes_data="$1" home_dir real_home d
  shift
  home_dir="$(cd "$HOME" 2>/dev/null && pwd -P)" ||
    die "Refusing to remove anything: HOME ($HOME) is not a directory."
  for d in "$@"; do
    case "$d" in
      "$HOME"/*|"$home_dir"/*) ;;
      *) die "Refusing to erase $d: it is outside HOME ($HOME). FORKMESH_HOME / FORKMESH_PROJECTS_DIR point at a folder this HOME does not own — that is how a sandboxed run erased real ForkMesh data. Unset them, or run with the real HOME." ;;
    esac
  done
  if [ "$wipes_data" = "1" ] && agent_shell; then
    real_home="$(account_home)"
    if [ -z "$real_home" ] || [ "$home_dir" = "$(cd "$real_home" 2>/dev/null && pwd -P)" ]; then
      die "Refusing: this is an AI agent shell, and erasing ForkMesh user data (identity key, settings, SSH keys, repositories, agent history) cannot be undone. Agents must never run this. Ask the user to run it themselves in their own terminal."
    fi
  fi
}

fm_safe_rm_tree() {
  local parent="$1" target base name home_dir projects project sub rel first kind root
  local worktree="${FM_SAFE_RM_WORKTREE:-0}"
  shift
  [ -n "$parent" ] && [ "$parent" != "/" ] || { printf 'Refusing to delete: no safe expected parent folder was given\n' >&2; return 64; }
  if [ ! -d "$parent" ]; then
    for target in "$@"; do
      if [ -e "$target" ] || [ -L "$target" ]; then printf 'Refusing to delete: %s is not inside %s\n' "$target" "$parent" >&2; return 64; fi
    done
    return 0
  fi
  parent="$(cd -P -- "$parent" 2>/dev/null && pwd -P)" || { printf 'Refusing to delete: cannot resolve %s\n' "$parent" >&2; return 64; }
  [ "$parent" != "/" ] || { printf 'Refusing to delete: the expected parent folder is /\n' >&2; return 64; }
  home_dir="$(cd -P -- "${HOME:-/}" 2>/dev/null && pwd -P)" || home_dir="/"
  for target in "$@"; do
    [ -n "$target" ] || { printf 'Refusing to delete: the path is empty\n' >&2; return 64; }
    [ -e "$target" ] || [ -L "$target" ] || continue
    name="${target%/}"; name="${name##*/}"
    case "$name" in ""|"."|"..") printf 'Refusing to delete: %s\n' "$target" >&2; return 64 ;; esac
    base="$(cd -P -- "$(dirname -- "$target")" 2>/dev/null && pwd -P)" || { printf 'Refusing to delete: cannot resolve %s\n' "$target" >&2; return 64; }
    target="${base%/}/$name"
    case "$home_dir/" in "$target"/*) printf 'Refusing to delete: %s is HOME or contains it\n' "$target" >&2; return 64 ;; esac
    case "$target" in "$parent"/*) ;; *) printf 'Refusing to delete: %s is not inside %s\n' "$target" "$parent" >&2; return 64 ;; esac
    case "/$target/" in */.git/*) printf 'Refusing to delete: %s is git history (.git)\n' "$target" >&2; return 64 ;; esac
    if [ -d "$target/.git" ] && [ ! -L "$target/.git" ]; then
      printf 'Refusing to delete: %s is a git checkout\n' "$target" >&2; return 64
    fi
    if [ -e "$target/.git" ] && [ "$worktree" != "1" ]; then
      printf 'Refusing to delete: %s is a git worktree\n' "$target" >&2; return 64
    fi
    for projects in "$home_dir/projects" "${FORKMESH_PROJECTS_DIR:-}" "${FORKMESH_HOME:+${FORKMESH_HOME%/*}}"; do
      [ -n "$projects" ] || continue
      if [ -d "$projects" ]; then projects="$(cd -P -- "$projects" && pwd -P)" || continue; fi
      case "$projects/" in "$target"/*) printf 'Refusing to delete: %s contains the projects folder %s\n' "$target" "$projects" >&2; return 64 ;; esac
      case "$target" in "$projects"/*) ;; *) continue ;; esac
      rel="${target#"$projects"/}"
      project="${rel%%/*}"
      [ "$project" != "$rel" ] || { printf 'Refusing to delete: %s is a project folder\n' "$target" >&2; return 64; }
      rel="${rel#*/}"
      if [ "$project" = "forkmesh_data" ]; then
        project="${rel%%/*}"
        [ "$project" != "$rel" ] || { printf 'Refusing to delete: %s is a project data folder\n' "$target" >&2; return 64; }
        rel="${rel#*/}"
      fi
      first="${rel%%/*}"
      case "$project" in forkmesh-backup) printf 'Refusing to delete: %s is inside the backup project\n' "$target" >&2; return 64 ;; esac
      case "$first" in
        src|ssh) kind=strict ;;
        data|config|state|logs) kind=owned ;;
        bin|builds|cache|release|tmp|toolchain|toolchain-*|worktrees) continue ;;
        *) kind=strict ;;
      esac
      sub="${rel#"$first"}"; sub="${sub#/}"
      [ -n "$sub" ] || { printf 'Refusing to delete: %s is a protected ForkMesh folder\n' "$target" >&2; return 64; }
      case "/$sub/" in
        */build/*|*/build-*|*/build_*|*/builds/*|*/cache/*|*/.cache/*|*/caches/*|*/tmp/*|*/.tmp/*|*/tmp-*|*/tmp.*|*/temp/*|*/worktrees/*|*/.worktrees/*|*/node_modules/*|*/__pycache__/*|*/.pytest_cache/*|*/dist/*|*/out/*|*/.forkmesh/?*/*) continue ;;
      esac
      if [ "$kind" = "owned" ] && [ "${sub#*/}" != "$sub" ]; then continue; fi
      printf 'Refusing to delete: %s is inside protected ForkMesh folder %s/%s/%s\n' "$target" "$projects" "$project" "$first" >&2
      return 64
    done
    for root in "$home_dir/.ssh" "$home_dir/.forkmesh" "$home_dir/.config/ForkMesh" "$home_dir/.local/share/ForkMesh" "$home_dir/.local/share/forkmesh" "${FORKMESH_DATA_DIR:-}" "${FORKMESH_CONFIG_DIR:-}"; do
      [ -n "$root" ] || continue
      case "$root/" in "$target"/*) printf 'Refusing to delete: %s is or contains %s\n' "$target" "$root" >&2; return 64 ;; esac
      case "$target" in "$root"/*/*) ;; "$root"/*) printf 'Refusing to delete: %s is a top-level folder of %s\n' "$target" "$root" >&2; return 64 ;; esac
    done
  done
  for target in "$@"; do
    [ -e "$target" ] || [ -L "$target" ] || continue
    rm -rf -- "$target" || return 1
  done
}

purge="${FORKMESH_PURGE:-0}"
assume_yes="${FORKMESH_ASSUME_YES:-0}"
for arg in "$@"; do
  case "$arg" in
    --purge) purge=1 ;;
    --yes|-y) assume_yes=1 ;;
    *) warn "Ignoring unknown option: $arg" ;;
  esac
done

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
cache_home="${XDG_CACHE_HOME:-$HOME/.cache}"

# Directories ForkMesh owns. QSettings org+app are both "ForkMesh", so the
# config/data/cache live under a capitalised "ForkMesh" dir; the installer's
# own checkout lives under the lowercase "forkmesh".
# In the projects layout the same things live in ~/projects/forkmesh. The
# project folder itself is never removed: it also holds src/, and may hold
# files of the user's own.
layout_home="${FORKMESH_HOME:-${FORKMESH_PROJECTS_DIR:-$HOME/projects}/forkmesh}"
layout_home="${layout_home%/}"
layout_store="${layout_home%/*}/forkmesh_data/forkmesh"
program_dirs=(
  "$cache_home/ForkMesh"           # caches
  "$layout_home/cache"
  "$layout_store/cache"
)
# User data: only erased with --purge.
data_dirs=(
  "$config_home/ForkMesh"          # settings (node name, server, prefs)
  "$data_home/ForkMesh"            # identity key, mirrors, repos, chat, actions
  "$HOME/.forkmesh"                # diagnostics and local runtime state
  "$layout_home/data"              # the same, in the projects layout
  "$layout_home/config"
  "$layout_home/ssh"
  "$layout_home/state"
  "$layout_home/logs"
  "$layout_store/data"
  "$layout_store/config"
  "$layout_store/ssh"
  "$layout_store/state"
  "$layout_store/logs"
)
# Loose files: binary, desktop launcher, autostart entry, installed icon.
files=(
  "$BIN"
  "$data_home/applications/forkmesh.desktop"
  "$config_home/autostart/forkmesh.desktop"
  "$data_home/icons/forkmesh.png"
  "$data_home/nautilus/scripts/Open with ForkMesh"
  "$data_home/kio/servicemenus/forkmesh-open.desktop"
  "$data_home/nemo/actions/forkmesh-open.nemo_action"
)

if [ "$purge" = "1" ]; then
  guard_user_data_wipe 1 "${program_dirs[@]}" "${data_dirs[@]}"
else
  guard_user_data_wipe 0 "${program_dirs[@]}"
fi

if [ "$purge" = "1" ]; then
  printf '\033[31mThis removes ForkMesh and ALL of its data from this computer:\033[0m\n'
  printf '  • the forkmesh binary\n'
  printf '  • settings, the node identity key (the account cannot be recovered)\n'
  printf '  • every mirrored repository, and all chat history\n'
  printf '  • the desktop launcher, icons, and login-autostart entry\n\n'

  # Honour a non-interactive confirm so `curl | bash` works: pass --yes (or set
  # FORKMESH_ASSUME_YES=1). Otherwise prompt when a terminal is attached.
  if [ "$assume_yes" != "1" ]; then
    if [ -t 0 ]; then
      printf 'Type DELETE to continue: '
      answer=""; read -r answer || answer=""
      [ "$answer" = "DELETE" ] || die "Uninstall cancelled."
    else
      die "Refusing to erase ForkMesh data without confirmation. Re-run with --yes (or set FORKMESH_ASSUME_YES=1):  curl -fsSL $FORKMESH_HOST/uninstall.sh | bash -s -- --purge --yes"
    fi
  fi
else
  printf 'This removes the ForkMesh program from this computer:\n'
  printf '  • the forkmesh binary and caches\n'
  printf '  • the desktop launcher, icons, and login-autostart entry\n'
  printf 'Your settings, node identity key, repositories, and chat history are kept.\n\n'
fi

# A headless install (e.g. a VPS) runs the binary as a plain background
# process (nohup, no systemd unit). Deleting the binary out from under it just
# unlinks the inode: the running process keeps executing from the deleted
# file and keeps reporting its (now stale) presence/version to the network,
# which is why mirrors could still show an old version after "uninstalling"
# the host. Stop it first — and match it more than one way, because the process
# we must kill may NOT be the binary at the current $BIN path:
#   • by exact process name ("forkmesh")  — catches an OLD install that lived
#     at a different path, and a copy still executing from a deleted/replaced
#     inode after an in-app update (the classic "still on an old version").
#   • by $BIN and by the source build dir — the normal and in-place locations.
_fm_alive() { pgrep -x forkmesh >/dev/null 2>&1 \
  || pgrep -f -- "$BIN" >/dev/null 2>&1 \
  || pgrep -f -- "$SRC" >/dev/null 2>&1; }
if _fm_alive; then
  pkill -x forkmesh   2>/dev/null || true
  pkill -f -- "$BIN"  2>/dev/null || true
  pkill -f -- "$SRC"  2>/dev/null || true
  for _ in 1 2 3 4 5 6 7 8 9 10; do _fm_alive || break; sleep 0.5; done
  if _fm_alive; then
    pkill -9 -x forkmesh   2>/dev/null || true
    pkill -9 -f -- "$BIN"  2>/dev/null || true
    pkill -9 -f -- "$SRC"  2>/dev/null || true
  fi
  # Still alive after SIGKILL => not ours to signal (almost always a root/sudo
  # install). Try a non-interactive sudo (never prompts, so curl|bash can't
  # hang), then give up with a loud warning rather than leaving a phantom node.
  if _fm_alive; then
    if [ "$(id -u 2>/dev/null)" != "0" ] && command -v sudo >/dev/null 2>&1; then
      sudo -n pkill -9 -x forkmesh 2>/dev/null || true
      sudo -n pkill -9 -f -- "$BIN" 2>/dev/null || true
    fi
    if _fm_alive; then
      warn "A ForkMesh daemon is STILL running (it is likely owned by root — re-run this uninstall with sudo). Until it is stopped it will keep reporting an old version to the network."
    else
      say "Stopped the running ForkMesh daemon"
    fi
  else
    say "Stopped the running ForkMesh daemon"
  fi
fi

dirs=("${program_dirs[@]}")
if [ "$purge" = "1" ]; then dirs+=("${data_dirs[@]}"); fi
for d in "${dirs[@]}"; do
  [ -e "$d" ] || continue
  if [ "$purge" = "1" ]; then
    rm -rf -- "$d" && say "Removed $d"
  else
    fm_safe_rm_tree "$(dirname -- "$d")" "$d" && say "Removed $d"
  fi
done
if [ -e "$SRC" ]; then
  say "Kept the source checkout $SRC (a git checkout with your branches); delete it yourself if you no longer need it."
fi
for f in "${files[@]}"; do
  if [ -e "$f" ]; then rm -f -- "$f" && say "Removed $f"; fi
done
# Every hicolor icon bucket the installer may have written.
find "$data_home/icons/hicolor" \( -name 'forkmesh.png' -o -name 'forkmesh.svg' \) -delete 2>/dev/null || true
# Refresh desktop caches so the launcher disappears promptly.
command -v update-desktop-database >/dev/null 2>&1 \
  && update-desktop-database "$data_home/applications" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 \
  && gtk-update-icon-cache -f -t "$data_home/icons/hicolor" >/dev/null 2>&1 || true

if [ "$purge" = "1" ]; then
  say "ForkMesh has been completely removed."
else
  for d in "${data_dirs[@]}"; do
    if [ -e "$d" ]; then say "Kept $d"; fi
  done
  say "ForkMesh has been removed; your identity and repositories were kept."
  say "Erase them too with:  curl -fsSL $FORKMESH_HOST/uninstall.sh | bash -s -- --purge"
fi
say "Reinstall any time with:  curl -fsSL $FORKMESH_HOST/install.sh | bash"
