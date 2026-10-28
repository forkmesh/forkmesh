#!/usr/bin/env bash
# ForkMesh desktop client installer.
#   curl -fsSL https://forkmesh.com/install.sh | bash
# Autodetects this machine's OS/arch and installs the matching PREBUILT binary
# attached to the latest release (no compiler/Qt toolchain, no multi-minute
# build). A missing prebuilt fails quickly instead of compiling on an end-user
# machine. Developers can explicitly request a source build with
# FORKMESH_FROM_SOURCE=1, or restore the legacy fallback with
# FORKMESH_NO_SOURCE_FALLBACK=0. Missing build
# prerequisites are installed automatically when a supported package manager
# is detected; set FORKMESH_NO_INSTALL_DEPS=1 to opt out. The binary lands in
# ~/.local/bin.
set -euo pipefail

# Everything below runs inside this brace group, and that is load-bearing — do
# not remove it, and keep the matching `}` as the last line of the file.
#
# Under `curl -fsSL … | bash` the shell reads this script FROM STDIN, so fd 0 is
# the pipe carrying the script text itself. Bash reads that pipe a byte at a
# time and does not move it to a higher fd, so any child process that reads
# stdin eats the script out from under the parser: bash resumes mid-line and
# tries to execute the remaining fragment. That is exactly what broke installs
# (issue #659) — apt-get consumed two bytes while installing age, so the next
# line, `ensure tar command -v tar`, was parsed as `sure tar command -v tar` and
# the install died with "line 1002: sure: command not found" before anything was
# installed. The reported line number is genuine; there is no typo to fix.
#
# A brace group is a compound command, so bash must parse through the closing
# `}` before it can execute any of it. That drains the whole script off the pipe
# up front, leaving children nothing to steal. Unlike wrapping the body in a
# function this is semantically transparent: `$@`, `shift`, variable scope and
# error line numbers are all unchanged.
{

# Installer script version. Bump on every change to install.sh so a user can
# confirm — from the banner printed at startup — that they are running the
# freshly deployed script and not a cached/older copy from the CDN edge.
INSTALLER_VERSION="0.19.0 (2026-09-29)"

# ForkMesh is self-hosted: the same server that serves this script also serves
# the source over git's smart-HTTP protocol at https://<host>/<node>/<repo>.
# Override FORKMESH_HOST when self-hosting. By default, the installer asks the
# mainnode for the currently-online forkmesh host with the most recent uptime.
FORKMESH_HOST="${FORKMESH_HOST:-https://forkmesh.com}"
FORKMESH_NODE="${FORKMESH_NODE:-}"
# The name to give a freshly-deployed mirror node. The deploy UI passes the
# operator's chosen name as FORKMESH_NODE_NAME, leaving FORKMESH_NODE empty so
# the clone source still auto-resolves to a real online mirror. The native
# mirror service adopts this name during bootstrap. Fall back to a verbatim
# FORKMESH_NODE (older deploy UI) so naming still works across skew.
# Empty unless the operator set one of them explicitly.
FORKMESH_NODE_NAME="${FORKMESH_NODE_NAME:-${FORKMESH_NODE:-}}"
# The account/owner this node is being attached to (adhoc #258). The desktop
# Hosts panel that drives the install passes the operator's own account name
# here so the installer can ECHO it back — that way the operator can see, right
# in the install stream, which account the fresh node is meant to end up under
# and confirm it attached correctly rather than silently registering as an
# orphan. Purely informational: the actual attachment is done by the link code
# below (the relay pairs it with the desktop's key-signed half). Empty on a
# plain `curl | bash` install where no driving account is known.
FORKMESH_OWNER="${FORKMESH_OWNER:-}"
# Reinstall (adhoc #258): wipe any existing ForkMesh install AND its data on this
# machine, then continue straight into a fresh install below (from the uploaded
# or freshly-downloaded prebuilt binary). Driven by the Hosts panel's
# "Uninstall + reinstall (all hosts)" action via FORKMESH_REINSTALL=1, or by
# --reinstall on the command line. Non-interactive by nature, so it assumes the
# destructive confirmation (the Qt-side dialog is the real gate).
FORKMESH_REINSTALL="${FORKMESH_REINSTALL:-0}"
# Restart (adhoc): stop the node's already-running daemon just before the
# launch step so the freshly (re)built binary cleanly takes over. A plain
# re-run would otherwise leave the old daemon running the old binary and start a
# SECOND one beside it. Used by the Hosts panel's "Update from source (all
# hosts)" action (with FORKMESH_FROM_SOURCE=1) to update the fleet straight from
# source without publishing a release. Unlike a reinstall it does NOT touch the
# node's identity key or mirrored data — it only stops+relaunches the process.
FORKMESH_RESTART="${FORKMESH_RESTART:-0}"
# Link code for attaching a freshly installed native mirror node to the
# installing user's account (adhoc #53).
FORKMESH_LINK_CODE="${FORKMESH_LINK_CODE:-}"
# Direct-upload install (adhoc #67): the desktop app's Hosts panel can stream
# the release binary over the SSH session itself instead of this machine
# downloading it from the relay. FORKMESH_LOCAL_BINARY names the pre-uploaded
# file; FORKMESH_LOCAL_OS / FORKMESH_LOCAL_ARCH record the platform the
# uploader says it targets, so a mismatched upload falls back to the normal
# relay download instead of installing a binary this machine cannot run.
FORKMESH_LOCAL_BINARY="${FORKMESH_LOCAL_BINARY:-}"
FORKMESH_LOCAL_OS="${FORKMESH_LOCAL_OS:-}"
FORKMESH_LOCAL_ARCH="${FORKMESH_LOCAL_ARCH:-}"
# Optional fail-closed release provenance pin. The desktop fleet installer
# supplies both the version and build commit embedded in its controller binary.
# A normal one-line install leaves these empty and keeps existing behavior.
FORKMESH_EXPECTED_BUILD_COMMIT="${FORKMESH_EXPECTED_BUILD_COMMIT:-}"
FORKMESH_EXPECTED_RELEASE_VERSION="${FORKMESH_EXPECTED_RELEASE_VERSION:-}"
# A prebuilt artifact is accepted only when its exact release manifest is
# authenticated independently of the mirror that supplied it. Controllers may
# pin the exact manifest SHA-256. Standalone installs may instead point at a
# locally provisioned Ed25519 publisher public key (PEM); the detached
# release.json.sig is then verified with OpenSSL. A checksum copied from the
# fetched manifest is never a trust anchor.
FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256="${FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256:-}"
FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE="${FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE:-}"
[ -n "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" ] ||
  [ ! -f /etc/forkmesh/release-publisher.pem ] ||
  FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE=/etc/forkmesh/release-publisher.pem
FORKMESH_MIN_RELEASE_VERSION="1.2.8"
# Direct uploads do not carry a release manifest, so the authenticated
# controller must supply their exact digest.
FORKMESH_LOCAL_BINARY_SHA256="${FORKMESH_LOCAL_BINARY_SHA256:-}"
FORKMESH_NAME="${FORKMESH_NAME:-forkmesh}"
FORKMESH_INSTALL_SOURCE_URL="${FORKMESH_INSTALL_SOURCE_URL:-${FORKMESH_HOST%/}/api/install-source}"
FORKMESH_DIAG_URL="${FORKMESH_DIAG_URL:-${FORKMESH_HOST%/}/api/install-diag}"
REPO="${FORKMESH_REPO:-}"
# Space-separated list of online mirror nodes resolved from the mainnode, best
# first, and the matching list of clone URLs to try in order. A direct-HTTPS
# endpoint can become unavailable after its latest signed health check, so the
# installer still falls back instead of dead-ending on the first one. Populated
# by resolve_install_node / the mirror block.
FORKMESH_NODES=""
REPO_CANDIDATES=()
# Set by clean_clone to the human-readable reason the last clone attempt failed
# (e.g. "mirror endpoint timed out (HTTP 504)"), so the final error and the anonymous
# diagnostics can say WHY every mirror was unreachable rather than just "failed".
CLONE_FAIL_REASON=""
# The build checkout lives in a dedicated location. It is a real git checkout
# that may hold the user's own branches, so the installer never deletes it:
# a failed update stops, and an incomplete checkout is moved aside.
# New installs use the projects layout, ~/projects/forkmesh/src, beside the
# application's own data. An install made before that layout keeps its old
# checkout: its ownership marker is bound to that exact path.
if [ -z "${FORKMESH_DIR:-}" ] && [ -d "$HOME/.local/share/forkmesh/src" ] &&
   [ ! -e "$HOME/projects/forkmesh/src" ]; then
  SRC="$HOME/.local/share/forkmesh/src"
else
  SRC="${FORKMESH_DIR:-$HOME/projects/forkmesh/src}"
fi
BIN_DIR="${FORKMESH_BIN_DIR:-$HOME/.local/bin}"
BIN="$BIN_DIR/forkmesh"
PID_FILE="${FORKMESH_PID_FILE:-${XDG_RUNTIME_DIR:-$HOME/.local/share/forkmesh}/forkmesh.pid}"
SYSTEMD_UNIT="/etc/systemd/system/forkmesh-mirror-node.service"
SYSTEMD_ENV="/etc/forkmesh/mirror-node.env"
SYSTEMD_CONFIG="/var/lib/forkmesh/mirror-node.json"
SYSTEMD_BIN="/usr/local/bin/forkmesh-mirror-node"
SYSTEMD_INSTALL_MARKER="/etc/forkmesh/installer-managed"
SYSTEMD_STATE_DIR="/var/lib/forkmesh"
SYSTEMD_STATE_MARKER="/var/lib/forkmesh/.forkmesh-managed-service"
SYSTEMD_BOOTSTRAP_MARKER="/var/lib/forkmesh/.forkmesh-installing-service"
# Set to 1 if a clone is rejected by the relay's integrity gate, so the final
# error can explain that specific (owner-fixable) case instead of a generic one.
PIN_FAILURE=0
# Set to 1 once a prebuilt release binary has been installed, so the whole
# source-build pipeline (toolchain deps, clone, compile) is skipped.
INSTALLED_PREBUILT=0
# Set by build_client to the build directory; pre-declared so the shared
# desktop/launch tail can reference it even on the prebuilt fast path (where no
# build ever runs) without tripping `set -u`.
BUILD=""
# A commit-pinned reinstall validates and stages its replacement before
# honoring the destructive reinstall request. This keeps the old node running
# when release metadata or candidate provenance is stale.
DEFER_PINNED_REINSTALL=0
# Source trees created by this installer carry an owner/path-bound marker. No
# recursive operation is allowed merely because an environment variable points
# at a directory.
MANAGED_MARKER=".forkmesh-managed"
INSTALLER_START_DIR="$(pwd -P 2>/dev/null || pwd)"
INSTALLER_WORKSPACE_ROOT="$(
  git -C "$INSTALLER_START_DIR" rev-parse --show-toplevel 2>/dev/null || true
)"

# Whether to fall back to a source build when no prebuilt binary is published.
# End-user installation is binary-only on every platform: silently switching to
# a large Qt build is precisely what makes installation fail on low-memory or
# damaged hosts. Source builds remain an explicit developer choice.
if [ -z "${FORKMESH_NO_SOURCE_FALLBACK:-}" ]; then
  FORKMESH_NO_SOURCE_FALLBACK=1
fi
# Seconds to wait before each re-attempt of a mirror metadata fetch (default
# 4 attempts over ~35 s, riding out a mirror gateway restart) and of the
# prebuilt blob download. Space-separated; "" disables re-attempts.
FORKMESH_METADATA_RETRY_DELAYS="${FORKMESH_METADATA_RETRY_DELAYS-5 10 20}"
FORKMESH_DOWNLOAD_RETRY_DELAYS="${FORKMESH_DOWNLOAD_RETRY_DELAYS-5}"

# Anchor to a directory that exists. The installer may be launched from a path
# that was just deleted — e.g. running this right after the uninstaller removed
# ~/.local/share/forkmesh from a shell still sitting inside it. With a missing
# working directory git aborts every clone up front with "fatal: Unable to read
# current working directory", before it ever contacts the relay. cd somewhere
# stable so this script and every child process inherit a valid CWD.
cd "$HOME" 2>/dev/null || cd / 2>/dev/null || true

say()  { printf '\033[32m==>\033[0m %s\n' "$1"; }
warn() { printf '\033[33mWarning:\033[0m %s\n' "$1" >&2; }
die()  { printf '\033[31mError:\033[0m %s\n' "$1" >&2; exit 1; }

# ============================================================================
# DESTRUCTIVE — AI AGENTS MUST NOT RUN --uninstall / --reinstall / --purge.
# Those modes (and uninstall.sh --purge) permanently erases
# the node identity key (the account cannot be recovered), settings, SSH keys,
# mirrored repositories, chat and agent history. Never run it, and never run a
# test that runs it, on a real machine. Ask the user to run it themselves.
#
# 2026-09-22: a test that sandboxed only HOME inherited the desktop's
# FORKMESH_HOME (AppPaths.cpp exports it to every agent) and a reinstall erased
# the owner's real ~/projects/forkmesh/{data,config,ssh,state,logs,cache}.
# guard_user_data_wipe below is the backstop; keep it in sync with uninstall.sh.
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
      if [ "$first" = "data" ] && { [ "$sub" = "core" ] || [ "${sub#core/}" != "$sub" ]; }; then
        sub="${sub#core}"; sub="${sub#/}"
        [ -n "$sub" ] || { printf 'Refusing to delete: %s is a protected ForkMesh folder\n' "$target" >&2; return 64; }
        first="${sub%%/*}"
        case "$first" in
          cache) continue ;;
          ssh) kind=strict ;;
          *) kind=owned ;;
        esac
        sub="${sub#"$first"}"; sub="${sub#/}"
        [ -n "$sub" ] || { printf 'Refusing to delete: %s is a protected ForkMesh folder\n' "$target" >&2; return 64; }
        first="data/core/$first"
      fi
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
# Verbose diagnostic line, printed only when FORKMESH_DEBUG=1. Use it for the
# extra detail a remote operator needs to debug a failed install (the resolved
# mirror list, the raw source response, the exact URLs being cloned) without
# cluttering the normal install log.
dbg()  { [ "${FORKMESH_DEBUG:-0}" = "1" ] && printf '\033[2m[debug]\033[0m %s\n' "$1" >&2 || true; }

_sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" 2>/dev/null | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" 2>/dev/null | awk '{print $1}'
  else
    echo ""
  fi
}

_canonical_path() {
  local value="$1" probe suffix="" base physical
  case "$value" in
    /*) probe="$value" ;;
    *) probe="$INSTALLER_START_DIR/$value" ;;
  esac
  case "$probe" in *'
'*) return 1 ;; esac
  # Resolve the deepest existing ancestor physically, then append only simple
  # non-symlink path components. This works on Linux and macOS without relying
  # on GNU realpath -m.
  while [ ! -e "$probe" ]; do
    base="${probe##*/}"
    [ -n "$base" ] && [ "$base" != "." ] && [ "$base" != ".." ] || return 1
    suffix="/$base$suffix"
    probe="${probe%/*}"
    [ -n "$probe" ] || probe="/"
  done
  [ ! -L "$probe" ] || return 1
  if [ -d "$probe" ]; then
    physical="$(cd -P -- "$probe" 2>/dev/null && pwd -P)" || return 1
  else
    physical="$(cd -P -- "${probe%/*}" 2>/dev/null && pwd -P)" || return 1
    physical="$physical/${probe##*/}"
  fi
  physical="${physical%/}$suffix"
  [ -n "$physical" ] || physical="/"
  printf '%s\n' "$physical"
}

_path_owner_uid() {
  stat -c '%u' "$1" 2>/dev/null || stat -f '%u' "$1" 2>/dev/null || true
}

_marker_is_valid() {
  local dir="$1" marker="$1/$MANAGED_MARKER" uid
  [ -d "$dir" ] && [ ! -L "$dir" ] && [ -f "$marker" ] &&
    [ ! -L "$marker" ] || return 1
  uid="$(_path_owner_uid "$marker")"
  [ "$uid" = "$(id -u)" ] || return 1
  grep -Fqx 'forkmesh-managed-v1' "$marker" &&
    grep -Fqx "uid=$(id -u)" "$marker" &&
    grep -Fqx "path=$dir" "$marker"
}

_write_managed_marker() {
  local dir="$1" marker="$1/$MANAGED_MARKER"
  [ -d "$dir" ] && [ ! -L "$dir" ] || return 1
  umask 077
  {
    printf 'forkmesh-managed-v1\n'
    printf 'uid=%s\n' "$(id -u)"
    printf 'path=%s\n' "$dir"
  } > "$marker"
}

_move_src_aside() {
  local dir="$1" aside
  [ -e "$dir" ] || return 0
  aside="$dir.replaced-$(date +%Y%m%d-%H%M%S)"
  while [ -e "$aside" ]; do aside="$aside-$$"; done
  mv -- "$dir" "$aside" || die "Could not move $dir aside to $aside; nothing was deleted."
  warn "Moved the previous checkout to $aside. Nothing was deleted; remove it yourself once you no longer need it."
}

_dir_is_empty() {
  [ -d "$1" ] && [ -z "$(ls -A -- "$1" 2>/dev/null)" ]
}

SRC="$(_canonical_path "$SRC")" ||
  die "FORKMESH_DIR is not a canonical, non-symlink path."
CANONICAL_HOME="$(_canonical_path "$HOME")" ||
  die "HOME could not be canonicalized safely."
case "$SRC" in
  /|/bin|/boot|/dev|/etc|/home|/lib|/lib64|/opt|/proc|/root|/run|/sbin|/srv|/sys|/tmp|/usr|/var)
    die "Refusing unsafe FORKMESH_DIR target: $SRC" ;;
esac
[ "$SRC" != "$CANONICAL_HOME" ] ||
  die "FORKMESH_DIR may not be the home directory."
if [ -n "$INSTALLER_WORKSPACE_ROOT" ]; then
  INSTALLER_WORKSPACE_ROOT="$(_canonical_path "$INSTALLER_WORKSPACE_ROOT")" ||
    die "Current workspace could not be canonicalized."
  case "$INSTALLER_WORKSPACE_ROOT/" in
    "$SRC/"*) die "FORKMESH_DIR may not be the current workspace or one of its parents." ;;
  esac
fi
# A new custom target must use the intentionally narrow .../forkmesh/src shape.
# Existing targets are accepted only when their owner/path-bound marker proves
# this installer already owns them.
if [ ! -e "$SRC" ]; then
  case "$SRC" in */forkmesh/src) ;; *)
    die "A new FORKMESH_DIR must end in /forkmesh/src." ;;
  esac
elif [ -L "$SRC" ]; then
  die "FORKMESH_DIR may not be a symlink."
elif ! _marker_is_valid "$SRC"; then
  _legacy_default="$(_canonical_path "$HOME/.local/share/forkmesh/src" 2>/dev/null || true)"
  _legacy_marker="$SRC/$MANAGED_MARKER"
  if [ "$SRC" = "$_legacy_default" ] && [ -f "$_legacy_marker" ] &&
     [ ! -L "$_legacy_marker" ] &&
     [ "$(_path_owner_uid "$_legacy_marker")" = "$(id -u)" ]; then
    _write_managed_marker "$SRC" ||
      die "Could not upgrade the legacy ForkMesh install marker."
  else
    die "Existing FORKMESH_DIR is not marked as an owner-matched ForkMesh install: $SRC"
  fi
fi

if [ -n "$FORKMESH_EXPECTED_BUILD_COMMIT" ]; then
  FORKMESH_EXPECTED_BUILD_COMMIT="$(
    printf '%s' "$FORKMESH_EXPECTED_BUILD_COMMIT" | tr 'A-F' 'a-f'
  )"
  if ! printf '%s' "$FORKMESH_EXPECTED_BUILD_COMMIT" |
      grep -Eq '^([0-9a-f]{40}|[0-9a-f]{64})$'; then
    die "FORKMESH_EXPECTED_BUILD_COMMIT must be an exact 40- or 64-hex Git commit."
  fi
  if ! printf '%s' "$FORKMESH_EXPECTED_RELEASE_VERSION" |
      grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9._+-]+)?$'; then
    die "FORKMESH_EXPECTED_RELEASE_VERSION must be an exact release version."
  fi
fi
for _fm_digest_name in \
    FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256 \
    FORKMESH_LOCAL_BINARY_SHA256; do
  eval "_fm_digest_value=\${${_fm_digest_name}}"
  if [ -n "$_fm_digest_value" ] &&
     ! printf '%s' "$_fm_digest_value" | grep -Eq '^[0-9a-fA-F]{64}$'; then
    die "$_fm_digest_name must be an exact 64-hex SHA-256 digest."
  fi
done
FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256="$(
  printf '%s' "$FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256" | tr 'A-F' 'a-f'
)"
FORKMESH_LOCAL_BINARY_SHA256="$(
  printf '%s' "$FORKMESH_LOCAL_BINARY_SHA256" | tr 'A-F' 'a-f'
)"
if [ -n "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" ]; then
  [ -f "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" ] &&
    [ ! -L "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" ] ||
    die "FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE must name a non-symlink regular file."
  _fm_key_uid="$(_path_owner_uid "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE")"
  [ "$_fm_key_uid" = "0" ] || [ "$_fm_key_uid" = "$(id -u)" ] ||
    die "Trusted release public key must be owned by root or the installing user."
  _fm_key_mode="$(
    stat -c '%a' "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" 2>/dev/null ||
      stat -f '%Lp' "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" 2>/dev/null || true
  )"
  printf '%s' "$_fm_key_mode" | grep -Eq '^[0-7]{3,4}$' ||
    die "Could not validate trusted release public-key permissions."
  if (( (8#${_fm_key_mode} & 8#022) != 0 )); then
    die "Trusted release public key may not be group/world writable."
  fi
fi

# In debug mode, make git print the full HTTP exchange (request/response status
# and headers) on stderr so a 5xx from the relay can be traced to its cause.
if [ "${FORKMESH_DEBUG:-0}" = "1" ]; then
  export GIT_CURL_VERBOSE=1
fi
# Never let git stop a piped `curl | bash` install to prompt for credentials;
# fail fast (and surface as a classifiable clone error) instead of hanging.
export GIT_TERMINAL_PROMPT=0

# --- anonymous diagnostics --------------------------------------------------
# Report each install step to the mainnode so operators can see, in aggregate,
# where installs succeed or fail (find-a-mirror, prerequisites, clone, build,
# install, first launch). This is ANONYMOUS: RUN_ID is a fresh random id minted
# for this run only — it is never tied to your account, email, or IP address,
# and the server does not record the requesting IP. Only coarse platform facts
# (OS, CPU arch, package manager, distro id, installer version) are sent. Opt
# out entirely with FORKMESH_NO_DIAG=1.
RUN_ID="$( (head -c 16 /dev/urandom 2>/dev/null | od -An -tx1 2>/dev/null | tr -d ' \n') || true )"
[ -n "$RUN_ID" ] || RUN_ID="$$-$(date +%s 2>/dev/null || echo 0)"
DIAG_OS="$(uname -s 2>/dev/null || echo unknown)"
DIAG_ARCH="$(uname -m 2>/dev/null || echo unknown)"
DIAG_DISTRO=""
if [ -r /etc/os-release ]; then
  DIAG_DISTRO="$( ( . /etc/os-release 2>/dev/null; printf '%s' "${ID:-}" ) || true )"
fi
# Logical prerequisites we had to install (empty = the machine already had them).
DIAG_MISSING=""
# The install phase currently in progress; the EXIT trap reports it as failed if
# the script dies, so the funnel shows exactly where an install dropped off.
CURRENT_STEP="start"

_diag_post() {
  [ "${FORKMESH_NO_DIAG:-0}" = "1" ] && return 0
  command -v curl >/dev/null 2>&1 || return 0
  local step="$1" ok="$2" detail="${3:-}"
  # Short timeout: diagnostics must never slow down or fail the install, so
  # every error is swallowed.
  curl -fsS -m 5 -X POST "$FORKMESH_DIAG_URL" \
    -H 'Content-Type: application/json' \
    --data "{\"run\":\"$RUN_ID\",\"step\":\"$step\",\"ok\":$ok,\"os\":\"$DIAG_OS\",\"arch\":\"$DIAG_ARCH\",\"pm\":\"${PM:-}\",\"distro\":\"$DIAG_DISTRO\",\"version\":\"$INSTALLER_VERSION\",\"detail\":\"$detail\"}" \
    >/dev/null 2>&1 </dev/null || true
  return 0
}

# Fire-and-forget in the background.
diag() {
  _diag_post "$@" &
  return 0
}

# Synchronous variant for the last report before the script exits: a
# backgrounded POST can be killed with the shell before it leaves the machine,
# which is exactly when the failure detail matters most. Bounded by curl -m 5.
diag_sync() {
  _diag_post "$@"
  return 0
}

# die, but print the error first and then report `diag <step> 0 <detail>`
# synchronously so the failure reason reliably reaches /api/install-diag.
die_diag() {
  local step="$1" detail="$2" msg="$3"
  printf '\033[31mError:\033[0m %s\n' "$msg" >&2
  diag_sync "$step" 0 "$detail"
  exit 1
}

on_diag_exit() {
  local code=$?
  [ "$code" -ne 0 ] && diag "$CURRENT_STEP" 0 "exit$code"
  return 0
}
trap on_diag_exit EXIT

resolve_install_node() {
  # An explicit FORKMESH_NODE override pins a single mirror; honour it verbatim.
  if [ -n "$FORKMESH_NODE" ]; then
    FORKMESH_NODES="$FORKMESH_NODE"
    return 0
  fi
  command -v curl >/dev/null 2>&1 || die "curl is required to find an online ForkMesh mirror."

  local body raw_nodes nodes_blob source_url sep
  sep="?"
  case "$FORKMESH_INSTALL_SOURCE_URL" in
    *\?*) sep="&" ;;
  esac
  source_url="${FORKMESH_INSTALL_SOURCE_URL}${sep}_=$(date +%s)"
  dbg "Querying install source: $source_url"
  # A bounded timeout so an unreachable mainnode fails loudly instead of the
  # install appearing to hang forever with no output (issue #418) — curl has no
  # timeout by default and can sit for minutes on a dead connection.
  if ! body="$(curl -sSL -m 20 -H 'Cache-Control: no-cache' -H 'Pragma: no-cache' "$source_url")"; then
    die "Could not check for an online ForkMesh mirror (timed out after 20s querying $FORKMESH_INSTALL_SOURCE_URL). Please try again shortly."
  fi
  dbg "install-source response: $body"
  # Prefer the ranked "nodes":[ ... ] list (newer mainnode); fall back to the
  # single "node" field so older mainnodes — and the no-mirror error shape —
  # still parse. Extract one node id per line, in server-ranked order.
  nodes_blob="$(printf '%s\n' "$body" | sed -n 's/.*"nodes"[[:space:]]*:[[:space:]]*\[\([^]]*\)\].*/\1/p')"
  if [ -n "$nodes_blob" ]; then
    raw_nodes="$(printf '%s\n' "$nodes_blob" | tr ',' '\n' | sed -n 's/.*"\([^"]*\)".*/\1/p')"
  else
    raw_nodes="$(printf '%s\n' "$body" | sed -n 's/.*"node"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)"
  fi
  # Keep only well-formed node ids, preserving order and dropping duplicates.
  local valid="" n
  for n in $raw_nodes; do
    case "$n" in
      *[!A-Za-z0-9._:-]*|"") continue ;;
    esac
    case " $valid " in *" $n "*) continue ;; esac
    valid="${valid:+$valid }$n"
  done
  [ -n "$valid" ] || die "No online ForkMesh node is currently mirroring '$FORKMESH_NAME'. Start a node that publishes this repository, then try the installer again."
  FORKMESH_NODES="$valid"
  FORKMESH_NODE="${valid%% *}"
}

# Resolve REPO/REPO_CANDIDATES from an online mirror, if not already resolved.
# Called eagerly below unless a direct-upload binary (FORKMESH_LOCAL_BINARY)
# makes it unnecessary, and lazily as a fallback if that upload turns out to
# be unusable (e.g. platform mismatch) and a mirror download or source build
# is needed after all.
ensure_mirror_candidates() {
  [ "${#REPO_CANDIDATES[@]}" -gt 0 ] && return 0
  CURRENT_STEP="mirror"
  say "Resolving an online ForkMesh mirror to clone from…"
  resolve_install_node
  # Clone through the stable source-of-truth route. The Worker uses the ranked
  # signed endpoint set above to select/fail over between mirror2, mirror3, ...
  # internally. Building node-namespaced clone URLs here tied the repository
  # pin to a mirror alias and could return 503 even while the canonical
  # forkmesh/forkmesh route was healthy.
  REPO_CANDIDATES+=(
    "${FORKMESH_HOST%/}/forkmesh/${FORKMESH_NAME}"
  )
  REPO="${REPO_CANDIDATES[0]}"
  say "Using mirror node: $FORKMESH_NODE"
  case "$FORKMESH_NODES" in
    *" "*) say "  Mirror gateway will fail over across: $FORKMESH_NODES" ;;
  esac
  diag mirror 1
}

# --- uninstall --------------------------------------------------------------
# Remove the ForkMesh program: the binary, the cloned source, caches, the desktop
# launcher + icons, and the login-autostart entry. User data — settings, the
# node identity key, all mirrored repositories, chat history, and a managed
# service's /var/lib/forkmesh state — is PRESERVED unless --purge (or
# FORKMESH_PURGE=1) is given, so an uninstall is never how an identity or a
# repository is lost (issue #500). Run with:
#   curl -fsSL https://forkmesh.com/install.sh | bash -s -- --uninstall
#   curl -fsSL https://forkmesh.com/install.sh | bash -s -- --uninstall --purge --yes
# Mode: "full" (the --uninstall entry point, exits when done) or "reinstall"
# (called inline before a fresh install — always purges and skips the
# interactive confirmation, since a reinstall is already gated by its own
# Qt-side dialog / explicit flag, and returns instead of exiting so the caller
# can carry on installing).
# Stop only the daemon instance this installer started. A managed systemd unit
# is named explicitly; a user daemon is addressed by its owner-only PID file
# and its live executable path is checked before any signal is sent. Process
# name scans (`pkill forkmesh`) are intentionally forbidden because they can
# stop unrelated users, test instances, or other ForkMesh nodes on the host.
# $1: a short context label for the log line (unused beyond readability).
stop_forkmesh_daemons() {
  local pid="" expected="$BIN" live="" owner=""
  if [ "$(id -u)" -eq 0 ] && [ -f "$SYSTEMD_UNIT" ] &&
     grep -Fqx '# Managed-By: ForkMesh installer' "$SYSTEMD_UNIT"; then
    if ! grep -Fqx "ExecStart=$SYSTEMD_BIN --config $SYSTEMD_CONFIG" "$SYSTEMD_UNIT"; then
      die "Refusing to stop a modified forkmesh-mirror-node.service."
    fi
    systemctl stop forkmesh-mirror-node.service ||
      die "Could not stop the managed forkmesh-mirror-node.service."
    say "Stopped managed forkmesh-mirror-node.service"
    return 0
  fi

  [ -f "$PID_FILE" ] || {
    warn "No managed ForkMesh PID file exists; no process was signalled."
    return 0
  }
  [ ! -L "$PID_FILE" ] || die "Refusing symlink PID file: $PID_FILE"
  IFS= read -r pid < "$PID_FILE" || true
  printf '%s' "$pid" | grep -Eq '^[1-9][0-9]*$' ||
    die "Managed ForkMesh PID file is invalid."
  if ! kill -0 "$pid" 2>/dev/null; then
    rm -f -- "$PID_FILE"
    return 0
  fi
  owner="$(ps -o uid= -p "$pid" 2>/dev/null | tr -d '[:space:]')"
  [ "$owner" = "$(id -u)" ] ||
    die "Refusing to signal PID $pid because it is owned by uid ${owner:-unknown}."
  if [ -e "/proc/$pid/exe" ]; then
    live="$(readlink "/proc/$pid/exe" 2>/dev/null || true)"
    live="${live% (deleted)}"
    expected="$(_canonical_path "$BIN" 2>/dev/null || printf '%s' "$BIN")"
    [ "$live" = "$expected" ] ||
      die "Refusing to signal PID $pid because its executable is $live, not $expected."
  else
    live="$(ps -o command= -p "$pid" 2>/dev/null | awk '{print $1}')"
    case "$live" in "$BIN"|"$BIN.app/Contents/MacOS/ForkMesh") ;; *)
      die "Refusing to signal PID $pid because its command does not match $BIN." ;;
    esac
  fi
  kill "$pid"
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.5
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid"
    sleep 0.1
  fi
  if kill -0 "$pid" 2>/dev/null; then
    die "Managed ForkMesh daemon PID $pid did not stop."
  fi
  rm -f -- "$PID_FILE"
  say "Stopped managed ForkMesh daemon PID $pid"
}

# An interrupted age/Git materialization cannot run its QTemporaryDir cleanup,
# so repeated service restarts can leave gigabytes of disposable mirror clones
# behind. Remove only ForkMesh's two runtime-only prefixes, only from the
# installer-owned service state directory, and only while the managed service
# has already been stopped. Agent workspaces (forkmesh-agent-*) and all durable
# repositories/archives are deliberately outside this match.
cleanup_stale_service_mirror_tmp() {
  [ "$(id -u)" -eq 0 ] || return 0
  [ -f "$SYSTEMD_STATE_MARKER" ] || return 0
  [ -d "$SYSTEMD_STATE_DIR/tmp" ] || return 0
  [ ! -L "$SYSTEMD_STATE_DIR/tmp" ] ||
    die "Refusing symlink managed service temp directory."
  find "$SYSTEMD_STATE_DIR/tmp" -mindepth 1 -maxdepth 1 -type d \
    \( -name 'ForkMesh-*' -o -name 'forkmesh-mirror-runtime-*' \) \
    -exec rm -rf -- {} +
}

uninstall_forkmesh() {
  local mode="${1:-full}"; shift || true
  local data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
  local config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
  local cache_home="${XDG_CACHE_HOME:-$HOME/.cache}"
  local purge="${FORKMESH_PURGE:-0}" assume_yes="${FORKMESH_ASSUME_YES:-0}" arg
  for arg in "$@"; do
    case "$arg" in
      --purge) purge=1 ;;
      --yes|-y) assume_yes=1 ;;
    esac
  done
  # A reinstall is the explicit wipe-and-replace action.
  if [ "$mode" = "reinstall" ]; then purge=1; fi

  # Directories ForkMesh owns. QSettings org+app are both "ForkMesh", so the
  # config/data/cache live under a capitalised "ForkMesh" dir; the installer's
  # own checkout lives under the lowercase "forkmesh".
  # In the projects layout the same things live in ~/projects/forkmesh. The
  # project folder itself is never removed: it also holds src/, and may hold
  # files of the user's own.
  local layout_home="${FORKMESH_HOME:-${FORKMESH_PROJECTS_DIR:-$HOME/projects}/forkmesh}"
  local dirs=(
    "$cache_home/ForkMesh"          # caches
    "$layout_home/cache"
  )
  # User data: only erased with --purge.
  local data_dirs=(
    "$config_home/ForkMesh"          # settings (node name, server, prefs)
    "$data_home/ForkMesh"            # identity key, mirrors, repos, chat, actions
    "$HOME/.forkmesh"               # diagnostics and local runtime state
    "$layout_home/data"             # the same, in the projects layout
    "$layout_home/config"
    "$layout_home/ssh"
    "$layout_home/state"
    "$layout_home/logs"
  )
  if [ "$purge" = "1" ]; then dirs+=("${data_dirs[@]}"); fi
  # Nothing has been touched yet: refuse here, before the prompt or any stop.
  guard_user_data_wipe "$purge" "${dirs[@]}"
  # Loose files: binary, desktop launcher, autostart entry, installed icons.
  local files=(
    "$BIN"
    "$data_home/applications/forkmesh.desktop"
    "$config_home/autostart/forkmesh.desktop"
    "$data_home/icons/forkmesh.png"
    "$data_home/nautilus/scripts/Open with ForkMesh"
    "$data_home/kio/servicemenus/forkmesh-open.desktop"
    "$data_home/nemo/actions/forkmesh-open.nemo_action"
  )

  if [ "$mode" = "reinstall" ]; then
    printf '\033[31mReinstall: removing the existing ForkMesh install and ALL of its data first:\033[0m\n'
  elif [ "$purge" = "1" ]; then
    printf '\033[31mThis removes ForkMesh and ALL of its data from this computer:\033[0m\n'
  else
    printf 'This removes the ForkMesh program from this computer:\n'
    printf '  • the forkmesh binary and caches\n'
    printf '  • the desktop launcher, icons, and login-autostart entry\n'
    printf 'Your settings, node identity key, repositories, and chat history are kept.\n\n'
  fi
  if [ "$purge" = "1" ]; then
    printf '  • the forkmesh binary\n'
    printf '  • settings, the node identity key (the account cannot be recovered)\n'
    printf '  • every mirrored repository, and all chat history\n'
    printf '  • the desktop launcher, icons, and login-autostart entry\n\n'
  fi

  # Honour a non-interactive confirm so `curl | bash` works: pass --yes (or set
  # FORKMESH_ASSUME_YES=1). Otherwise prompt when a terminal is attached. Only
  # an erase of user data needs it; a reinstall skips this gate entirely — it is
  # already an explicit, pre-confirmed action (the Qt-side dialog / the
  # --reinstall flag).
  if [ "$purge" = "1" ] && [ "$mode" != "reinstall" ] && [ "$assume_yes" != "1" ]; then
    if [ -t 0 ]; then
      printf 'Type DELETE to continue: '
      local answer=""; read -r answer || answer=""
      [ "$answer" = "DELETE" ] || die "Uninstall cancelled."
    else
      die "Refusing to erase ForkMesh data without confirmation. Re-run with --yes (or set FORKMESH_ASSUME_YES=1) to proceed:  curl -fsSL $FORKMESH_HOST/install.sh | bash -s -- --uninstall --purge --yes"
    fi
  fi

  # Compatibility cleanup for old installer-owned desktop daemons. New installs
  # never create this PID-file process; keeping the bounded exact-PID cleanup
  # ensures uninstall removes the retired server rather than leaving it alive.
  stop_forkmesh_daemons "installer uninstall"

  if [ "$(id -u)" -eq 0 ] && [ -f "$SYSTEMD_UNIT" ] &&
     grep -Fqx '# Managed-By: ForkMesh installer' "$SYSTEMD_UNIT"; then
    systemctl disable forkmesh-mirror-node.service >/dev/null 2>&1 || true
    rm -f -- "$SYSTEMD_UNIT" "$SYSTEMD_ENV"
    systemctl daemon-reload >/dev/null 2>&1 || true
    if [ -f "$SYSTEMD_INSTALL_MARKER" ] &&
       grep -Fqx "binary=$SYSTEMD_BIN" "$SYSTEMD_INSTALL_MARKER"; then
      _managed_hash="$(sed -n 's/^sha256=//p' "$SYSTEMD_INSTALL_MARKER" | head -n1)"
      _current_hash="$(_sha256_file "$SYSTEMD_BIN")"
      if [ -n "$_managed_hash" ] && [ "$_managed_hash" = "$_current_hash" ]; then
        rm -f -- "$SYSTEMD_BIN"
      else
        warn "Managed service binary changed since install; preserving $SYSTEMD_BIN."
      fi
      rm -f -- "$SYSTEMD_INSTALL_MARKER"
    fi
    if [ "$purge" != "1" ]; then
      if [ -e "$SYSTEMD_STATE_DIR" ]; then
        say "Kept $SYSTEMD_STATE_DIR (node identity and repositories)"
      fi
    elif [ -f "$SYSTEMD_STATE_MARKER" ] &&
       [ ! -L "$SYSTEMD_STATE_MARKER" ] &&
       [ "$(_path_owner_uid "$SYSTEMD_STATE_MARKER")" = "0" ] &&
       grep -Fqx 'forkmesh-managed-service-v1' "$SYSTEMD_STATE_MARKER" &&
       grep -Fqx "path=$SYSTEMD_STATE_DIR" "$SYSTEMD_STATE_MARKER"; then
      rm -rf -- "$SYSTEMD_STATE_DIR"
    else
      warn "Managed service state marker is missing or invalid; preserving $SYSTEMD_STATE_DIR."
    fi
  fi

  local d f
  if [ -e "$SRC" ]; then
    say "Kept the source checkout $SRC (a git checkout with your branches); delete it yourself if you no longer need it."
  fi
  for d in "${dirs[@]}"; do
    [ -e "$d" ] || continue
    if [ "$purge" = "1" ]; then
      rm -rf -- "$d" && say "Removed $d"
    else
      fm_safe_rm_tree "$(dirname -- "$d")" "$d" && say "Removed $d"
    fi
  done
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
  if [ "$mode" = "reinstall" ]; then
    say "Previous ForkMesh install removed; installing a fresh copy now."
    return 0
  fi
  if [ "$purge" = "1" ]; then
    say "ForkMesh has been completely removed."
  else
    for d in "${data_dirs[@]}"; do
      if [ -e "$d" ]; then say "Kept $d"; fi
    done
    say "ForkMesh has been removed; your identity and repositories were kept."
    say "Erase them too with:  curl -fsSL $FORKMESH_HOST/install.sh | bash -s -- --uninstall --purge"
  fi
  exit 0
}

# Dispatch uninstall before any install work (and before the diag EXIT trap can
# misreport a clean uninstall as a failed install step). --reinstall wipes the
# old install then falls through into the normal install below (adhoc #258).
for arg in "$@"; do
  case "$arg" in
    --uninstall|--remove|-u) CURRENT_STEP="uninstall"; trap - EXIT
      uninstall_forkmesh full "$@" ;;
    --reinstall) FORKMESH_REINSTALL=1; shift || true ;;
  esac
done
if [ "$FORKMESH_REINSTALL" = "1" ]; then
  if [ -n "$FORKMESH_EXPECTED_BUILD_COMMIT" ]; then
    DEFER_PINNED_REINSTALL=1
    say "Reinstall requested — validating the replacement before clearing the existing node."
  else
    CURRENT_STEP="reinstall"
    say "Reinstall requested — clearing the existing install before reinstalling."
    uninstall_forkmesh reinstall
  fi
fi

# Plain ASCII box (not Unicode box-drawing): the box-drawing characters are
# "ambiguous width" in Unicode, so non-UTF-8 terminals/consoles (and some
# CJK-locale fonts) render them double-width or as mojibake, breaking the
# alignment against the fixed-width version field below.
printf '\033[32m+-----------------------------------------------+\033[0m\n'
printf '\033[32m|\033[0m  ForkMesh installer  \033[2mv%-24s\033[0m\033[32m|\033[0m\n' "$INSTALLER_VERSION"
printf '\033[32m+-----------------------------------------------+\033[0m\n'
say "Host:   $FORKMESH_HOST"
say "Source: ${SRC}"
say "Target: ${BIN}"
[ -n "$FORKMESH_NODE_NAME" ] && say "Node:   $FORKMESH_NODE_NAME"
# Echo the account this node is being attached to (adhoc #258) so the operator
# can confirm, right here in the install output, that it will end up under the
# right owner rather than registering as an orphan node.
[ -n "$FORKMESH_OWNER" ] && say "Owner:  $FORKMESH_OWNER  (this node will be attached to this account)"
if [ "$(id -u)" -eq 0 ]; then
  say "Privileges: running as root (no sudo needed)"
else
  say "Privileges: non-root; will use sudo/doas for package installs"
fi
diag start 1

# Skip resolving a mirror up front when a binary is being streamed straight
# onto this machine over the SSH session (adhoc #67 direct-upload install):
# nothing needs to be cloned unless that upload later turns out to be
# unusable, in which case ensure_mirror_candidates resolves one lazily.
if [ -z "$REPO" ] && [ -z "$FORKMESH_LOCAL_BINARY" ]; then
  ensure_mirror_candidates
fi
# An explicit FORKMESH_REPO (or the override path above leaving it unset) means
# there is exactly one URL to try; make it the sole candidate so clean_clone has
# a non-empty list to iterate.
[ "${#REPO_CANDIDATES[@]}" -eq 0 ] && [ -n "$REPO" ] && REPO_CANDIDATES=("$REPO")

# --- privilege escalation ---------------------------------------------------
# Resolve how to run a package manager that needs root. Empty when we are
# already root; otherwise prefer sudo, then doas.
SUDO=""
need_sudo() {
  if [ "$(id -u)" -eq 0 ]; then
    SUDO=""
  elif command -v sudo >/dev/null 2>&1; then
    SUDO="sudo"
  elif command -v doas >/dev/null 2>&1; then
    SUDO="doas"
  else
    return 1
  fi
  return 0
}

# --- package manager detection ----------------------------------------------
# Sets PM to the detected manager and PM_INSTALL to the command (as an array)
# that installs packages non-interactively.
PM=""
PM_INSTALL=()
PM_UPDATED=0
detect_pm() {
  if   command -v apt-get >/dev/null 2>&1; then PM="apt";    PM_INSTALL=(apt-get install -y)
  elif command -v dnf     >/dev/null 2>&1; then PM="dnf";    PM_INSTALL=(dnf install -y)
  elif command -v yum     >/dev/null 2>&1; then PM="yum";    PM_INSTALL=(yum install -y)
  elif command -v pacman  >/dev/null 2>&1; then PM="pacman"; PM_INSTALL=(pacman -S --noconfirm --needed)
  elif command -v zypper  >/dev/null 2>&1; then PM="zypper"; PM_INSTALL=(zypper install -y)
  elif command -v apk     >/dev/null 2>&1; then PM="apk";    PM_INSTALL=(apk add)
  elif command -v brew    >/dev/null 2>&1; then PM="brew";   PM_INSTALL=(brew install)
  else return 1
  fi
  return 0
}

# Refresh package indexes once, for managers that need it before install.
pm_refresh() {
  [ "$PM_UPDATED" -eq 1 ] && return 0
  PM_UPDATED=1
  case "$PM" in
    apt)    run_pm apt-get update ;;
    pacman) run_pm pacman -Sy ;;
  esac
}

# Run a package-manager command with escalation when required (brew must not
# run as root). Echoes the exact command (including any sudo/doas prefix) so the
# install log shows precisely what is being executed and with what privileges.
#
# stdin is closed for every package manager: they are all driven
# non-interactively (-y / --noconfirm), so a prompt reaching apt/dpkg/debconf
# has nothing useful to read anyway, and letting one inherit our stdin means it
# either hangs on a terminal or — under `curl | bash` — swallows part of this
# script (issue #659; see the brace group at the top).
run_pm() {
  if [ "$PM" = "brew" ]; then
    say "Running: $*"
    "$@" </dev/null
  else
    say "Running: ${SUDO:+$SUDO }$*"
    $SUDO "$@" </dev/null
  fi
}

# Install one or more packages by name.
pm_install() {
  # Resolve privilege escalation before touching the package manager. Every
  # install path funnels through here, so this guarantees $SUDO is set even when
  # the caller didn't go through ensure() (e.g. the Qt/OpenSSL block below, which
  # runs unconditionally). Without this, apt-get runs unprivileged and fails with
  # "Could not open lock file ... Permission denied" on a fresh machine.
  if [ "$PM" != "brew" ] && ! need_sudo; then
    die "Installing packages via $PM needs root, but neither sudo nor doas is available. Re-run as root, or install the build dependencies manually and re-run with FORKMESH_NO_INSTALL_DEPS=1."
  fi
  pm_refresh
  run_pm "${PM_INSTALL[@]}" "$@"
}

# --- prerequisites ----------------------------------------------------------
# Map a logical prerequisite to the package providing it for the detected PM.
# Echoes the package name, or nothing when there is no candidate to install.
pkg_for() {
  local what="$1"
  case "$PM:$what" in
    apt:git)        echo git ;;
    apt:age)        echo age ;;
    apt:tar)        echo tar ;;
    apt:cmake)      echo cmake ;;
    apt:compiler)   echo "g++" ;;
    # qt6-base-dev-tools provides the Qt build tools, while pkg-config is used
    # by have_qt6_dev to verify the installed Qt modules.
    apt:qt)         echo "qt6-base-dev qt6-svg-dev qt6-base-dev-tools pkg-config" ;;
    apt:openssl)    echo libssl-dev ;;

    dnf:git|yum:git)            echo git ;;
    dnf:age|yum:age)            echo age ;;
    dnf:tar|yum:tar)            echo tar ;;
    dnf:cmake|yum:cmake)        echo cmake ;;
    dnf:compiler|yum:compiler)  echo "gcc-c++" ;;
    dnf:qt|yum:qt)              echo "qt6-qtbase-devel qt6-qtsvg-devel" ;;
    dnf:openssl|yum:openssl)    echo openssl-devel ;;

    pacman:git)       echo git ;;
    pacman:age)       echo age ;;
    pacman:tar)       echo tar ;;
    pacman:cmake)     echo cmake ;;
    pacman:compiler)  echo gcc ;;
    pacman:qt)        echo "qt6-base qt6-svg" ;;
    pacman:openssl)   echo openssl ;;

    zypper:git)       echo git ;;
    zypper:age)       echo age ;;
    zypper:tar)       echo tar ;;
    zypper:cmake)     echo cmake ;;
    zypper:compiler)  echo "gcc-c++" ;;
    zypper:qt)        echo "qt6-base-devel qt6-svg-devel" ;;
    zypper:openssl)   echo libopenssl-devel ;;

    apk:git)       echo git ;;
    apk:age)       echo age ;;
    apk:tar)       echo tar ;;
    apk:cmake)     echo "cmake make" ;;
    apk:compiler)  echo "g++" ;;
    apk:qt)        echo "qt6-qtbase-dev qt6-qtsvg-dev" ;;
    apk:openssl)   echo "openssl-dev" ;;

    brew:git)       echo git ;;
    brew:age)       echo age ;;
    brew:tar)       echo gnu-tar ;;
    brew:cmake)     echo cmake ;;
    brew:qt)        echo qt ;;
    brew:openssl)   echo "openssl@3" ;;
    brew:compiler)  echo "" ;;  # provided by Xcode CLT, handled separately
  esac
}

# Install a small set of runtime prerequisites without a system package
# manager. A fresh macOS installation has curl, tar, and shasum but commonly
# has neither Homebrew nor MacPorts. Requiring users to install a package
# manager just to obtain age made the otherwise self-contained prebuilt path
# fail before it could download ForkMesh.
#
# Keep this deliberately narrow and fail closed: downloads are version-pinned,
# selected only for architectures published by the official age project, and
# checked against the SHA-256 values on its signed GitHub release before any
# extracted executable is installed.
fallback_installer_for() {
  local what="$1"
  case "$(uname -s 2>/dev/null || true):$what" in
    Darwin:age) echo "official age binary" ;;
  esac
}

install_macos_age() {
  local version="1.3.1" arch archive_name expected url tmp archive got
  local extracted_age extracted_keygen staged_age staged_keygen

  arch="$(uname -m 2>/dev/null || true)"
  case "$arch" in
    arm64|aarch64)
      arch="arm64"
      expected="01120ea2cbf0463d4c6bd767f99f3271bbed1cdc8a9aa718a76ba1fe4f01998b"
      ;;
    x86_64|amd64)
      arch="amd64"
      expected="2b233301ad21ab7b1eabd9ae1198a164005fa4928fcdd745d47c39f8593209d7"
      ;;
    *)
      warn "The official age release has no supported macOS archive for architecture '$arch'."
      return 1
      ;;
  esac

  command -v curl >/dev/null 2>&1 || {
    warn "curl is required to download age on macOS."
    return 1
  }
  command -v tar >/dev/null 2>&1 || {
    warn "tar is required to unpack age on macOS."
    return 1
  }
  command -v sha256sum >/dev/null 2>&1 ||
    command -v shasum >/dev/null 2>&1 || {
    warn "A working shasum or sha256sum is required to verify age."
    return 1
  }

  archive_name="age-v${version}-darwin-${arch}.tar.gz"
  url="https://github.com/FiloSottile/age/releases/download/v${version}/${archive_name}"
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/forkmesh-age.XXXXXX" 2>/dev/null)" || return 1
  archive="$tmp/$archive_name"

  say "Downloading age v${version} for macOS/${arch}…"
  if ! curl -fsSL --retry 2 --connect-timeout 15 "$url" -o "$archive" </dev/null; then
    warn "Could not download the official age archive."
    fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"
    return 1
  fi
  got="$(_sha256_file "$archive")"
  if [ "$got" != "$expected" ]; then
    warn "The downloaded age archive failed its SHA-256 check."
    fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"
    return 1
  fi
  if ! tar -xzf "$archive" -C "$tmp" \
       "age/age" "age/age-keygen" </dev/null; then
    warn "Could not unpack the verified age archive."
    fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"
    return 1
  fi

  extracted_age="$tmp/age/age"
  extracted_keygen="$tmp/age/age-keygen"
  [ -f "$extracted_age" ] && [ ! -L "$extracted_age" ] &&
    [ -f "$extracted_keygen" ] && [ ! -L "$extracted_keygen" ] || {
      warn "The verified age archive did not contain the expected tools."
      fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"
      return 1
    }

  mkdir -p "$BIN_DIR" || { fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"; return 1; }
  staged_age="$(mktemp "$BIN_DIR/.age-install.XXXXXX" 2>/dev/null)" || {
    fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"
    return 1
  }
  staged_keygen="$(mktemp "$BIN_DIR/.age-keygen-install.XXXXXX" 2>/dev/null)" || {
    rm -f "$staged_age"
    fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"
    return 1
  }
  if ! install -m 0755 "$extracted_age" "$staged_age" ||
     ! install -m 0755 "$extracted_keygen" "$staged_keygen" ||
     ! mv -f "$staged_age" "$BIN_DIR/age" ||
     ! mv -f "$staged_keygen" "$BIN_DIR/age-keygen"; then
    warn "Could not install age and age-keygen into $BIN_DIR."
    rm -f "$staged_age" "$staged_keygen"
    fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"
    return 1
  fi
  fm_safe_rm_tree "${TMPDIR:-/tmp}" "$tmp"
  export PATH="$BIN_DIR:$PATH"
  say "Installed age and age-keygen to $BIN_DIR"
}

# Ensure a prerequisite is present, installing it if missing and possible.
# Args: <logical name> <test command...>
ensure() {
  local what="$1" fallback; shift
  if "$@" >/dev/null 2>&1; then
    say "  $what: already present"
    return 0
  fi

  if [ "${FORKMESH_NO_INSTALL_DEPS:-0}" = "1" ]; then
    die "'$what' is required but not installed (auto-install disabled via FORKMESH_NO_INSTALL_DEPS)."
  fi
  if [ -z "$PM" ]; then
    fallback="$(fallback_installer_for "$what")"
    if [ -n "$fallback" ]; then
      say "Installing missing prerequisite: $what ($fallback)"
      DIAG_MISSING="${DIAG_MISSING:+$DIAG_MISSING }$what"
      case "$what" in
        age) install_macos_age || die "Failed to install age on macOS." ;;
        *) die "Internal error: no fallback installer is implemented for '$what'." ;;
      esac
      "$@" >/dev/null 2>&1 ||
        die "Installed $what but its command is still unavailable from PATH."
      return 0
    fi
    die "'$what' is required but not installed, and no supported package manager was found to install it."
  fi
  if ! need_sudo && [ "$PM" != "brew" ]; then
    die "'$what' is missing and installing it needs root, but neither sudo nor doas is available."
  fi

  local pkgs; pkgs="$(pkg_for "$what")"
  [ -n "$pkgs" ] || die "'$what' is required but no package candidate is known for '$PM'."

  say "Installing missing prerequisite: $what ($pkgs)"
  DIAG_MISSING="${DIAG_MISSING:+$DIAG_MISSING }$what"
  # shellcheck disable=SC2086
  pm_install $pkgs || die "Failed to install $pkgs via $PM."

  "$@" >/dev/null 2>&1 || die "Installed $pkgs but the check for '$what' still fails. If it is already present in a custom prefix, export PATH/CMAKE_PREFIX_PATH so the check can see it, then re-run."
}

# A C++ compiler can come from any of cc/clang/g++ (or Xcode CLT on macOS).
have_compiler() {
  command -v cc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 \
    || command -v g++ >/dev/null 2>&1 || command -v c++ >/dev/null 2>&1
}

# pkg-config is not guaranteed to be installed: minimal Debian/Alpine images
# ship neither it nor Debian's `pkgconf` rename of it, and nothing in the build
# actually needs it. Run whichever binary exists; returning 1 when neither does
# means "unknown", which callers must never read as "the library is missing".
pkg_config_probe() {
  if command -v pkg-config >/dev/null 2>&1; then
    pkg-config "$@"
  elif command -v pkgconf >/dev/null 2>&1; then
    pkgconf "$@"
  else
    return 1
  fi
}

# Look for the CMake package files that desktop's find_package(Qt6 COMPONENTS
# Widgets ... Svg) actually resolves through, in the prefixes CMake searches:
# explicit CMAKE_PREFIX_PATH / Qt6_DIR hints first, then the standard system lib
# dirs — plain lib (Arch, Alpine), lib64 (Fedora, openSUSE), multiarch
# lib/<triplet> (Debian, Ubuntu) and Homebrew's keg-only opt prefix. Widgets and
# Svg come from different distro packages (qt6-base-dev vs qt6-svg-dev), so both
# are checked. This needs no pkg-config, compiler or build tool, so it stays
# accurate on a host the installer has not finished setting up.
have_qt6_cmake_package() {
  local root cfg cmake_dir module found
  local modules="${*:-Widgets Svg}"
  # shellcheck disable=SC2086
  for root in ${CMAKE_PREFIX_PATH:+${CMAKE_PREFIX_PATH//[:;]/ }} \
              ${Qt6_DIR:+$Qt6_DIR} \
              /usr /usr/local /opt/homebrew/opt/qt /usr/local/opt/qt; do
    for cfg in "$root"/Qt6Config.cmake \
               "$root"/lib/cmake/Qt6/Qt6Config.cmake \
               "$root"/lib64/cmake/Qt6/Qt6Config.cmake \
               "$root"/lib/*/cmake/Qt6/Qt6Config.cmake; do
      [ -f "$cfg" ] || continue
      cmake_dir="$(dirname "$(dirname "$cfg")")"
      found=1
      for module in $modules; do
        [ -f "$cmake_dir/Qt6$module/Qt6${module}Config.cmake" ] || found=0
      done
      [ "$found" = 1 ] && return 0
    done
  done
  return 1
}

have_qt6_dev() {
  # Cheap probe first: the .pc files the Qt 6 development packages ship, for the
  # same modules the build links against.
  local mods="Qt6Widgets Qt6Network Qt6Svg Qt6Concurrent"
  if [ "$(uname -s)" = "Linux" ]; then
    # Linux additionally needs Qt's D-Bus module for portal screen capture.
    mods="$mods Qt6DBus"
  fi
  # shellcheck disable=SC2086
  if pkg_config_probe --exists $mods 2>/dev/null; then
    return 0
  fi
  # A host with no pkg-config (or a Qt build that ships no .pc files) must not
  # read as "Qt 6 is missing": that made the installer reinstall the dev packages
  # the package manager already had, re-probe with the same broken check and die
  # with "Installed qt6-base-dev qt6-svg-dev but 'qt' is still unavailable" on a
  # machine that was ready to build. Look for the CMake packages instead.
  have_qt6_cmake_package
}

have_qt6_webengine() {
  if pkg_config_probe --exists Qt6WebEngineWidgets 2>/dev/null; then
    return 0
  fi
  have_qt6_cmake_package WebEngineWidgets
}

have_openssl_dev() {
  if pkg_config_probe --exists openssl 2>/dev/null; then
    return 0
  fi
  [ -f /usr/include/openssl/ssl.h ] ||
    [ -f /usr/local/include/openssl/ssl.h ]
}

have_age_tools() {
  command -v age >/dev/null 2>&1 &&
    command -v age-keygen >/dev/null 2>&1
}

# Prebuilt/uploaded binaries skip the source-build pipeline (and thus its Qt 6
# dependency install), but the binary is dynamically linked against the Qt 6
# runtime libraries (libQt6Widgets/Gui/Core/Network/Svg) and will not even start
# without them — it dies at exec with "error while loading shared libraries:
# libQt6Widgets.so.6: cannot open shared object file". This bit fresh headless
# servers that had never had Qt installed: the node "installed" but the daemon
# never launched, so it never registered, connected, or served the repo. Ensure
# the Qt 6 runtime is present after a prebuilt install so the node actually comes
# up. The qt6 dev metapackages depend on the runtime libs (and the offscreen QPA
# plugin the headless daemon needs) and resolve on both pre- and post-t64 Debian,
# so we reuse them rather than chase the version-specific runtime package names.
# macOS prebuilds bundle their frameworks, so this only applies to Linux with a
# package manager.
ensure_qt_runtime() {
  [ "$(uname -s)" = "Linux" ] || return 0
  if [ "$(id -u)" -eq 0 ] && [ -z "${DISPLAY:-}" ] &&
     [ -z "${WAYLAND_DISPLAY:-}" ] &&
     [ -n "${FORKMESH_TUNNEL_HOSTNAME:-}" ]; then
    say "  Qt runtime: not needed by the native mirror service"
    return 0
  fi
  # Already have the Qt runtime libraries? Nothing to do.
  if command -v ldconfig >/dev/null 2>&1 \
     && ldconfig -p 2>/dev/null | grep -q 'libQt6Widgets\.so\.6'; then
    say "  Qt 6 runtime: already present"
    return 0
  fi
  if [ "${FORKMESH_NO_INSTALL_DEPS:-0}" = "1" ]; then
    warn "Qt 6 runtime libraries appear to be missing and auto-install is disabled (FORKMESH_NO_INSTALL_DEPS); the prebuilt binary may fail to start until they are installed (e.g. apt install qt6-base-dev qt6-svg-dev)."
    return 0
  fi
  if [ -z "$PM" ]; then
    warn "Qt 6 runtime libraries appear to be missing and no supported package manager was found to install them; the prebuilt binary may fail to start (install Qt 6 base + svg runtime, then re-run: $BIN)."
    return 0
  fi
  local qt_pkgs; qt_pkgs="$(pkg_for qt)"
  [ -n "$qt_pkgs" ] || { warn "No Qt 6 runtime package candidate is known for '$PM'."; return 0; }
  say "Installing Qt 6 runtime libraries ($qt_pkgs) for the prebuilt binary"
  # shellcheck disable=SC2086
  pm_install $qt_pkgs || die "Failed to install the Qt 6 runtime ($qt_pkgs) via $PM; the prebuilt binary cannot start without it."
}

have_colour_emoji_font() {
  command -v fc-list >/dev/null 2>&1 &&
    fc-list : family 2>/dev/null | grep -qiE 'color emoji|coloremoji|twemoji|openmoji|joypixels'
}

emoji_font_package() {
  case "$PM" in
    apt)     echo fonts-noto-color-emoji ;;
    dnf|yum) echo google-noto-color-emoji-fonts ;;
    pacman)  echo noto-fonts-emoji ;;
    zypper)  echo noto-coloremoji-fonts ;;
    apk)     echo font-noto-emoji ;;
  esac
}

webengine_package() {
  case "$PM" in
    apt)     echo qt6-webengine-dev ;;
    dnf|yum) echo qt6-qtwebengine-devel ;;
    pacman)  echo qt6-webengine ;;
    zypper)  echo qt6-webengine-devel ;;
    apk)     echo qt6-qtwebengine-dev ;;
  esac
}

ensure_webengine() {
  [ "$(uname -s)" = "Linux" ] || return 0
  if [ "$(id -u)" -eq 0 ] && [ -z "${DISPLAY:-}" ] &&
     [ -z "${WAYLAND_DISPLAY:-}" ] &&
     [ -n "${FORKMESH_TUNNEL_HOSTNAME:-}" ]; then
    return 0
  fi
  if have_qt6_webengine; then
    say "  Qt WebEngine: already present"
    return 0
  fi
  local pkg; pkg="$(webengine_package)"
  if [ "${FORKMESH_NO_INSTALL_DEPS:-0}" = "1" ] || [ -z "$pkg" ] ||
     { [ "$PM" != "brew" ] && ! need_sudo; }; then
    warn "Qt WebEngine is not installed, so YouTube, the Browser tab and agent browser windows open outside ForkMesh. Install your distribution's Qt 6 WebEngine development package${pkg:+ ($pkg)} and rebuild."
    return 0
  fi
  say "Installing Qt WebEngine ($pkg) so YouTube and web pages play inside ForkMesh"
  pm_install "$pkg" ||
    warn "Could not install $pkg via $PM; YouTube, the Browser tab and agent browser windows open outside ForkMesh until it is installed."
}

ensure_emoji_font() {
  [ "$(uname -s)" = "Linux" ] || return 0
  if [ "$(id -u)" -eq 0 ] && [ -z "${DISPLAY:-}" ] &&
     [ -z "${WAYLAND_DISPLAY:-}" ] &&
     [ -n "${FORKMESH_TUNNEL_HOSTNAME:-}" ]; then
    return 0
  fi
  if have_colour_emoji_font; then
    say "  Colour emoji font: already present"
    return 0
  fi
  local pkg; pkg="$(emoji_font_package)"
  if [ "${FORKMESH_NO_INSTALL_DEPS:-0}" = "1" ] || [ -z "$pkg" ] ||
     { [ "$PM" != "brew" ] && ! need_sudo; }; then
    warn "No colour emoji font is installed, so emoji in ForkMesh draw in monochrome. Install your distribution's Noto Color Emoji package${pkg:+ ($pkg)}."
    return 0
  fi
  say "Installing the colour emoji font ($pkg)"
  pm_install "$pkg" ||
    warn "Could not install $pkg via $PM; emoji in ForkMesh draw in monochrome until a colour emoji font is installed."
}

if detect_pm; then
  say "Detected package manager: $PM (${PM_INSTALL[*]})"
else
  warn "No supported package manager found; missing tools cannot be auto-installed."
fi

CURRENT_STEP="deps"
say "Checking runtime prerequisites (Git, age, age-keygen, tar)…"
ensure git command -v git
ensure age have_age_tools
ensure tar command -v tar

# --- prebuilt release binary (fast path) ------------------------------------
# Resolve this machine's release asset name from uname. The release workflow
# (.forkmesh/release.yml) names every attached build forkmesh-<os>-<arch> (with
# a .exe suffix on Windows), so the installer can pick the right one with no
# server round-trip beyond the clone the build path already needs.
detect_release_asset() {
  local os arch
  os="$(uname -s 2>/dev/null || echo unknown)"
  arch="$(uname -m 2>/dev/null || echo unknown)"
  case "$os" in
    Linux)                            os="linux" ;;
    Darwin)                           os="macos" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)  os="windows" ;;
    *) os="$(printf '%s' "$os" | tr '[:upper:]' '[:lower:]')" ;;
  esac
  case "$arch" in
    x86_64|amd64|x64) arch="x86_64" ;;
    aarch64|arm64)    arch="arm64" ;;
  esac
  ASSET_OS="$os"
  ASSET_ARCH="$arch"
  ASSET_NAME="forkmesh-${os}-${arch}"
  [ "$os" = "windows" ] && ASSET_NAME="${ASSET_NAME}.exe"
  # Release channel: the directory under .forkmesh/releases/ the workflow publishes into.
  RELEASE_CHANNEL="${FORKMESH_RELEASE:-latest}"
  ASSET_REL_PATH=".forkmesh/releases/${RELEASE_CHANNEL}/${ASSET_NAME}"
}

# Sparse-fetch a single committed file (repo-relative path $3) from clone URL $1
# into dir $2, without checking out the whole tree. Prefers a blobless clone
# (fetches only that one blob); if the mirror/relay does not honour partial-clone
# filters, retries with a plain shallow clone so the fast path still works (it
# just transfers more). Leaves the file at "$2/$3" on success.
#
# forkmesh.com redirects every git HTTP request to one of several mirrors, and
# the blobless clone's lazy blob fetch during checkout is a SEPARATE request
# that can land on a mirror whose gateway is restarting (HTTP 503) — often while
# every mirror restarts in the same ~15 s window. So when neither mode gets
# through, the whole fetch is retried after each delay in
# FORKMESH_METADATA_RETRY_DELAYS (seconds, default "5 10 20": 4 attempts over
# ~35 s), announcing each retry so the install doesn't look hung.
#
# Sets SPARSE_FETCH_STATUS on return: "ok"; "missing" (a checkout completed but
# the required file is not in the tree — authoritative, never retried); or
# "unreachable" (no attempt got a checkout through). SPARSE_FETCH_REASON holds
# git's last error line and SPARSE_FETCH_ATTEMPTS the attempts made.
_sparse_fetch_file() {
  local repo="$1" tmp="$2"; shift 2
  # The first path is required — success is gated on it being present and
  # non-empty. Any extra paths are fetched best-effort in the SAME checkout (e.g.
  # release.json alongside SHASUMS256.txt), so the canonical owner/repo can be
  # read without a second clone.
  local paths=("$@") mode err rc delay attempt=1 total checked_out
  local -a delays=()
  read -r -a delays <<<"${FORKMESH_METADATA_RETRY_DELAYS-5 10 20}" || true
  total=$(( ${#delays[@]} + 1 ))
  SPARSE_FETCH_STATUS=""
  SPARSE_FETCH_REASON=""
  SPARSE_FETCH_ATTEMPTS=0
  while :; do
    SPARSE_FETCH_ATTEMPTS="$attempt"
    checked_out=0
    for mode in "--filter=blob:none" ""; do
      _rm_install_scratch "$tmp" || return 1; mkdir -p "$tmp" || return 1
      # Keep git's stderr (only) so a failure can say why; stdout is discarded.
      # shellcheck disable=SC2086
      err="$(
        { git clone --quiet --depth 1 $mode --no-checkout "$repo" "$tmp" \
            && git -C "$tmp" sparse-checkout set --no-cone "${paths[@]}" \
            && git -C "$tmp" checkout --quiet; } 2>&1 >/dev/null </dev/null
      )" && rc=0 || rc=$?
      if [ "$rc" -eq 0 ]; then
        checked_out=1
        if [ -s "$tmp/${paths[0]}" ]; then
          SPARSE_FETCH_STATUS="ok"
          return 0
        fi
      else
        # Prefer the HTTP status line ("The requested URL returned error: 503")
        # over the generic "could not fetch ... from promisor remote" that
        # follows it.
        SPARSE_FETCH_REASON="$(printf '%s\n' "$err" | awk '
          /returned error:/ { http = $0 }
          /fatal:|error:/   { last = $0 }
          END { line = (http != "" ? http : last); sub(/^[[:space:]]*/, "", line); print substr(line, 1, 200) }')"
      fi
    done
    if [ "$checked_out" = "1" ]; then
      # The mirror answered and the tree simply lacks the file: retrying cannot
      # change that.
      SPARSE_FETCH_STATUS="missing"
      return 1
    fi
    if [ "$attempt" -ge "$total" ]; then
      SPARSE_FETCH_STATUS="unreachable"
      return 1
    fi
    delay="${delays[$((attempt - 1))]}"
    case "$delay" in ''|*[!0-9]*) delay=0 ;; esac
    warn "Could not fetch from $repo${SPARSE_FETCH_REASON:+ ($SPARSE_FETCH_REASON)}; a ForkMesh mirror may be restarting. Retrying in ${delay}s (attempt $((attempt + 1)) of $total)…"
    sleep "$delay"
    attempt=$((attempt + 1))
  done
}

_rm_install_scratch() {
  local scratch="$1" base dir
  [ -n "$scratch" ] || return 0
  [ -e "$scratch" ] || [ -L "$scratch" ] || return 0
  base="$(cd -P -- "${TMPDIR:-/tmp}" 2>/dev/null && pwd -P)" || return 1
  dir="$(cd -P -- "$(dirname -- "$scratch")" 2>/dev/null && pwd -P)" || return 1
  case "$dir/${scratch##*/}" in
    "$base"/forkmesh-prebuilt.??????|"$base"/forkmesh-tools.??????) rm -rf -- "$scratch" ;;
    *) warn "Refusing to delete $scratch: it is not an installer scratch folder in ${TMPDIR:-/tmp}."; return 1 ;;
  esac
}

# Why install_prebuilt_release returned non-zero, beyond the authentication
# failures tracked by RELEASE_CANDIDATE_FAILURE. With several repo candidates
# the most informative cause wins: anything learned from real release metadata
# outranks "could not read the metadata at all".
_prebuilt_failure_rank() {
  case "$1" in
    git-missing|curl-missing) echo 1 ;;
    metadata-unreachable)     echo 2 ;;
    no-release)               echo 3 ;;
    no-asset)                 echo 4 ;;
    asset-download)           echo 5 ;;
    asset-checksum)           echo 6 ;;
    install-failed)           echo 7 ;;
    mirror-companion)         echo 8 ;;
    *)                        echo 0 ;;
  esac
}

_note_prebuilt_failure() {
  if [ "$(_prebuilt_failure_rank "$1")" -gt \
       "$(_prebuilt_failure_rank "${RELEASE_PREBUILT_FAILURE:-}")" ]; then
    RELEASE_PREBUILT_FAILURE="$1"
  fi
  return 0
}

# Split a clone URL (https://host/owner/repo[.git]) into RELEASE_REPO_OWNER and
# RELEASE_REPO_NAME — the namespace for the relay's release-download endpoint.
_repo_owner_name() {
  local u="${1%.git}"
  RELEASE_REPO_NAME="${u##*/}"; u="${u%/*}"
  RELEASE_REPO_OWNER="${u##*/}"
}

# Echo the canonical "owner/repo" the release manifest (release.json) records. The
# release blob lives in an out-of-git, content-addressed store on the node that
# STAGED the release — never in git — so only that repo's host can serve it. A
# mirror node mirrors the git tree (it has SHASUMS256.txt/release.json) but NOT
# the CAS, so requesting the blob from the mirror that served the clone 404s,
# which is exactly why a published binary still fell back to a source build. Echo
# empty when the manifest is missing or records no well-formed owner/repo.
_manifest_repo() {
  [ -f "$1" ] || return 0
  sed -n 's/.*"repo"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -n 1
}

_manifest_build_commit() {
  [ -f "$1" ] || return 0
  sed -n 's/.*"build_commit"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" |
    head -n 1 | tr 'A-F' 'a-f'
}

_manifest_asset_commit() {
  [ -f "$1" ] && [ -n "$2" ] || return 0
  local name_re
  name_re="$(printf '%s' "$2" | sed 's/[.]/[.]/g')"
  tr -d '\n' < "$1" | grep -o "{\"name\":\"${name_re}\"[^}]*}" |
    sed -n 's/.*"commit"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F]*\)".*/\1/p' |
    head -n 1 | tr 'A-F' 'a-f'
}

_manifest_tag() {
  [ -f "$1" ] || return 0
  sed -n 's/.*"tag"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" |
    head -n 1
}

_manifest_channel() {
  [ -f "$1" ] || return 0
  sed -n 's/.*"channel"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" |
    head -n 1
}

_release_channel_accepts() {
  local wanted="$1" channel="$2" version="${3#v}"
  [ -n "$channel" ] || return 1
  [ "$channel" = "$wanted" ] && return 0
  [ "$wanted" = "latest" ] && [ -n "$version" ] &&
    [ "$channel" = "candidate-$version" ]
}

_release_version_at_least() {
  local have="${1#v}" want="${2#v}" index h w
  printf '%s' "$have" | grep -Eq '^[0-9]{1,6}(\.[0-9]{1,6}){0,3}$' || return 1
  printf '%s' "$want" | grep -Eq '^[0-9]{1,6}(\.[0-9]{1,6}){0,3}$' || return 1
  local -a have_parts want_parts
  IFS=. read -r -a have_parts <<<"$have"
  IFS=. read -r -a want_parts <<<"$want"
  for index in 0 1 2 3; do
    h=$((10#${have_parts[$index]:-0}))
    w=$((10#${want_parts[$index]:-0}))
    [ "$h" -gt "$w" ] && return 0
    [ "$h" -lt "$w" ] && return 1
  done
  return 0
}

_manifest_checksums_sha256() {
  [ -f "$1" ] || return 0
  sed -n 's/.*"checksums_sha256"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F]*\)".*/\1/p' "$1" |
    head -n 1 | tr 'A-F' 'a-f'
}

# Authenticate release.json independently, then bind it to the checksum list.
# Either an authenticated controller pins the exact manifest digest or a local
# Ed25519 publisher trust anchor verifies its detached signature.
_verify_release_metadata() {
  local manifest="$1" signature="$2" sums="$3"
  local manifest_hash sums_hash expected_sums tag build_commit asset_commit
  local signature_trust=0
  manifest_hash="$(_sha256_file "$manifest")"
  if ! printf '%s' "$manifest_hash" | grep -Eq '^[0-9a-f]{64}$'; then
    warn "A working SHA-256 tool is required for every prebuilt install."
    return 1
  fi
  if [ -n "$FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256" ]; then
    if [ "$manifest_hash" != "$FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256" ]; then
      warn "Release manifest does not match the controller-pinned SHA-256."
      return 1
    fi
  elif [ -n "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" ]; then
    if ! command -v openssl >/dev/null 2>&1; then
      warn "OpenSSL is required to verify the trusted release signature."
      return 1
    fi
    if [ ! -f "$signature" ] || [ "$(wc -c < "$signature" | tr -d ' ')" != "64" ] ||
       ! openssl pkeyutl -verify -pubin -rawin \
           -inkey "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" \
           -in "$manifest" -sigfile "$signature" >/dev/null 2>&1; then
      warn "Release manifest signature is missing or invalid."
      return 1
    fi
    signature_trust=1
  else
    warn "No independent release trust anchor is configured; refusing prebuilt bytes."
    return 1
  fi

  grep -Eq '"schema"[[:space:]]*:[[:space:]]*"forkmesh-release-v2"' "$manifest" ||
    { warn "Authenticated release manifest uses an unsupported schema."; return 1; }
  if [ "$signature_trust" = "1" ]; then
    tag="$(_manifest_tag "$manifest")"
    if ! _release_channel_accepts "${RELEASE_CHANNEL:-latest}" \
         "$(_manifest_channel "$manifest")" "$tag"; then
      warn "Authenticated release manifest is not for the ${RELEASE_CHANNEL:-latest} release channel."
      return 1
    fi
    if [ -z "$FORKMESH_EXPECTED_BUILD_COMMIT" ] &&
       ! _release_version_at_least "$tag" "${FORKMESH_MIN_RELEASE_VERSION:-}"; then
      warn "Authenticated release manifest ${tag:-without a tag} is older than this installer's minimum release ${FORKMESH_MIN_RELEASE_VERSION:-}."
      return 1
    fi
  fi
  expected_sums="$(_manifest_checksums_sha256 "$manifest")"
  sums_hash="$(_sha256_file "$sums")"
  if ! printf '%s' "$expected_sums" | grep -Eq '^[0-9a-f]{64}$' ||
     [ "$sums_hash" != "$expected_sums" ]; then
    warn "SHASUMS256.txt is not bound to the authenticated release manifest."
    return 1
  fi
  if [ -n "$FORKMESH_EXPECTED_BUILD_COMMIT" ]; then
    build_commit="$(_manifest_build_commit "$manifest")"
    asset_commit="$(_manifest_asset_commit "$manifest" "${ASSET_NAME:-}")"
    [ -z "$asset_commit" ] || build_commit="$asset_commit"
    if [ "$build_commit" != "$FORKMESH_EXPECTED_BUILD_COMMIT" ]; then
      warn "Published release does not match required build commit $FORKMESH_EXPECTED_BUILD_COMMIT."
      return 1
    fi
    tag="$(_manifest_tag "$manifest")"
    if [ "$tag" != "v$FORKMESH_EXPECTED_RELEASE_VERSION" ] &&
       [ "$tag" != "$FORKMESH_EXPECTED_RELEASE_VERSION" ]; then
      warn "Published release does not match required version $FORKMESH_EXPECTED_RELEASE_VERSION."
      return 1
    fi
  fi
  return 0
}

# Stage into the destination filesystem, validate those exact staged bytes, then
# atomically rename over $BIN. The old inode/process remains intact on every
# checksum or provenance failure.
_install_binary() {
  local candidate="$1" expected_hash="${2:-}" staged source_hash staged_hash
  INSTALL_BINARY_FAILURE_KIND=""
  mkdir -p "$BIN_DIR" || return 1
  if ! printf '%s' "$expected_hash" | grep -Eq '^[0-9a-f]{64}$'; then
    warn "An authenticated SHA-256 is required before staging a prebuilt binary."
    INSTALL_BINARY_FAILURE_KIND="checksum"
    return 1
  fi
  source_hash="$(_sha256_file "$candidate")"
  if [ "$source_hash" != "$expected_hash" ]; then
    warn "ForkMesh binary failed its source SHA-256 check; leaving the existing node untouched."
    INSTALL_BINARY_FAILURE_KIND="checksum"
    return 1
  fi
  staged="$(mktemp "$BIN_DIR/.forkmesh-install.XXXXXX" 2>/dev/null)" ||
    return 1
  if ! install -m 0755 "$candidate" "$staged" 2>/dev/null; then
    if ! cp "$candidate" "$staged" || ! chmod 0755 "$staged"; then
      rm -f "$staged"
      return 1
    fi
  fi
  staged_hash="$(_sha256_file "$staged")"
  if [ "$staged_hash" != "$expected_hash" ]; then
    warn "Staged ForkMesh binary failed its SHA-256 check; leaving the existing node untouched."
    INSTALL_BINARY_FAILURE_KIND="checksum"
    rm -f "$staged"
    return 1
  fi
  if ! chmod 0755 "$staged"; then
    rm -f "$staged"
    return 1
  fi
  if [ "$DEFER_PINNED_REINSTALL" = "1" ]; then
    CURRENT_STEP="reinstall"
    uninstall_forkmesh reinstall
    DEFER_PINNED_REINSTALL=0
    mkdir -p "$BIN_DIR" || { rm -f "$staged"; return 1; }
  fi
  if ! mv -f "$staged" "$BIN"; then
    rm -f "$staged"
    return 1
  fi
  return 0
}

_install_mirror_node_binary() {
  local candidate="$1" expected_hash="$2" destination="$BIN_DIR/forkmesh-mirror-node"
  local got staged
  got="$(_sha256_file "$candidate")"
  [ "$got" = "$expected_hash" ] || {
    warn "Go mirror-node companion failed its authenticated SHA-256 check."
    return 1
  }
  staged="$(mktemp "$BIN_DIR/.forkmesh-mirror-node.XXXXXX" 2>/dev/null)" || return 1
  if ! install -m 0755 "$candidate" "$staged" 2>/dev/null ||
     [ "$(_sha256_file "$staged")" != "$expected_hash" ] ||
     ! mv -f "$staged" "$destination"; then
    rm -f "$staged"
    return 1
  fi
  say "Installed native Go mirror-node companion to $destination"
}

# Install the prebuilt binary for this platform. New model (issue #304): release
# binaries are NOT committed to git. The installer reads the tiny committed
# release manifest (SHASUMS256.txt, fetched over the git proxy) to learn the
# platform asset's content hash, downloads the bytes from the relay's
# content-addressed release endpoint, and verifies the authenticated manifest,
# checksum-list binding, and binary SHA-256 before installing. Unsigned legacy
# releases are never executed. The caller may explicitly allow a source build
# after this returns non-zero.
install_prebuilt_release() {
  RELEASE_FRESHNESS_MATCH=0
  RELEASE_CANDIDATE_FAILURE=0
  RELEASE_PREBUILT_FAILURE=""
  RELEASE_METADATA_REASON=""
  RELEASE_METADATA_ATTEMPTS=0
  if ! command -v git >/dev/null 2>&1; then
    RELEASE_PREBUILT_FAILURE="git-missing"
    return 1
  fi
  if ! command -v curl >/dev/null 2>&1; then
    RELEASE_PREBUILT_FAILURE="curl-missing"
    return 1
  fi
  ensure_mirror_candidates
  local tmp repo sums manifest signature canon hash url bin got attempt attempt_url
  local mirror_name mirror_hash mirror_url mirror_bin delay dl_total
  local builtin_release_trust=0
  local -a dl_delays=()
  # Blob download retries on top of curl's own --retry: seconds to wait before
  # each re-attempt (default "5": two attempts).
  read -r -a dl_delays <<<"${FORKMESH_DOWNLOAD_RETRY_DELAYS-5}" || true
  dl_total=$(( ${#dl_delays[@]} + 1 ))
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/forkmesh-prebuilt.XXXXXX" 2>/dev/null)" || return 1
  # Standalone `curl | bash` installs need an independent trust anchor too.
  # Materialize ForkMesh's release-only public key after each sparse checkout
  # unless an administrator or authenticated controller supplied a different
  # trust mechanism. _sparse_fetch_file intentionally replaces its staging
  # directory, so writing the key before it runs would delete the trust anchor
  # and make every otherwise-valid standalone install fail authentication.
  # The corresponding private key is never shipped.
  if [ -z "$FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256" ] &&
     [ -z "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" ]; then
    builtin_release_trust=1
  fi
  sums=".forkmesh/releases/${RELEASE_CHANNEL}/SHASUMS256.txt"
  manifest=".forkmesh/releases/${RELEASE_CHANNEL}/release.json"
  signature=".forkmesh/releases/${RELEASE_CHANNEL}/release.json.sig"
  for repo in "${REPO_CANDIDATES[@]}"; do
    # The sparse-checkout clone below is silent (redirected to /dev/null so a
    # missing manifest isn't logged as an error) and can take a while over a
    # slow mirror, so announce the attempt here — otherwise the install appears
    # to hang with no output between "Resolving..." and "Downloading..." (#418).
    say "Checking $repo for a prebuilt release…"
    # New model: manifest checksum + content-addressed download (+ verify). Fetch
    # release.json in the same checkout so the blob can be requested from the repo
    # that staged it — not the mirror that happened to serve this clone.
    if _sparse_fetch_file "$repo" "$tmp" "$sums" "$manifest" "$signature"; then
      if [ "$builtin_release_trust" = "1" ]; then
        FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE="$tmp/release-publisher.pem"
        printf '%s\n' \
          '-----BEGIN PUBLIC KEY-----' \
          'MCowBQYDK2VwAyEAHBYOCW4qnyZkYAnEoqUrYxPiDRszjfJa+xJUeyQPUS0=' \
          '-----END PUBLIC KEY-----' \
          >"$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" || return 1
        chmod 0600 "$FORKMESH_TRUSTED_RELEASE_PUBLIC_KEY_FILE" 2>/dev/null || true
      fi
      if [ ! -s "$tmp/$manifest" ] ||
         ! _verify_release_metadata "$tmp/$manifest" "$tmp/$signature" "$tmp/$sums"; then
        RELEASE_CANDIDATE_FAILURE=1
        continue
      fi
      RELEASE_FRESHNESS_MATCH=1
      hash="$(awk -v n="$ASSET_NAME" '$2==n {print $1; exit}' "$tmp/$sums" 2>/dev/null)"
      if printf '%s' "$hash" | grep -Eq '^[0-9a-f]{64}$'; then
        # Prefer the canonical owner/repo the manifest records — only that node
        # holds the out-of-git release blob. Fall back to the mirror's own
        # owner/repo for legacy manifests that don't record it.
        canon="$(_manifest_repo "$tmp/$manifest")"
        if printf '%s' "$canon" | grep -Eq '^[^/]+/[^/]+$'; then
          RELEASE_REPO_OWNER="${canon%%/*}"
          RELEASE_REPO_NAME="${canon##*/}"
        else
          _repo_owner_name "$repo"
        fi
        url="${FORKMESH_HOST%/}/api/repo/${RELEASE_REPO_OWNER}/${RELEASE_REPO_NAME}/releases/blob/sha256/${hash}"
        bin="$tmp/asset.bin"
        say "Downloading prebuilt ${ASSET_OS}/${ASSET_ARCH} binary ($ASSET_NAME)…"
        for ((attempt = 1; attempt <= dl_total; attempt++)); do
          attempt_url="$url"
          if [ "$attempt" -gt 1 ]; then
            attempt_url="${url}?retry=$(date +%s)"
          fi
          # Bounded connect, abort a stalled transfer, and let curl ride out
          # transient 5xx/timeouts itself before this loop's own re-attempt.
          if ! curl -fsSL --connect-timeout 20 --speed-limit 1 --speed-time 60 \
                 --retry 2 --retry-delay 3 -H 'Cache-Control: no-cache' \
                 "$attempt_url" -o "$bin" 2>/dev/null || [ ! -s "$bin" ]; then
            _note_prebuilt_failure asset-download
            if [ "$attempt" -lt "$dl_total" ]; then
              delay="${dl_delays[$((attempt - 1))]}"
              case "$delay" in ''|*[!0-9]*) delay=0 ;; esac
              warn "Downloading $ASSET_NAME failed; retrying in ${delay}s (attempt $((attempt + 1)) of $dl_total)…"
              sleep "$delay"
            fi
            continue
          fi
          got="$(_sha256_file "$bin")"
          if ! printf '%s' "$got" | grep -Eq '^[0-9a-f]{64}$'; then
            warn "A working SHA-256 tool is required for every prebuilt install."
            RELEASE_CANDIDATE_FAILURE=1
            rm -f "$bin"
            continue
          elif [ -n "$got" ] && [ "$got" != "$hash" ]; then
            warn "Checksum mismatch for $ASSET_NAME (expected $hash, got $got); skipping."
            _note_prebuilt_failure asset-checksum
            rm -f "$bin"
            continue
          elif _install_binary "$bin" "$hash"; then
            mirror_name="forkmesh-mirror-node-${ASSET_OS}-${ASSET_ARCH}"
            [ "$ASSET_OS" = "windows" ] && mirror_name="${mirror_name}.exe"
            mirror_hash="$(awk -v n="$mirror_name" '$2==n {print $1; exit}' "$tmp/$sums" 2>/dev/null)"
            if ! printf '%s' "$mirror_hash" | grep -Eq '^[0-9a-f]{64}$'; then
              # Releases published before the native service existed remain
              # installable. Every newly published release includes this asset;
              # the warning tells an operator to update before deploying Vultr.
              warn "This older release has no Go mirror-node companion for ${ASSET_OS}/${ASSET_ARCH}."
            else
              mirror_url="${FORKMESH_HOST%/}/api/repo/${RELEASE_REPO_OWNER}/${RELEASE_REPO_NAME}/releases/blob/sha256/${mirror_hash}"
              mirror_bin="$tmp/mirror-node.bin"
              if ! curl -fsSL --connect-timeout 20 --speed-limit 1 --speed-time 60 \
                     --retry 3 --retry-delay 3 -H 'Cache-Control: no-cache' \
                     "$mirror_url" -o "$mirror_bin" 2>/dev/null ||
                 [ ! -s "$mirror_bin" ] ||
                 ! _install_mirror_node_binary "$mirror_bin" "$mirror_hash"; then
                warn "Could not install the native Go mirror-node companion."
                _note_prebuilt_failure mirror-companion
                _rm_install_scratch "$tmp"
                return 1
              fi
            fi
            REPO="$repo"; _rm_install_scratch "$tmp"
            say "Installed prebuilt ForkMesh ${ASSET_OS}/${ASSET_ARCH} binary to $BIN"
            return 0
          elif [ -n "${INSTALL_BINARY_FAILURE_KIND:-}" ]; then
            RELEASE_CANDIDATE_FAILURE=1
          else
            _note_prebuilt_failure install-failed
          fi
        done
      else
        # Authenticated metadata that genuinely lists no asset for this
        # platform: the only case that may say "no prebuilt is published".
        _note_prebuilt_failure no-asset
      fi
    elif [ "${SPARSE_FETCH_STATUS:-}" = "missing" ]; then
      _note_prebuilt_failure no-release
    else
      _note_prebuilt_failure metadata-unreachable
      RELEASE_METADATA_REASON="${SPARSE_FETCH_REASON:-}"
      RELEASE_METADATA_ATTEMPTS="${SPARSE_FETCH_ATTEMPTS:-0}"
    fi
    # Manifest-free/unsigned legacy assets are intentionally never executed.
  done
  _rm_install_scratch "$tmp"
  return 1
}

# Direct-upload fast path (adhoc #67): install a binary the deploying desktop
# app already streamed onto this machine over the SSH session, skipping the
# relay download entirely (useful when this host can't reach the release
# endpoint, or to push exactly the build the operator is running). The
# uploader's declared platform must match this machine; on any mismatch — or a
# missing/empty upload — return non-zero so the normal download path runs.
install_local_binary() {
  [ -n "$FORKMESH_LOCAL_BINARY" ] || return 1
  if [ ! -s "$FORKMESH_LOCAL_BINARY" ]; then
    warn "Uploaded binary $FORKMESH_LOCAL_BINARY is missing or empty; falling back to a relay download."
    return 1
  fi
  if [ -n "$FORKMESH_LOCAL_OS" ] && [ "$FORKMESH_LOCAL_OS" != "$ASSET_OS" ]; then
    warn "Uploaded binary targets $FORKMESH_LOCAL_OS but this machine is $ASSET_OS; falling back to a relay download."
    return 1
  fi
  if [ -n "$FORKMESH_LOCAL_ARCH" ] && [ "$FORKMESH_LOCAL_ARCH" != "$ASSET_ARCH" ]; then
    warn "Uploaded binary targets $FORKMESH_LOCAL_ARCH but this machine is $ASSET_ARCH; falling back to a relay download."
    return 1
  fi
  if [ -z "$FORKMESH_LOCAL_BINARY_SHA256" ]; then
    warn "Uploaded binary has no controller-pinned SHA-256; refusing it and falling back to a signed release."
    return 1
  fi
  _install_binary "$FORKMESH_LOCAL_BINARY" "$FORKMESH_LOCAL_BINARY_SHA256" || return 1
  say "Installed the directly-uploaded ForkMesh binary to $BIN"
  return 0
}

# True when install_prebuilt_release never got as far as reading authenticated
# release metadata (git/curl missing, or every mirror attempt failed). Nothing
# is then known about what IS published, so the commit-mismatch and
# "no prebuilt is published" errors would both be false.
_prebuilt_metadata_unavailable() {
  [ "${RELEASE_CANDIDATE_FAILURE:-0}" != "1" ] || return 1
  case "${RELEASE_PREBUILT_FAILURE:-}" in
    git-missing|curl-missing|metadata-unreachable) return 0 ;;
  esac
  return 1
}

# Set PREBUILT_FAIL_DETAIL (the diag token), PREBUILT_FAIL_MESSAGE (the fatal
# error) and PREBUILT_FAIL_SUMMARY (a short clause for the source-build
# fallback) from RELEASE_PREBUILT_FAILURE.
_explain_prebuilt_failure() {
  local platform="${ASSET_OS}/${ASSET_ARCH}" repos="${REPO_CANDIDATES[*]:-}"
  local advice="Download a native installer at ${FORKMESH_HOST%/}/desktop, or use FORKMESH_FROM_SOURCE=1 only on a development machine with a full build toolchain."
  PREBUILT_FAIL_DETAIL="${RELEASE_PREBUILT_FAILURE:-no-asset}"
  case "$PREBUILT_FAIL_DETAIL" in
    git-missing)
      PREBUILT_FAIL_SUMMARY="git is not installed, so the release metadata could not be read"
      PREBUILT_FAIL_MESSAGE="git is required to install the prebuilt ForkMesh binary (the installer reads the signed release metadata over git). Install git with your package manager and re-run the installer."
      ;;
    curl-missing)
      PREBUILT_FAIL_SUMMARY="curl is not installed, so the prebuilt binary could not be downloaded"
      PREBUILT_FAIL_MESSAGE="curl is required to download the prebuilt ForkMesh binary. Install curl with your package manager and re-run the installer."
      ;;
    metadata-unreachable)
      PREBUILT_FAIL_SUMMARY="could not reach a ForkMesh mirror to read the release metadata"
      PREBUILT_FAIL_MESSAGE="Could not reach a ForkMesh mirror to read the release metadata (${RELEASE_METADATA_REASON:-git fetch failed}; ${RELEASE_METADATA_ATTEMPTS:-0} attempts against ${repos}). The mirrors may be restarting; please retry in a minute."
      ;;
    no-release)
      PREBUILT_FAIL_SUMMARY="no release metadata is published on the '${RELEASE_CHANNEL}' channel"
      PREBUILT_FAIL_MESSAGE="No prebuilt ForkMesh binary is published for ${platform}: the '${RELEASE_CHANNEL}' release channel at ${repos} has no SHASUMS256.txt. ${advice}"
      ;;
    asset-download)
      PREBUILT_FAIL_SUMMARY="the prebuilt ${platform} binary could not be downloaded"
      PREBUILT_FAIL_MESSAGE="The ForkMesh release metadata was read, but downloading the prebuilt ${platform} binary ($ASSET_NAME) from ${FORKMESH_HOST%/} failed. Please retry in a minute."
      ;;
    asset-checksum)
      PREBUILT_FAIL_SUMMARY="the downloaded ${platform} binary failed its SHA-256 check"
      PREBUILT_FAIL_MESSAGE="The downloaded ForkMesh ${platform} binary did not match its authenticated SHA-256 (a stale or corrupted download), so nothing was installed. Please retry in a minute."
      ;;
    install-failed)
      PREBUILT_FAIL_SUMMARY="the verified binary could not be written to $BIN"
      PREBUILT_FAIL_MESSAGE="The prebuilt ForkMesh binary was downloaded and verified, but could not be written to $BIN. Check that $BIN_DIR is writable and has free space, then re-run the installer."
      ;;
    mirror-companion)
      PREBUILT_FAIL_SUMMARY="the Go mirror-node companion could not be downloaded or verified"
      PREBUILT_FAIL_MESSAGE="The prebuilt ForkMesh binary was installed, but its native Go mirror-node companion for ${platform} could not be downloaded or verified. Please retry in a minute."
      ;;
    *)
      PREBUILT_FAIL_DETAIL="no-asset"
      PREBUILT_FAIL_SUMMARY="no prebuilt binary published for ${platform}"
      PREBUILT_FAIL_MESSAGE="No prebuilt ForkMesh binary is published for ${platform}. ${advice}"
      ;;
  esac
}

# Die with the accurate reason, or (FORKMESH_NO_SOURCE_FALLBACK=0) explain it
# and continue into the source build.
_prebuilt_fail_or_build_from_source() {
  _explain_prebuilt_failure
  if [ "$FORKMESH_NO_SOURCE_FALLBACK" = "1" ]; then
    die_diag prebuilt "$PREBUILT_FAIL_DETAIL" "$PREBUILT_FAIL_MESSAGE"
  fi
  say "Prebuilt install unavailable (${PREBUILT_FAIL_SUMMARY}); building from source."
  diag prebuilt 0 "$PREBUILT_FAIL_DETAIL"
}

detect_release_asset
if [ "${FORKMESH_FROM_SOURCE:-0}" != "1" ]; then
  CURRENT_STEP="prebuilt"
  if install_local_binary; then
    INSTALLED_PREBUILT=1
    ensure_qt_runtime
    ensure_emoji_font
    diag prebuilt 1 "uploaded"
  elif install_prebuilt_release; then
    INSTALLED_PREBUILT=1
    ensure_qt_runtime
    ensure_emoji_font
    diag prebuilt 1 "$ASSET_NAME"
  elif _prebuilt_metadata_unavailable; then
    _prebuilt_fail_or_build_from_source
  elif [ -n "$FORKMESH_EXPECTED_BUILD_COMMIT" ] &&
       [ "${RELEASE_FRESHNESS_MATCH:-0}" != "1" ]; then
    die_diag prebuilt "release-commit-mismatch" "No published ForkMesh artifact matches required build commit $FORKMESH_EXPECTED_BUILD_COMMIT. Publish that commit's release before retrying the fleet binary install."
  elif [ -n "$FORKMESH_EXPECTED_BUILD_COMMIT" ] &&
       [ "${RELEASE_CANDIDATE_FAILURE:-0}" = "1" ]; then
    die_diag prebuilt "release-candidate-provenance" "The published ForkMesh release failed authenticated-manifest or SHA-256 verification. The existing binary and daemon were left untouched."
  elif [ "${RELEASE_CANDIDATE_FAILURE:-0}" = "1" ] &&
       [ "$FORKMESH_NO_SOURCE_FALLBACK" = "1" ]; then
    die_diag prebuilt "release-authentication" "No prebuilt ForkMesh release passed independent manifest authentication and SHA-256 verification. Configure a trusted publisher key or controller-pinned manifest digest."
  else
    _prebuilt_fail_or_build_from_source
  fi
fi

# Everything from here to the build retry is the source-build pipeline; skip it
# entirely once a prebuilt binary is in place.
if [ "$INSTALLED_PREBUILT" != "1" ]; then
ensure cmake  command -v cmake

# Compiler: on macOS this means the Xcode Command Line Tools, which brew can't
# install — trigger Apple's installer instead.
if ! have_compiler; then
  if [ "$(uname -s)" = "Darwin" ]; then
    if [ "${FORKMESH_NO_INSTALL_DEPS:-0}" = "1" ]; then
      die "A C++ compiler is required. Install the Xcode Command Line Tools: xcode-select --install"
    fi
    say "Installing Xcode Command Line Tools (a system dialog may appear)"
    DIAG_MISSING="${DIAG_MISSING:+$DIAG_MISSING }compiler"
    xcode-select --install 2>/dev/null || true
    until have_compiler; do
      say "Waiting for the Command Line Tools install to finish..."
      sleep 10
    done
  else
    ensure compiler have_compiler
  fi
fi

# Qt 6 (Widgets, Network, Svg) and OpenSSL are required by CMake. Probe them
# before touching the package manager: repeatedly running `curl | bash` must
# not demand sudo or reinstall large development packages on an already-ready
# host. `ensure` still installs the known packages and fails clearly when a
# component is genuinely absent.
ensure qt have_qt6_dev
ensure openssl have_openssl_dev
ensure_webengine
ensure_emoji_font
# detail records which prerequisites had to be installed ("none" = all present),
# so the funnel shows how often a machine already met the requirements.
diag deps 1 "${DIAG_MISSING:-none}"

# --- fetch + build + install (one retry; the checkout is never deleted) ------
# Each phase returns non-zero instead of aborting so the pipeline can run once
# more. A failed pull stops and changes nothing; an incomplete checkout is moved
# aside, never deleted; a clone only goes into a missing or empty folder.
# docs/engineering/destructive-actions.md.

# The owner/path-bound marker declared above is the sole authority for updating
# or moving aside an installer checkout.
owns_src() { _marker_is_valid "$SRC"; }

# Extract the node segment ("https://host/<node>/forkmesh" -> "<node>") so log
# lines can name the offending mirror without echoing the whole clone URL.
repo_node() {
  local r="${1%/}"   # drop any trailing slash
  r="${r%/*}"        # drop the trailing /<repo> segment
  printf '%s' "${r##*/}"
}

# Turn the captured `git clone` output into a short, human-readable reason. The
# 504 "Host timed out" the relay returns when a named mirror's git tunnel is
# unresponsive is the case this whole fallback exists for, so name it precisely;
# everything else gets a best-effort classification for the diagnostics funnel.
classify_clone_failure() {
  case "$1" in
    *"failed integrity check"*|*"repository failed integrity"*) echo "integrity pin rejected by the relay" ;;
    *"HTTP2 framing layer"*|*"HTTP/2 stream"*)                  echo "HTTPS connection broke partway (HTTP/2); retry, or set git config --global http.version HTTP/1.1" ;;
    *"transfer closed with outstanding read data"*|*"early EOF"*|*"unexpected disconnect"*) echo "connection dropped during the download" ;;
    *"Host timed out"*|*"error: 504"*|*" 504"*)                 echo "mirror endpoint timed out (HTTP 504)" ;;
    *"error: 502"*|*" 502"*)                                    echo "relay gateway error (HTTP 502)" ;;
    *"error: 503"*|*" 503"*)                                    echo "mirror temporarily unavailable (HTTP 503)" ;;
    *"error: 404"*|*"not found"*|*"Repository not found"*)      echo "repository not found on this mirror (HTTP 404)" ;;
    *"error: 401"*|*"Authentication failed"*)                   echo "authentication required (HTTP 401)" ;;
    *"Could not resolve host"*|*"Couldn't resolve"*)            echo "DNS resolution failed" ;;
    *"Connection refused"*|*"Failed to connect"*)               echo "connection refused" ;;
    *"timed out"*|*"timeout"*|*"Operation timed out"*)          echo "network timeout" ;;
    *)                                                          echo "git clone failed" ;;
  esac
}

_dir_is_empty() {
  [ -d "$1" ] && [ -z "$(ls -A -- "$1" 2>/dev/null)" ]
}

clean_clone() {
  local scratch_root scratch repo out rc reason node total="${#REPO_CANDIDATES[@]}" idx=0
  CLONE_FAIL_REASON=""
  if [ -e "$SRC" ] && ! _dir_is_empty "$SRC"; then
    CLONE_FAIL_REASON="$SRC is not empty"
    warn "Refusing to clone into $SRC: it is not empty. Nothing was changed."
    return 1
  fi
  scratch_root="$(dirname "$SRC")/tmp"
  mkdir -p "$scratch_root" || return 1
  for repo in "${REPO_CANDIDATES[@]}"; do
    idx=$((idx + 1))
    node="$(repo_node "$repo")"
    scratch="$(mktemp -d "$scratch_root/src-clone.XXXXXX")" || return 1
    if [ "$total" -gt 1 ]; then
      say "Cloning $repo  (mirror $idx of $total)"
    else
      say "Cloning $repo"
    fi
    out="$(git clone --depth 1 "$repo" "$scratch/src" 2>&1)"; rc=$?
    printf '%s\n' "$out"
    if [ "$rc" -eq 0 ]; then
      if [ -e "$SRC" ] && ! _dir_is_empty "$SRC"; then
        fm_safe_rm_tree "$scratch_root" "$scratch" || true
        CLONE_FAIL_REASON="$SRC is not empty"
        warn "Refusing to replace $SRC: it filled up during the clone. Nothing was changed."
        return 1
      fi
      rmdir -- "$SRC" 2>/dev/null || true
      mv -- "$scratch/src" "$SRC" || return 1
      rmdir -- "$scratch" "$scratch_root" 2>/dev/null || true
      _write_managed_marker "$SRC" ||
        die "Could not write the managed-install marker in $SRC."
      REPO="$repo"
      return 0
    fi
    fm_safe_rm_tree "$scratch_root" "$scratch" || true
    reason="$(classify_clone_failure "$out")"
    CLONE_FAIL_REASON="$reason"
    case "$out" in
      *"failed integrity check"*|*"repository failed integrity"*) PIN_FAILURE=1; return 1 ;;
    esac
    warn "Mirror '$node' could not be cloned: $reason."
    [ "$idx" -lt "$total" ] && say "  Falling back to the next online mirror…"
  done
  rmdir -- "$scratch_root" 2>/dev/null || true
  return 1
}

pull_source() {
  local repo candidates=("${REPO_CANDIDATES[@]}")
  [ "${#candidates[@]}" -gt 0 ] || candidates=("$REPO")
  CLONE_FAIL_REASON=""
  for repo in "${candidates[@]}"; do
    git -C "$SRC" remote set-url origin "$repo" 2>/dev/null || true
    if git -C "$SRC" pull --ff-only; then
      REPO="$repo"
      return 0
    fi
    CLONE_FAIL_REASON="git pull --ff-only failed"
    warn "Mirror '$(repo_node "$repo")' could not update $SRC."
  done
  return 1
}

fetch_source() {
  mkdir -p "$(dirname "$SRC")" || return 1
  if [ -d "$SRC/.git" ]; then
    if ! owns_src; then
      CLONE_FAIL_REASON="existing checkout not created by this installer"
      warn "$SRC is a git checkout this installer did not create. It was left untouched. Set FORKMESH_DIR to another folder to install from a fresh clone there."
      return 1
    fi
    say "Updating existing checkout in $SRC"
    if ! pull_source; then
      warn "Could not update $SRC from any mirror. The checkout was left untouched: nothing was deleted or re-cloned. Try again shortly, or update it yourself with git pull."
      return 1
    fi
  elif [ -e "$SRC" ] && ! _dir_is_empty "$SRC"; then
    if ! owns_src; then
      CLONE_FAIL_REASON="$SRC is not empty"
      warn "$SRC is not empty and is not a checkout this installer created. It was left untouched. Move it away or set FORKMESH_DIR to another folder."
      return 1
    fi
    _move_src_aside "$SRC"
    clean_clone || return 1
  else
    clean_clone || return 1
  fi
  if [ ! -f "$SRC/desktop/CMakeLists.txt" ]; then
    warn "Checkout in $SRC is incomplete (no desktop/CMakeLists.txt); moving it aside and cloning again."
    _move_src_aside "$SRC"
    clean_clone || return 1
  fi
  [ -f "$SRC/desktop/CMakeLists.txt" ] || return 1
}

build_client() {
  local jobs build_dir total_kb=0 available_kb=0 reserve_kb usable_kb memory_jobs
  jobs="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )"
  [ "$jobs" -gt 4 ] && jobs=4
  if [ -r /proc/meminfo ]; then
    total_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
    available_kb="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)"
  fi
  if [ "${total_kb:-0}" -gt 0 ]; then
    memory_jobs=$((total_kb / (4 * 1024 * 1024)))
    [ "$memory_jobs" -lt 1 ] && memory_jobs=1
    [ "$jobs" -gt "$memory_jobs" ] && jobs="$memory_jobs"
  fi
  if [ "${available_kb:-0}" -gt 0 ]; then
    reserve_kb=$((6 * 1024 * 1024))
    if [ "${total_kb:-0}" -gt 0 ] && [ "$reserve_kb" -gt $((total_kb / 2)) ]; then
      reserve_kb=$((total_kb / 2))
    fi
    usable_kb=$((available_kb - reserve_kb))
    [ "$usable_kb" -lt 0 ] && usable_kb=0
    memory_jobs=$((usable_kb / (2 * 1024 * 1024)))
    [ "$memory_jobs" -lt 1 ] && memory_jobs=1
    [ "$jobs" -gt "$memory_jobs" ] && jobs="$memory_jobs"
  fi
  build_dir="$SRC/desktop/build"
  local cmake_args=(-S "$SRC/desktop" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DFORKMESH_BUILD_TESTS=OFF)
  if command -v brew >/dev/null 2>&1; then
    local qt_prefix ssl_prefix
    qt_prefix="$(brew --prefix qt 2>/dev/null || true)"
    ssl_prefix="$(brew --prefix openssl@3 2>/dev/null || true)"
    [ -n "$qt_prefix" ] && cmake_args+=("-DCMAKE_PREFIX_PATH=$qt_prefix")
    [ -n "$ssl_prefix" ] && cmake_args+=("-DOPENSSL_ROOT_DIR=$ssl_prefix")
  fi
  say "Configuring"
  cmake "${cmake_args[@]}" || return 1
  say "Building with $jobs memory-safe job(s) (this can take a few minutes)"
  cmake --build "$build_dir" -j"$jobs" || return 1
  BUILD="$build_dir"  # used by the install/launch phases below
}

install_client() {
  local built
  if [ -x "$BUILD/ForkMesh.app/Contents/MacOS/ForkMesh" ]; then
    built="$BUILD/ForkMesh.app/Contents/MacOS/ForkMesh"
  else
    built="$BUILD/forkmesh"
  fi
  [ -x "$built" ] || { warn "Build did not produce an executable at $built."; return 1; }
  mkdir -p "$BIN_DIR" || return 1
  install -m 0755 "$built" "$BIN" 2>/dev/null \
    || { cp "$built" "$BIN" && chmod 0755 "$BIN"; } || return 1
  say "Installed to $BIN"
}

# Fetch -> build -> install, tagging the active phase for diagnostics. Returns
# non-zero (rather than exiting) on the first failure so the caller can retry.
attempt_install() {
  # On a fetch failure, report WHY (the classified clone reason) to the funnel so
  # operators can see, e.g., that every mirror returned a 504 — not just that the
  # fetch step dropped off.
  CURRENT_STEP="fetch";   fetch_source   || { diag fetch 0 "${CLONE_FAIL_REASON:-fetch_failed}"; return 1; }; diag fetch 1
  CURRENT_STEP="build";   build_client   || return 1; diag build 1
  CURRENT_STEP="install"; install_client || return 1; diag install 1
}

# A stale integrity pin is rejected identically on a clean re-clone (the relay,
# not the local checkout, refuses it), so don't bother retrying that case — go
# straight to a clear, owner-actionable explanation.
pin_failure_help() {
  printf '\033[31mError:\033[0m The mirror serving %s failed its integrity check.\n' "$REPO" >&2
  printf '\n' >&2
  printf 'This is NOT a problem with your machine. The relay pins an owner-signed\n' >&2
  printf 'fingerprint of the repository'"'"'s refs and refuses to serve any mirror that\n' >&2
  printf 'does not match it. The currently published pin is stale or mismatched, so\n' >&2
  printf 'every clone is being rejected before any data is sent.\n' >&2
  printf '\n' >&2
  printf 'The repository owner needs to refresh it from the ForkMesh desktop app:\n' >&2
  printf '  open the repo, then click "Reset integrity pin" (it re-signs the refs\n' >&2
  printf '  the node currently serves). A fresh publish from the host node also fixes it.\n' >&2
  printf '\n' >&2
  printf 'Once the pin is reset, re-run:\n' >&2
  printf '  curl -fsSL %s/install.sh | bash\n' "$FORKMESH_HOST" >&2
  exit 1
}

if ! attempt_install; then
  [ "$PIN_FAILURE" = "1" ] && pin_failure_help
  warn "Install failed; retrying once. The source checkout is kept as it is."
  if ! attempt_install; then
    [ "$PIN_FAILURE" = "1" ] && pin_failure_help
    # Name the last failure reason and every mirror that was tried so the cause
    # is obvious from the final line alone (e.g. all mirrors returned a 504).
    if [ "${CLONE_FAIL_REASON:-}" ]; then
      warn "Mirrors tried: ${FORKMESH_NODES:-$REPO}"
      warn "Re-run with FORKMESH_DEBUG=1 for the full git/HTTP trace."
      die "Could not fetch the source from any online mirror ($CLONE_FAIL_REASON). All mirrors are unreachable right now — please try again shortly."
    fi
    die "Install failed again; see the messages above for the cause. Nothing in $SRC was deleted."
  fi
fi
fi  # end source-build pipeline (skipped when a prebuilt binary was installed)

case ":$PATH:" in
  *":$BIN_DIR:"*) ;;
  *) say "Add $BIN_DIR to your PATH, e.g.  export PATH=\"$BIN_DIR:\$PATH\"" ;;
esac

# --- desktop integration (Linux) --------------------------------------------
# Register a .desktop launcher + hicolor icons so ForkMesh appears in the
# GNOME/KDE app menu and dock — not just on the PATH. This reuses the dedicated
# desktop/install.sh that ships in the cloned source, so the launcher entry
# stays in one place and points at the build output (picking up in-app updates).
# Best-effort and Linux-only: macOS gets its menu entry from the .app bundle,
# and a missing icon-cache tool must never fail the whole install.
register_desktop_entry() {
  [ "$(uname -s)" = "Linux" ] || return 0
  CURRENT_STEP="desktop"
  # A headless box has no GNOME/KDE app menu or dock to register into, so writing
  # a .desktop launcher + icons there is pointless. Skip it (matching the
  # auto-launch display check below); the app still runs from $BIN / the CLI.
  if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    say "No display detected; skipping app-menu registration (run:  forkmesh)."
    return 0
  fi
  local script="$SRC/desktop/install.sh"
  if [ ! -f "$script" ]; then
    # The prebuilt fast path never clones the source, so the helper that writes
    # the .desktop entry isn't present — that's expected, not an error. The app
    # still runs from $BIN on the PATH.
    if [ "$INSTALLED_PREBUILT" = "1" ]; then
      say "ForkMesh adds itself to the application menu the first time it opens."
    else
      warn "Desktop integration script not found at $script; skipping menu registration."
    fi
    diag desktop 1 "skipped"
    return 0
  fi
  say "Registering ForkMesh in the application menu (this can take a moment)…"
  # The helper renders icon buckets and rebuilds the GTK icon cache, either of
  # which can hang on a misconfigured box (a wedged ImageMagick delegate, a slow
  # `gtk-update-icon-cache -f` over a huge hicolor theme). Run it under a hard
  # time limit so desktop integration can NEVER freeze the whole install, capture
  # its verbose output to a log, and replay that log on failure (or always with
  # FORKMESH_DEBUG=1). A non-zero exit here is non-fatal — the app still runs from
  # $BIN / the build output.
  local log rc=0
  log="$(mktemp 2>/dev/null || echo "${TMPDIR:-/tmp}/forkmesh-desktop.$$.log")"
  dbg "running desktop helper: bash $script (output log: $log)"
  if command -v timeout >/dev/null 2>&1; then
    timeout 180 bash "$script" >"$log" 2>&1 || rc=$?
  else
    bash "$script" >"$log" 2>&1 || rc=$?
  fi
  dbg "desktop helper exited with status $rc"
  [ "${FORKMESH_DEBUG:-0}" = "1" ] && [ -s "$log" ] && cat "$log" >&2
  if [ "$rc" -eq 0 ]; then
    say "Added to the application menu — search \"ForkMesh\" in Activities/the app grid."
    diag desktop 1
  elif [ "$rc" -eq 124 ]; then
    warn "Desktop menu registration timed out (>180s) and was skipped; ForkMesh still runs via:  forkmesh"
    [ -s "$log" ] && warn "Last helper output: $(tail -n 3 "$log" 2>/dev/null | tr '\n' ' ')"
    warn "Re-run with FORKMESH_DEBUG=1 to see exactly which desktop step hung."
    diag desktop 0 "timeout"
  else
    warn "Could not register the desktop menu entry (exit $rc); ForkMesh still runs via:  forkmesh"
    [ -s "$log" ] && warn "Helper output: $(tail -n 5 "$log" 2>/dev/null | tr '\n' ' ')"
    diag desktop 0 "rc$rc"
  fi
  rm -f "$log" 2>/dev/null || true
}
register_desktop_entry

# --- launch -----------------------------------------------------------------
# One-shot install: start ForkMesh automatically so the user lands in the app.
# Detached from this script (which may itself be running under `curl | bash`) so
# it keeps running after the installer exits. Set FORKMESH_NO_LAUNCH=1 to skip
# (e.g. headless build servers).
#
# On a desktop this opens the GUI. On a display-less root host it prepares the
# separate native Go mirror service; it never launches the Qt desktop as a
# server. LAUNCH_MODE records which path ran so the caller prints the right
# message.
LAUNCH_MODE=""

prepare_root_mirror_service() {
  local service_user="forkmesh-node" state_dir="$SYSTEMD_STATE_DIR"
  local native_candidate="$BIN_DIR/forkmesh-mirror-node"
  local binary_hash account home shell managed_install=0 bootstrap_install=0
  [ "$(id -u)" -eq 0 ] || return 1
  command -v systemctl >/dev/null 2>&1 &&
    [ -d /run/systemd/system ] ||
    die "A managed mirror install requires systemd."
  printf '%s' "$FORKMESH_NODE_NAME" | grep -Eq '^[A-Za-z0-9._-]*$' ||
    die "Mirror node names may contain only letters, numbers, dot, underscore, and hyphen."
  printf '%s' "$FORKMESH_LINK_CODE" | grep -Eq '^[0-9]{6}$' ||
    die "Mirror node link code is invalid."
  if [ ! -x "$native_candidate" ] && [ ! -x "$SYSTEMD_BIN" ] &&
     [ -f "$SRC/server/go.mod" ] && command -v go >/dev/null 2>&1; then
    mkdir -p "$BIN_DIR"
    (cd "$SRC/server" && go build -trimpath \
      -o "$native_candidate" ./cmd/forkmesh-mirror-node) ||
      die "Could not build the native forkmesh-mirror-node service."
    chmod 0755 "$native_candidate"
  fi
  [ -x "$native_candidate" ] || [ -x "$SYSTEMD_BIN" ] ||
    die "The authenticated release does not contain forkmesh-mirror-node; refusing to run the desktop client as a server."
  [ ! -L "$SYSTEMD_UNIT" ] && [ ! -L "$SYSTEMD_ENV" ] &&
  [ ! -L "$SYSTEMD_CONFIG" ] && [ ! -L "$SYSTEMD_BIN" ] &&
    [ ! -L "$SYSTEMD_INSTALL_MARKER" ] &&
    [ ! -L "$SYSTEMD_BOOTSTRAP_MARKER" ] ||
    die "Refusing a symlink in the managed mirror-service paths."
  # One-time migration from the installer-owned Qt server service. Validate
  # every old path and its recorded hash before removing it; operator-owned
  # services or binaries are never touched.
  local legacy_unit="/etc/systemd/system/forkmesh-node.service"
  local legacy_env="/etc/forkmesh/node.env"
  local legacy_bin="/usr/local/bin/forkmesh" legacy_hash current_hash
  if [ -f "$legacy_unit" ] &&
     grep -Fqx '# Managed-By: ForkMesh installer' "$legacy_unit" &&
     grep -Fqx "ExecStart=$legacy_bin" "$legacy_unit" &&
     [ -f "$SYSTEMD_INSTALL_MARKER" ] &&
     grep -Fqx 'forkmesh-system-service-v1' "$SYSTEMD_INSTALL_MARKER" &&
     grep -Fqx "binary=$legacy_bin" "$SYSTEMD_INSTALL_MARKER" &&
     [ -f "$SYSTEMD_STATE_MARKER" ] &&
     grep -Fqx 'forkmesh-managed-service-v1' "$SYSTEMD_STATE_MARKER" &&
     grep -Fqx "path=$state_dir" "$SYSTEMD_STATE_MARKER"; then
    systemctl disable --now forkmesh-node.service >/dev/null 2>&1 || true
    legacy_hash="$(sed -n 's/^sha256=//p' "$SYSTEMD_INSTALL_MARKER" | head -n1)"
    current_hash="$(_sha256_file "$legacy_bin")"
    if [ -n "$legacy_hash" ] && [ "$legacy_hash" = "$current_hash" ]; then
      rm -f -- "$legacy_bin"
    else
      warn "The legacy desktop service binary was modified; preserving $legacy_bin."
    fi
    rm -f -- "$legacy_unit" "$legacy_env" "$SYSTEMD_INSTALL_MARKER"
    rm -rf -- "$state_dir/.config/ForkMesh" \
      "$state_dir/.local/share/ForkMesh"
    systemctl daemon-reload
    managed_install=1
    say "Removed the installer-owned desktop server service; migrating to forkmesh-mirror-node."
  fi
  if [ -e "$SYSTEMD_BIN" ] || [ -e "$SYSTEMD_UNIT" ] ||
     [ -e "$SYSTEMD_STATE_DIR" ]; then
    if [ "$managed_install" -eq 1 ]; then
      :
    elif [ -f "$SYSTEMD_INSTALL_MARKER" ] &&
      grep -Fqx 'forkmesh-system-service-v1' "$SYSTEMD_INSTALL_MARKER" &&
      grep -Fqx "binary=$SYSTEMD_BIN" "$SYSTEMD_INSTALL_MARKER" &&
      [ -f "$SYSTEMD_UNIT" ] &&
      grep -Fqx '# Managed-By: ForkMesh installer' "$SYSTEMD_UNIT" &&
      [ -f "$SYSTEMD_STATE_MARKER" ] &&
      [ ! -L "$SYSTEMD_STATE_MARKER" ] &&
      [ "$(_path_owner_uid "$SYSTEMD_STATE_MARKER")" = "0" ] &&
      grep -Fqx 'forkmesh-managed-service-v1' "$SYSTEMD_STATE_MARKER" &&
      grep -Fqx "path=$SYSTEMD_STATE_DIR" "$SYSTEMD_STATE_MARKER"; then
      managed_install=1
    elif [ ! -e "$SYSTEMD_BIN" ] && [ ! -e "$SYSTEMD_UNIT" ] &&
         [ ! -e "$SYSTEMD_INSTALL_MARKER" ] &&
         [ -f "$SYSTEMD_BOOTSTRAP_MARKER" ] &&
         [ "$(_path_owner_uid "$SYSTEMD_BOOTSTRAP_MARKER")" = "0" ] &&
         grep -Fqx 'forkmesh-installing-service-v1' "$SYSTEMD_BOOTSTRAP_MARKER" &&
         grep -Fqx "path=$SYSTEMD_STATE_DIR" "$SYSTEMD_BOOTSTRAP_MARKER"; then
      # A previous installer run may have created the locked service account
      # and state directory before a mirror clone/download failed. The
      # root-owned, path-bound marker is written before those fallible steps,
      # so this exact partial transaction is safe to resume. An unmarked
      # directory remains protected as operator-owned data.
      bootstrap_install=1
    else
      die "Refusing to overwrite an unmanaged mirror binary, service, or state directory."
    fi
  fi

  if id "$service_user" >/dev/null 2>&1; then
    command -v getent >/dev/null 2>&1 ||
      die "getent is required to validate the existing service account."
    account="$(getent passwd "$service_user")"
    home="$(printf '%s' "$account" | awk -F: '{print $6}')"
    shell="$(printf '%s' "$account" | awk -F: '{print $7}')"
    [ "$(id -u "$service_user")" != "0" ] ||
      die "Refusing to use a root-valued forkmesh-node account."
    [ "$home" = "$state_dir" ] ||
      die "Existing forkmesh-node account has an unexpected home directory."
    case "$shell" in */nologin|*/false) ;; *)
      die "Existing forkmesh-node account does not use a locked login shell." ;;
    esac
  else
    command -v useradd >/dev/null 2>&1 ||
      die "useradd is required to create the unprivileged forkmesh-node service account."
    useradd --system --home-dir "$state_dir" --create-home \
      --shell /usr/sbin/nologin "$service_user"
  fi
  if [ "$managed_install" -eq 0 ] && [ "$bootstrap_install" -eq 0 ]; then
    {
      printf 'forkmesh-installing-service-v1\n'
      printf 'path=%s\n' "$state_dir"
    } > "$SYSTEMD_BOOTSTRAP_MARKER"
    chown root:root "$SYSTEMD_BOOTSTRAP_MARKER"
    chmod 0600 "$SYSTEMD_BOOTSTRAP_MARKER"
    bootstrap_install=1
  fi
  mkdir -p "$state_dir/tmp" "$state_dir/repositories" \
    "$state_dir/mirror-gateway" /etc/forkmesh
  chown -R "$service_user:$service_user" "$state_dir"
  chmod 0750 "$state_dir"
  chmod 0700 "$state_dir/tmp" "$state_dir/repositories" \
    "$state_dir/mirror-gateway"
  {
    printf 'forkmesh-managed-service-v1\n'
    printf 'path=%s\n' "$state_dir"
  } > "$SYSTEMD_STATE_MARKER"
  chown root:root "$SYSTEMD_STATE_MARKER"
  chmod 0600 "$SYSTEMD_STATE_MARKER"

  if [ -x "$native_candidate" ]; then
    install -m 0755 "$native_candidate" "$SYSTEMD_BIN"
  fi
  if [ -s "$SYSTEMD_CONFIG" ] &&
     [ -n "$FORKMESH_EXPECTED_RELEASE_VERSION" ]; then
    HOME="$state_dir" TMPDIR="$state_dir/tmp" \
      runuser -u "$service_user" -- "$SYSTEMD_BIN" \
        --config "$SYSTEMD_CONFIG" \
        --set-version "$FORKMESH_EXPECTED_RELEASE_VERSION" >/dev/null ||
      die "Could not update the managed mirror's reported release version."
  fi
  binary_hash="$(_sha256_file "$SYSTEMD_BIN")"
  printf '%s' "$binary_hash" | grep -Eq '^[0-9a-f]{64}$' ||
    die "Could not hash the staged system service binary."
  umask 077
  {
    printf 'HOME=%s\n' "$state_dir"
    printf 'TMPDIR=%s\n' "$state_dir/tmp"
  } > "$SYSTEMD_ENV"
  {
    printf '# Managed-By: ForkMesh installer\n'
    printf '[Unit]\nDescription=ForkMesh native mirror node\nAfter=network-online.target\nWants=network-online.target\n\n'
    printf '[Service]\nType=simple\nUser=%s\nGroup=%s\n' "$service_user" "$service_user"
    printf 'EnvironmentFile=%s\n' "$SYSTEMD_ENV"
    printf 'ExecStartPre=%s --config %s --check\n' "$SYSTEMD_BIN" "$SYSTEMD_CONFIG"
    printf 'ExecStart=%s --config %s\n' "$SYSTEMD_BIN" "$SYSTEMD_CONFIG"
    printf 'Restart=on-failure\nRestartSec=5\nNoNewPrivileges=true\n'
    printf 'PrivateTmp=true\nPrivateDevices=true\nProtectSystem=strict\nProtectHome=true\n'
    printf 'ReadWritePaths=%s\nRestrictSUIDSGID=true\nLockPersonality=true\n' "$state_dir"
    printf 'TasksMax=256\nLimitNOFILE=8192\nTimeoutStopSec=15\n\n'
    printf '[Install]\nWantedBy=multi-user.target\n'
  } > "$SYSTEMD_UNIT"
  {
    printf 'forkmesh-system-service-v1\n'
    printf 'binary=%s\n' "$SYSTEMD_BIN"
    printf 'sha256=%s\n' "$binary_hash"
  } > "$SYSTEMD_INSTALL_MARKER"
  chmod 0600 "$SYSTEMD_ENV" "$SYSTEMD_INSTALL_MARKER"
  chmod 0644 "$SYSTEMD_UNIT"
  systemctl daemon-reload
  rm -f -- "$SYSTEMD_BOOTSTRAP_MARKER"
  # A fresh service is intentionally not started yet: tunnel provisioning
  # initializes its native config, identity and connector token first. Existing
  # managed nodes already have those files and can be restarted immediately.
  if [ -s "$SYSTEMD_CONFIG" ] &&
     [ -s "$state_dir/mirror-gateway/connector.token" ]; then
    systemctl reset-failed forkmesh-mirror-node.service >/dev/null 2>&1 || true
    systemctl enable --now forkmesh-mirror-node.service
    LAUNCH_MODE="service"
  else
    LAUNCH_MODE="service-staged"
  fi
  LOG_PATH="journalctl -u forkmesh-mirror-node.service"
  return 0
}

launch_forkmesh() {
  case "$(uname -s)" in
    Darwin)
      LAUNCH_MODE="gui"
      if [ -d "$BUILD/ForkMesh.app" ]; then
        open "$BUILD/ForkMesh.app" && return 0
      fi
      open "$BIN" 2>/dev/null && return 0
      ;;
    *)
      if [ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ]; then
        LAUNCH_MODE="gui"
        if command -v setsid >/dev/null 2>&1; then
          setsid "$BIN" >/dev/null 2>&1 < /dev/null &
        else
          nohup "$BIN" >/dev/null 2>&1 < /dev/null &
        fi
        return 0
      fi
      # The desktop is a client only. A display-less root install may prepare
      # the separate native Go mirror service; a non-root install simply leaves
      # the client installed and does not turn it into a background server.
      LAUNCH_MODE="client-only"
      local rand
      # Mint the account link code (adhoc #53) unless the operator pre-set one.
      case "$FORKMESH_LINK_CODE" in
        [0-9][0-9][0-9][0-9][0-9][0-9]) ;;
        *)
          rand="$(od -An -N4 -tu4 /dev/urandom 2>/dev/null | tr -d '[:space:]')"
          [ -n "$rand" ] || rand=$(( $(date +%s) + $$ ))
          FORKMESH_LINK_CODE="$(printf '%06d' $(( rand % 1000000 )))"
          ;;
      esac
      if [ "$(id -u)" -eq 0 ]; then
        prepare_root_mirror_service
        return $?
      fi
      return 0
      ;;
  esac
  return 1
}

# --- automatic Cloudflare Tunnel provisioning (opt-in) -----------------------
# When FORKMESH_TUNNEL_HOSTNAME (e.g. mirror9.forkmesh.com) and
# CLOUDFLARE_API_TOKEN are both set, a fresh native node provisions its own
# direct-HTTPS mirror endpoint right after install: the pinned cloudflared and
# mirror tools are installed, the relay's mirror-router public key is
# discovered, tools/cloudflare_tunnel_bootstrap.py runs AS THE SERVICE USER
# (the API token only ever travels through the child environment — never argv,
# never logged), and the native service supervises the gateway, Tunnel
# connector, synchronization, and signed endpoint registration. Optional knobs:
#   FORKMESH_TUNNEL_ZONE            Cloudflare zone (default: hostname minus
#                                   its first label, e.g. forkmesh.com)
#   FORKMESH_CLOUDFLARE_ACCOUNT_ID  passed as --account-id when set
#   FORKMESH_RELAY_HOSTNAME         relay Worker host (default: $FORKMESH_HOST)
# Every failure here is soft: a tunnel that cannot be provisioned must never
# break the plain install that already succeeded.
FORKMESH_TUNNEL_HOSTNAME="${FORKMESH_TUNNEL_HOSTNAME:-}"
FORKMESH_TUNNEL_ZONE="${FORKMESH_TUNNEL_ZONE:-}"
FORKMESH_CLOUDFLARE_ACCOUNT_ID="${FORKMESH_CLOUDFLARE_ACCOUNT_ID:-}"
FORKMESH_RELAY_HOSTNAME="${FORKMESH_RELAY_HOSTNAME:-}"
FORKMESH_REQUIRE_TUNNEL="${FORKMESH_REQUIRE_TUNNEL:-0}"
FORKMESH_PUBLIC_HOSTNAME="${FORKMESH_PUBLIC_HOSTNAME:-${FORKMESH_TUNNEL_HOSTNAME:-}}"
FORKMESH_PUBLIC_ZONE="${FORKMESH_PUBLIC_ZONE:-${FORKMESH_TUNNEL_ZONE:-}}"
FORKMESH_PUBLIC_ADDRESS="${FORKMESH_PUBLIC_ADDRESS:-}"
FORKMESH_REQUIRE_DIRECT_HTTPS="${FORKMESH_REQUIRE_DIRECT_HTTPS:-${FORKMESH_REQUIRE_TUNNEL:-0}}"

# Copy the pinned mirror tools into the native service's read-only shared tools
# directory. Source order: an installer source checkout, then a sparse fetch
# from the resolved mirrors (the same way release metadata is fetched).
_tunnel_stage_tools() {
  local tools_dir="$1" tmp="" src="" source_root="" f
  local wanted="cloudflare_bootstrap.py cloudflared_install.py cloudflare_tunnel_bootstrap.py direct_https_bootstrap.py mirror_gateway.py node_operator_policy.py secret_gate.py"
  for src in "$SRC/tools" ""; do
    [ -n "$src" ] && [ -f "$src/cloudflare_tunnel_bootstrap.py" ] && break
  done
  if [ -z "$src" ]; then
    # Mirrors were already resolved for the binary download; a direct-upload
    # install that never resolved any must not hard-fail this soft path.
    [ "${#REPO_CANDIDATES[@]}" -gt 0 ] || return 1
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/forkmesh-tools.XXXXXX" 2>/dev/null)" || return 1
    local repo fetched=1
    for repo in "${REPO_CANDIDATES[@]}"; do
      # shellcheck disable=SC2086
      if _sparse_fetch_file "$repo" "$tmp" \
           tools/cloudflare_bootstrap.py \
           tools/cloudflare_tunnel_bootstrap.py \
           tools/cloudflared_install.py \
           tools/direct_https_bootstrap.py \
           tools/mirror_gateway.py \
           tools/node_operator_policy.py \
           tools/secret_gate.py \
           app/src/mirror_replica.py \
           app/src/public_snapshots.py \
           app/src/portable_sessions.py; then
        fetched=0
        break
      fi
    done
    if [ "$fetched" -ne 0 ]; then
      _rm_install_scratch "$tmp"
      return 1
    fi
    src="$tmp/tools"
  fi
  source_root="$(dirname "$src")"
  mkdir -p "$tools_dir" || { [ -n "$tmp" ] && _rm_install_scratch "$tmp"; return 1; }
  # Root installs run under umask 077. Without explicit traversal permissions,
  # the unprivileged forkmesh-node service cannot execute these public pinned
  # helpers even though the files themselves are installed as 0644.
  chmod 0755 "$(dirname "$tools_dir")" "$tools_dir" || {
    [ -n "$tmp" ] && _rm_install_scratch "$tmp"
    return 1
  }
  for f in $wanted; do
    [ -f "$src/$f" ] || continue
    install -m 0644 "$src/$f" "$tools_dir/$f" || { [ -n "$tmp" ] && _rm_install_scratch "$tmp"; return 1; }
  done
  # These pure-Python contracts are imported explicitly by mirror_gateway.py
  # even under systemd's python -I validation mode. The Worker imports the same
  # source files, so public replica schemas cannot drift between controller and
  # mirror implementations.
  for f in mirror_replica.py public_snapshots.py portable_sessions.py; do
    [ -f "$source_root/app/src/$f" ] || {
      [ -n "$tmp" ] && _rm_install_scratch "$tmp"
      return 1
    }
    install -m 0644 "$source_root/app/src/$f" "$tools_dir/$f" || {
      [ -n "$tmp" ] && _rm_install_scratch "$tmp"
      return 1
    }
  done
  [ -n "$tmp" ] && _rm_install_scratch "$tmp"
  [ -f "$tools_dir/cloudflare_tunnel_bootstrap.py" ] &&
    [ -f "$tools_dir/cloudflare_bootstrap.py" ] &&
    [ -f "$tools_dir/mirror_gateway.py" ] &&
    [ -f "$tools_dir/direct_https_bootstrap.py" ] &&
    [ -f "$tools_dir/mirror_replica.py" ] &&
    [ -f "$tools_dir/public_snapshots.py" ] &&
    [ -f "$tools_dir/portable_sessions.py" ]
}

# Provision host-owned HTTPS for a dedicated native mirror. Cloudflare is used
# only as an authoritative DNS API; the record is unproxied and points directly
# at this host. The legacy tunnel function below remains readable solely for
# rollback/migration compatibility and is no longer called by the installer.
FORKMESH_DNS_WAIT_SECONDS="${FORKMESH_DNS_WAIT_SECONDS:-3600}"
DIRECT_HTTPS_PENDING=0

_dns_record_type() {
  case "$1" in *:*) printf 'AAAA' ;; *) printf 'A' ;; esac
}

_dns_points_here() {
  local hostname="$1" address="$2" type answers
  type="$(_dns_record_type "$address")"
  answers="$(curl -fsS --connect-timeout 5 --max-time 10 \
    -H 'accept: application/dns-json' \
    "https://cloudflare-dns.com/dns-query?name=$hostname&type=$type" 2>/dev/null |
    python3 -c 'import json,sys
for answer in json.load(sys.stdin).get("Answer") or []:
    print(str(answer.get("data", "")).strip())' 2>/dev/null || true)"
  if [ -z "$answers" ] && command -v getent >/dev/null 2>&1; then
    answers="$(getent ahosts "$hostname" 2>/dev/null | awk '{print $1}' || true)"
  fi
  printf '%s\n' "$answers" | grep -Fxq "$address"
}

_wait_for_public_dns() {
  local hostname="$1" address="$2" type waited=0 limit
  limit="$FORKMESH_DNS_WAIT_SECONDS"
  printf '%s' "$limit" | grep -Eq '^[0-9]+$' || limit=3600
  _dns_points_here "$hostname" "$address" && return 0
  type="$(_dns_record_type "$address")"
  say ""
  say "Point your DNS at this server to finish HTTPS setup. Create this record:"
  say "  $hostname  $type  $address   (DNS only, no proxy)"
  say "Waiting up to $((limit / 60)) minutes for it to resolve…"
  while [ "$waited" -lt "$limit" ]; do
    sleep 15
    waited=$((waited + 15))
    _dns_points_here "$hostname" "$address" && {
      say "DNS for $hostname now points at this server."
      return 0
    }
    [ $((waited % 300)) -eq 0 ] &&
      say "  Still waiting for $hostname → $address ($((waited / 60)) min)…"
  done
  return 1
}

provision_direct_https() {
  CURRENT_STEP="direct-https"
  local hostname zone relay_host address state_home tools_dir router_body router_key
  local node_name upstream direct_ok attempt dns_mode="cloudflare"
  local service_user="forkmesh-node" link_attempt link_registered=0 health_tries
  hostname="$(printf '%s' "$FORKMESH_PUBLIC_HOSTNAME" | tr 'A-Z' 'a-z')"
  printf '%s' "$hostname" | grep -Eq '^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$' ||
    { warn "FORKMESH_PUBLIC_HOSTNAME is not a DNS hostname; skipping direct HTTPS provisioning."; return 1; }
  zone="$(printf '%s' "${FORKMESH_PUBLIC_ZONE:-${hostname#*.}}" | tr 'A-Z' 'a-z')"
  [ -n "${CLOUDFLARE_API_TOKEN:-}" ] || dns_mode="manual"
  relay_host="${FORKMESH_RELAY_HOSTNAME:-${FORKMESH_HOST#*://}}"
  relay_host="${relay_host%%/*}"
  [ "${EUID:-$(id -u)}" = "0" ] ||
    { warn "Direct HTTPS provisioning requires root."; return 1; }
  case "${LAUNCH_MODE:-}" in
    service|service-staged) ;;
    *) warn "Direct HTTPS targets a managed native mirror service."; return 1 ;;
  esac
  command -v python3 >/dev/null 2>&1 || return 1
  command -v runuser >/dev/null 2>&1 || return 1
  state_home="${SYSTEMD_STATE_DIR:-/var/lib/forkmesh}"
  tools_dir="$state_home/shared-tools"
  _tunnel_stage_tools "$tools_dir" || {
    warn "Could not stage the pinned direct-mirror tools."
    return 1
  }
  if ! command -v caddy >/dev/null 2>&1 ||
     ! python3 -c 'import cryptography' >/dev/null 2>&1; then
    say "Installing the host HTTPS server and signing support…"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq &&
      apt-get install -y -qq --no-install-recommends ca-certificates caddy python3-cryptography || {
        warn "Could not install the host HTTPS and signing dependencies."
        return 1
      }
  fi
  if command -v ufw >/dev/null 2>&1 &&
     ufw status | grep -Fq 'Status: active'; then
    ufw allow 80/tcp >/dev/null && ufw allow 443/tcp >/dev/null || {
      warn "Could not open the public HTTPS firewall ports."
      return 1
    }
  fi
  address="$FORKMESH_PUBLIC_ADDRESS"
  if [ -z "$address" ]; then
    address="$(curl -4fsS --connect-timeout 5 --max-time 10 https://api.ipify.org 2>/dev/null || true)"
  fi
  [ -n "$address" ] || {
    warn "Could not determine a public mirror address; set FORKMESH_PUBLIC_ADDRESS."
    return 1
  }
  node_name="$(printf '%s' "$FORKMESH_NODE_NAME" | tr 'A-Z' 'a-z')"
  printf '%s' "$node_name" | grep -Eq '^[a-z][a-z0-9-]{0,62}$' ||
    node_name="${hostname%%.*}"
  printf '%s' "$node_name" | grep -Eq '^[a-z][a-z0-9-]{0,62}$' || {
    warn "Could not derive a valid node name; set FORKMESH_NODE_NAME."
    return 1
  }
  router_body="$(curl -fsS -m 20 "https://$relay_host/api/mirrors/https" 2>/dev/null || true)"
  router_key="$(printf '%s' "$router_body" | python3 -c 'import json,sys; v=json.load(sys.stdin).get("routerPublicKey", ""); print(v if isinstance(v,str) else "")' 2>/dev/null || true)"
  printf '%s' "$router_key" | grep -Eq '^[A-Za-z0-9_-]{43}$' || {
    warn "Could not discover a valid mirror-router public key."
    return 1
  }
  ensure_mirror_candidates
  upstream="${REPO_CANDIDATES[0]:-}"
  [ -n "$upstream" ] || {
    warn "No healthy mirror upstream is available for bootstrap."
    return 1
  }
  say "Setting up $node_name at https://$hostname…"
  systemctl stop forkmesh-mirror-node.service >/dev/null 2>&1 || true
  HOME="$state_home" TMPDIR="$state_home/tmp" \
    runuser -u "$service_user" -- "$SYSTEMD_BIN" \
    --config "$SYSTEMD_CONFIG" --init \
    --state-dir "$state_home" --node "$node_name" \
    --owner forkmesh --repository forkmesh --upstream "$upstream" \
    --catalog-url "https://$relay_host/api/repositories" \
    --public-origin "https://$hostname" --mirror-url "https://$hostname" \
    --disable-cloudflared --router-public-key "$router_key" \
    --gateway-script "$tools_dir/mirror_gateway.py" \
    --python "$(command -v python3)" \
    --version "${FORKMESH_EXPECTED_RELEASE_VERSION:-unknown}" >/dev/null || {
      warn "Could not initialize the independent mirror configuration."
      return 1
    }
  if [ "$dns_mode" = "cloudflare" ]; then
    CLOUDFLARE_API_TOKEN="$CLOUDFLARE_API_TOKEN" \
      python3 "$tools_dir/direct_https_bootstrap.py" \
        --hostname "$hostname" --zone "$zone" --address "$address" \
        --adopt-dedicated-host >/dev/null || return 1
  else
    python3 "$tools_dir/direct_https_bootstrap.py" \
      --hostname "$hostname" --zone "$zone" --address "$address" \
      --adopt-dedicated-host --skip-dns >/dev/null || return 1
  fi
  caddy validate --config /etc/caddy/Caddyfile >/dev/null || return 1
  for link_attempt in 1 2 3; do
    if HOME="$state_home" TMPDIR="$state_home/tmp" \
      runuser -u "$service_user" -- "$SYSTEMD_BIN" \
        --config "$SYSTEMD_CONFIG" \
        --register-link-code "$FORKMESH_LINK_CODE" \
        --accounts-url "https://$relay_host/api/accounts" >/dev/null; then
      link_registered=1
      break
    fi
    sleep $((link_attempt * 2))
  done
  [ "$link_registered" -eq 1 ] ||
    warn "The node could not register its link code yet; it will still start."
  systemctl daemon-reload
  systemctl reset-failed forkmesh-mirror-node.service >/dev/null 2>&1 || true
  systemctl enable --now forkmesh-mirror-node.service || {
    warn "Could not start forkmesh-mirror-node.service."
    return 1
  }
  LAUNCH_MODE="service"
  if [ "$dns_mode" = "manual" ] &&
     ! _wait_for_public_dns "$hostname" "$address"; then
    systemctl enable caddy.service
    systemctl reload-or-restart caddy.service
    DIRECT_HTTPS_PENDING=1
    warn "DNS for $hostname does not point at $address yet. The node is running; HTTPS comes up on its own once the record resolves."
    [ "$FORKMESH_REQUIRE_DIRECT_HTTPS" = "1" ] && return 1
    return 0
  fi
  systemctl enable caddy.service
  systemctl reload-or-restart caddy.service
  direct_ok=0
  health_tries=12
  [ "$dns_mode" = "manual" ] && health_tries=36
  attempt=0
  while [ "$attempt" -lt "$health_tries" ]; do
    if curl -fsS --connect-timeout 3 --max-time 10 "https://$hostname/health" >/dev/null 2>&1 &&
       curl -fsS --connect-timeout 3 --max-time 10 "https://$hostname/forkmesh-mirror.json" |
         grep -Fq '"kind": "public-https-origin"'; then
      direct_ok=1
      break
    fi
    attempt=$((attempt + 1))
    sleep 5
  done
  [ "$direct_ok" = "1" ] || {
    warn "The independent public HTTPS endpoint did not become healthy."
    return 1
  }
  systemctl disable --now cloudflared.service cloudflared-forkmesh.service >/dev/null 2>&1 || true
  say "Independent HTTPS mirror endpoint provisioned: https://$hostname"
  diag direct-https 1
}

provision_cloudflare_tunnel() {
  CURRENT_STEP="tunnel"
  local hostname zone relay_host node_name account
  local state_home gw_root node_binary tools_dir cfd_dest cloudflared_path
  local service_user="forkmesh-node"
  hostname="$(printf '%s' "$FORKMESH_TUNNEL_HOSTNAME" | tr 'A-Z' 'a-z')"
  printf '%s' "$hostname" | grep -Eq '^[a-z0-9]([a-z0-9-]*[a-z0-9])?(\.[a-z0-9]([a-z0-9-]*[a-z0-9])?)+$' ||
    { warn "FORKMESH_TUNNEL_HOSTNAME is not a DNS hostname; skipping tunnel provisioning."; return 1; }
  zone="$(printf '%s' "${FORKMESH_TUNNEL_ZONE:-${hostname#*.}}" | tr 'A-Z' 'a-z')"
  relay_host="${FORKMESH_RELAY_HOSTNAME:-${FORKMESH_HOST#*://}}"
  relay_host="${relay_host%%/*}"
  case "$LAUNCH_MODE" in
    service|service-staged) ;;
    *) warn "Tunnel provisioning targets a managed native mirror service; skipping (launch mode: ${LAUNCH_MODE:-none})."; return 1 ;;
  esac
  command -v python3 >/dev/null 2>&1 ||
    { warn "python3 is required for tunnel provisioning; skipping."; return 1; }
  command -v runuser >/dev/null 2>&1 ||
    { warn "runuser is required to bootstrap the tunnel as $service_user; skipping."; return 1; }
  state_home="$SYSTEMD_STATE_DIR"
  gw_root="$state_home/mirror-gateway"
  node_binary="$SYSTEMD_BIN"
  tools_dir="/usr/local/share/forkmesh/tools"
  cfd_dest="/usr/local/bin/cloudflared"
  # Node name for the manifest/registration: the operator's chosen name when it
  # fits the relay's node-name shape, else the hostname's first label.
  node_name="$(printf '%s' "$FORKMESH_NODE_NAME" | tr 'A-Z' 'a-z')"
  printf '%s' "$node_name" | grep -Eq '^[a-z][a-z0-9-]{0,62}$' ||
    node_name="${hostname%%.*}"
  printf '%s' "$node_name" | grep -Eq '^[a-z][a-z0-9-]{0,62}$' ||
    { warn "Could not derive a valid node name for the tunnel; skipping."; return 1; }

  say "Provisioning the direct HTTPS mirror endpoint $hostname (zone $zone)…"

  if ! _tunnel_stage_tools "$tools_dir"; then
    warn "Could not stage the pinned mirror tools into $tools_dir; skipping tunnel provisioning."
    return 1
  fi
  say "  Pinned mirror tools staged in $tools_dir"

  # Pinned cloudflared connector. A system cloudflared on the PATH wins (same
  # preference the app applies); otherwise install the exact SHA-256-pinned
  # release with the bundled verifier — no shell pipeline.
  if command -v cloudflared >/dev/null 2>&1; then
    say "  cloudflared: already present ($(command -v cloudflared))"
  else
    if python3 "$tools_dir/cloudflared_install.py" \
         --destination "$cfd_dest" --json-stdout >/dev/null 2>&1; then
      say "  Installed the SHA-256-pinned cloudflared connector to $cfd_dest"
    else
      # The daemon can install its own managed copy on first start, so this is
      # a warning, not a dead end.
      warn "Could not install the pinned cloudflared now; the node will retry with its own verified installer."
    fi
  fi
  # A root umask of 077 can leave the pinned connector executable only by
  # root, while the service deliberately runs as forkmesh-node. Restrict this
  # correction to the installer-owned system destination.
  if [ -f "$cfd_dest" ] &&
     [ ! -L "$cfd_dest" ]; then
    chmod 0755 "$cfd_dest" || {
      warn "Could not make the managed cloudflared connector executable by $service_user."
      return 1
    }
  fi

  # The Worker's mirror-router public key — the same discovery endpoint the
  # desktop uses before registering a direct endpoint.
  local router_body router_key
  router_body="$(curl -fsS -m 20 "https://$relay_host/api/mirrors/https" 2>/dev/null || true)"
  router_key="$(printf '%s\n' "$router_body" |
    sed -n 's/.*"routerPublicKey"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)"
  if ! printf '%s' "$router_key" | grep -Eq '^[A-Za-z0-9_-]{43}$'; then
    warn "Could not discover a valid mirror-router public key from https://$relay_host/api/mirrors/https; skipping tunnel provisioning."
    return 1
  fi

  cloudflared_path="$(command -v cloudflared 2>/dev/null || true)"
  [ -n "$cloudflared_path" ] || cloudflared_path="$cfd_dest"
  if [ ! -s "$SYSTEMD_CONFIG" ]; then
    ensure_mirror_candidates
    [ "${#REPO_CANDIDATES[@]}" -gt 0 ] || {
      warn "No healthy upstream is available for native mirror bootstrap."
      return 1
    }
    say "  Initializing the native Go mirror service (the desktop is not launched)…"
    if ! HOME="$state_home" TMPDIR="$state_home/tmp" \
      runuser -u "$service_user" -- "$node_binary" \
        --config "$SYSTEMD_CONFIG" --init \
        --state-dir "$state_home" --node "$node_name" \
        --owner forkmesh --repository forkmesh \
        --upstream "${REPO_CANDIDATES[0]}" \
        --catalog-url "https://$relay_host/api/repositories" \
        --public-origin "https://$hostname" \
        --router-public-key "$router_key" \
        --gateway-script "$tools_dir/mirror_gateway.py" \
        --python "$(command -v python3)" \
        --cloudflared "$cloudflared_path" \
        --version "${FORKMESH_EXPECTED_RELEASE_VERSION:-unknown}" \
        >/dev/null; then
      warn "Could not initialize the native mirror-node configuration."
      return 1
    fi
  fi

  # Ask the native service binary for its public key; no Qt settings or desktop
  # process participates in mirror identity or publication.
  local node_pubkey
  node_pubkey="$(HOME="$state_home" TMPDIR="$state_home/tmp" \
    runuser -u "$service_user" -- "$node_binary" \
      --config "$SYSTEMD_CONFIG" --public-key 2>/dev/null || true)"
  if ! printf '%s' "$node_pubkey" | grep -Eq '^[A-Za-z0-9_-]{43}$'; then
    warn "Could not read the native mirror node public key; skipping tunnel provisioning."
    return 1
  fi

  systemctl stop forkmesh-mirror-node.service >/dev/null 2>&1 || true
  _tunnel_restart_node() {
    systemctl enable --now forkmesh-mirror-node.service ||
      warn "Could not start forkmesh-mirror-node.service; start it manually."
  }

  # Provision tunnel + DNS + signed manifest + connector token. The Cloudflare
  # API token travels only in the child environment.
  say "  Running the pinned Cloudflare Tunnel bootstrap as $service_user…"
  # A valid base64url Ed25519 key may begin with "-". Bind it with '=' so
  # argparse cannot mistake that leading character for another option.
  set -- \
    "$tools_dir/cloudflare_tunnel_bootstrap.py" \
    --hostname "$hostname" \
    --zone "$zone" \
    --node-name "$node_name" \
    --origin-host 127.0.0.1 \
    --origin-port 8790 \
    --gateway-config "$gw_root/config.json" \
    --mirror-public-key="$node_pubkey" \
    --manifest-signer-command "'$node_binary' --config '$SYSTEMD_CONFIG' --sign-mirror-manifest" \
    --manifest-output "$gw_root/forkmesh-mirror.json" \
    --tunnel-token-file "$gw_root/connector.token"
  if [ -n "$FORKMESH_CLOUDFLARE_ACCOUNT_ID" ]; then
    set -- "$@" --account-id "$FORKMESH_CLOUDFLARE_ACCOUNT_ID"
  fi
  local bootstrap_rc=0
  CLOUDFLARE_API_TOKEN="$CLOUDFLARE_API_TOKEN" \
  HOME="$state_home" TMPDIR="$state_home/tmp" PYTHONUNBUFFERED=1 \
    runuser -u "$service_user" -- python3 "$@" </dev/null || bootstrap_rc=$?
  if [ "$bootstrap_rc" -ne 0 ]; then
    warn "Cloudflare Tunnel bootstrap failed (exit $bootstrap_rc); the native mirror service was not started."
    return 1
  fi
  if [ ! -s "$gw_root/connector.token" ]; then
    warn "The Tunnel bootstrap wrote no connector token; the native mirror service was not started."
    return 1
  fi

  local link_attempt=1 link_registered=0
  while [ "$link_attempt" -le 3 ]; do
    if HOME="$state_home" TMPDIR="$state_home/tmp" \
      runuser -u "$service_user" -- "$node_binary" \
        --config "$SYSTEMD_CONFIG" \
        --register-link-code "$FORKMESH_LINK_CODE" \
        --accounts-url "https://$relay_host/api/accounts" >/dev/null; then
      link_registered=1
      break
    fi
    [ "$link_attempt" -eq 1 ] && sleep 2 || sleep 5
    link_attempt=$((link_attempt + 1))
  done
  [ "$link_registered" -eq 1 ] ||
    warn "The native node could not register its link code yet; mirror serving will still start."

  _tunnel_restart_node
  local stable_tick restarts main_pid
  for stable_tick in 1 2 3 4 5; do
    sleep 1
    systemctl is-active --quiet forkmesh-mirror-node.service || {
      warn "forkmesh-mirror-node.service did not remain active."
      return 1
    }
    main_pid="$(systemctl show forkmesh-mirror-node.service --property=MainPID --value)"
    restarts="$(systemctl show forkmesh-mirror-node.service --property=NRestarts --value)"
    printf '%s' "$main_pid" | grep -Eq '^[1-9][0-9]*$' || return 1
    [ "${restarts:-0}" = "0" ] || return 1
  done
  LAUNCH_MODE="service"
  say "Direct HTTPS mirror endpoint provisioned:"
  say "  Hostname:  https://$hostname  (tunnel + proxied DNS in zone $zone)"
  say "  Node name: $node_name    Relay: $relay_host"
  say "  forkmesh-mirror-node supervises sync, gateway, Tunnel, native intake,"
  say "  catalog publication, and signed endpoint registration."
  diag tunnel 1
  return 0
}

CURRENT_STEP="launch"
LOG_PATH="${XDG_DATA_HOME:-$HOME/.local/share}/forkmesh/node.log"
# Update-in-place restart (adhoc): the binary/build output has just been
# refreshed, so stop the daemon that is still running the OLD build before we
# relaunch — otherwise launch_forkmesh starts a second daemon alongside the
# stale one. Only the process is stopped; the node's identity key and mirrored
# data are left in place (this is not a reinstall). Skipped when nothing is
# going to be launched anyway.
if [ "$FORKMESH_RESTART" = "1" ] && [ "${FORKMESH_NO_LAUNCH:-0}" != "1" ]; then
  stop_forkmesh_daemons "restart before relaunch"
  cleanup_stale_service_mirror_tmp
fi
if [ "${FORKMESH_NO_LAUNCH:-0}" = "1" ]; then
  LAUNCH_MODE="skipped"
  diag launch 1 "skipped"
elif launch_forkmesh; then
  diag launch 1 "$LAUNCH_MODE"
else
  LAUNCH_MODE="manual"
  diag launch 1 "manual"
fi

# Automatic host-owned HTTPS provisioning for the native mirror node. Older
# callers may still send FORKMESH_TUNNEL_HOSTNAME; it is treated only as a
# hostname alias and does not enable the retired connector path.
if [ -n "$FORKMESH_PUBLIC_HOSTNAME" ]; then
  if ! provision_direct_https; then
    if [ "$FORKMESH_REQUIRE_DIRECT_HTTPS" = "1" ]; then
      die "Automatic direct HTTPS provisioning did not complete; this install requires a live independent endpoint."
    fi
    warn "Automatic direct HTTPS provisioning did not complete; the node is installed without a public endpoint."
  fi
fi

case "$LAUNCH_MODE" in
  gui)
    say "Done — launching the ForkMesh desktop client now."
    say "To open it again later, search \"ForkMesh\" in your applications menu, or type  forkmesh  in a terminal ($BIN)."
    ;;
  service)
    say "Done — the native ForkMesh mirror service is running."
    say "  Logs: journalctl -u forkmesh-mirror-node.service"
    say "  Stop: systemctl stop forkmesh-mirror-node.service"
    say ""
    if [ -n "$FORKMESH_PUBLIC_HOSTNAME" ]; then
      if [ "$DIRECT_HTTPS_PENDING" = "1" ]; then
        say "  Address: https://$FORKMESH_PUBLIC_HOSTNAME (live once DNS points here)"
      else
        say "  Address: https://$FORKMESH_PUBLIC_HOSTNAME"
      fi
    fi
    say ""
    say "FORKMESH LINK CODE: $FORKMESH_LINK_CODE"
    if [ -n "$FORKMESH_OWNER" ]; then
      say "  This links the new node to owner \"$FORKMESH_OWNER\"."
    fi
    ;;
  service-staged)
    say "Done — the desktop client was not launched as a server."
    warn "The native mirror service is staged but needs a public hostname before it can start."
    say "  Point a DNS name at this server and re-run with FORKMESH_PUBLIC_HOSTNAME=<that name>."
    ;;
  client-only)
    say "Done — the desktop client is installed but was not daemonized on this display-less machine."
    ;;
  *)
    say "Done. Open ForkMesh from your applications menu (search \"ForkMesh\"), or type  forkmesh  in a terminal ($BIN)."
    ;;
esac

# Whole install finished successfully; the EXIT trap only fires on failure.
CURRENT_STEP="done"
diag done 1

} # end of the stdin-drain brace group opened near the top — keep this last.
