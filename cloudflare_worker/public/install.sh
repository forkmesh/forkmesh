#!/usr/bin/env bash
# ForkMesh desktop client installer.
#   curl -fsSL https://forkmesh.com/install.sh | bash
# Autodetects this machine's OS/arch and installs the matching PREBUILT binary
# attached to the latest release (no compiler/Qt toolchain, no multi-minute
# build). When no prebuilt asset is published for the platform — or
# FORKMESH_FROM_SOURCE=1 is set — it falls back to cloning the repository and
# building the Qt client from source. Set FORKMESH_NO_SOURCE_FALLBACK=1 to
# disable that fallback and fail instead when no prebuilt binary is available
# (this is the default on headless Linux — see below). Missing build
# prerequisites are installed automatically when a supported package manager
# is detected; set FORKMESH_NO_INSTALL_DEPS=1 to opt out. The binary lands in
# ~/.local/bin.
set -euo pipefail

# Installer script version. Bump on every change to install.sh so a user can
# confirm — from the banner printed at startup — that they are running the
# freshly deployed script and not a cached/older copy from the CDN edge.
INSTALLER_VERSION="0.12.14 (2026-07-04)"

# ForkMesh is self-hosted: the same server that serves this script also serves
# the source over git's smart-HTTP protocol at https://<host>/<node>/<repo>.
# Override FORKMESH_HOST when self-hosting. By default, the installer asks the
# mainnode for the currently-online forkmesh host with the most recent uptime.
FORKMESH_HOST="${FORKMESH_HOST:-https://forkmesh.com}"
FORKMESH_NODE="${FORKMESH_NODE:-}"
# The name to give a freshly-deployed mirror node. The deploy UI passes the
# operator's chosen name as FORKMESH_NODE_NAME, leaving FORKMESH_NODE empty so
# the clone source still auto-resolves to a real online mirror. A headless launch
# hands this on to the app so the new node adopts the chosen name and
# auto-connects instead of sitting idle at the setup screen. Fall back to a
# verbatim FORKMESH_NODE (older deploy UI) so naming still works across skew.
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
# Link code for attaching this fresh node to the installing user's account
# (adhoc #53). A headless launch mints one (or honours a pre-set 6-digit value),
# prints it as "FORKMESH LINK CODE: NNNNNN", and hands it to the daemon, which
# presents it when it registers. The desktop app that drove the install offers
# the same code signed with its own key; the relay pairs the two halves and
# records the new node under that user.
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
FORKMESH_NAME="${FORKMESH_NAME:-forkmesh}"
FORKMESH_INSTALL_SOURCE_URL="${FORKMESH_INSTALL_SOURCE_URL:-${FORKMESH_HOST%/}/api/install-source}"
FORKMESH_DIAG_URL="${FORKMESH_DIAG_URL:-${FORKMESH_HOST%/}/api/install-diag}"
FORKMESH_FALLBACK_REPO="${FORKMESH_FALLBACK_REPO-https://github.com/forkmesh/forkmesh.git}"
REPO="${FORKMESH_REPO:-}"
# Space-separated list of online mirror nodes resolved from the mainnode, best
# first, and the matching list of clone URLs to try in order. A mirror can report
# itself online (a live host WebSocket) yet still time out the git clone proxy
# with a 504, so the installer falls back to the next mirror instead of dead-
# ending on the first one. Populated by resolve_install_node / the mirror block.
FORKMESH_NODES=""
REPO_CANDIDATES=()
# Set by clean_clone to the human-readable reason the last clone attempt failed
# (e.g. "mirror host timed out (HTTP 504)"), so the final error and the anonymous
# diagnostics can say WHY every mirror was unreachable rather than just "failed".
CLONE_FAIL_REASON=""
# The build checkout lives in a dedicated, installer-only location. The only
# thing that ever lives there is a throwaway clone used to build, so it is
# always safe to wipe and re-clone — the installer fully owns this path.
SRC="${FORKMESH_DIR:-$HOME/.local/share/forkmesh/src}"
BIN_DIR="${FORKMESH_BIN_DIR:-$HOME/.local/bin}"
BIN="$BIN_DIR/forkmesh"
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

# Whether to fall back to a source build when no prebuilt binary is published
# for this platform/arch. A headless Linux box (no DISPLAY/WAYLAND_DISPLAY) is
# almost always an unattended mirror-node deploy — pulling in a full
# git/cmake/compiler/Qt toolchain there is undesirable, and a same-platform
# release binary should always exist, so the fallback defaults to OFF (binary
# only) there. Every other case (headful Linux, macOS) defaults the fallback
# ON, unchanged from prior behaviour. Explicitly set FORKMESH_NO_SOURCE_FALLBACK
# to 1 or 0 to override the default either way.
if [ -z "${FORKMESH_NO_SOURCE_FALLBACK:-}" ]; then
  if [ "$(uname -s 2>/dev/null)" = "Linux" ] && [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    FORKMESH_NO_SOURCE_FALLBACK=1
  else
    FORKMESH_NO_SOURCE_FALLBACK=0
  fi
fi

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
# Verbose diagnostic line, printed only when FORKMESH_DEBUG=1. Use it for the
# extra detail a remote operator needs to debug a failed install (the resolved
# mirror list, the raw source response, the exact URLs being cloned) without
# cluttering the normal install log.
dbg()  { [ "${FORKMESH_DEBUG:-0}" = "1" ] && printf '\033[2m[debug]\033[0m %s\n' "$1" >&2 || true; }

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

diag() {
  [ "${FORKMESH_NO_DIAG:-0}" = "1" ] && return 0
  command -v curl >/dev/null 2>&1 || return 0
  local step="$1" ok="$2" detail="${3:-}"
  # Fire-and-forget in the background with a short timeout: diagnostics must
  # never slow down or fail the install, so every error is swallowed.
  curl -fsS -m 5 -X POST "$FORKMESH_DIAG_URL" \
    -H 'Content-Type: application/json' \
    --data "{\"run\":\"$RUN_ID\",\"step\":\"$step\",\"ok\":$ok,\"os\":\"$DIAG_OS\",\"arch\":\"$DIAG_ARCH\",\"pm\":\"${PM:-}\",\"distro\":\"$DIAG_DISTRO\",\"version\":\"$INSTALLER_VERSION\",\"detail\":\"$detail\"}" \
    >/dev/null 2>&1 </dev/null &
  return 0
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
  if ! body="$(curl -sSL -H 'Cache-Control: no-cache' -H 'Pragma: no-cache' "$source_url")"; then
    die "Could not check for an online ForkMesh mirror. Please try again shortly."
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
  if [ -z "$valid" ]; then
    if [ -n "${FORKMESH_FALLBACK_REPO:-}" ]; then
      warn "No online ForkMesh node is currently mirroring '$FORKMESH_NAME'. Using fallback repository: $FORKMESH_FALLBACK_REPO"
      FORKMESH_NODES="fallback"
      FORKMESH_NODE="fallback"
      return 0
    fi
    die "No online ForkMesh node is currently mirroring '$FORKMESH_NAME'. Start a node that publishes this repository, then try the installer again."
  fi
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
  local _node
  if [ "$FORKMESH_NODE" = "fallback" ] && [ -n "${FORKMESH_FALLBACK_REPO:-}" ]; then
    REPO_CANDIDATES+=("$FORKMESH_FALLBACK_REPO")
  else
    for _node in $FORKMESH_NODES; do
      REPO_CANDIDATES+=("${FORKMESH_HOST%/}/${_node}/${FORKMESH_NAME}")
    done
  fi
  REPO="${REPO_CANDIDATES[0]}"
  say "Using mirror node: $FORKMESH_NODE"
  if [ "${#REPO_CANDIDATES[@]}" -gt 1 ]; then
    say "  ${#REPO_CANDIDATES[@]} online mirrors available; will fall back if one is unreachable: $FORKMESH_NODES"
  fi
  diag mirror 1
}

# --- uninstall --------------------------------------------------------------
# Remove ForkMesh completely: the binary, the cloned source, the desktop
# launcher + icons, the login-autostart entry, AND every byte of user data
# (settings, the node identity key, all mirrored repositories, and chat
# history). A plain delete of the binary leaves this data behind — which is why
# a reinstall used to show an old node name and stale chat. Run with:
#   curl -fsSL https://forkmesh.com/install.sh | bash -s -- --uninstall
# Mode: "full" (the --uninstall entry point, exits when done) or "reinstall"
# (called inline before a fresh install — skips the interactive confirmation,
# since a reinstall is already gated by its own Qt-side dialog / explicit flag,
# and returns instead of exiting so the caller can carry on installing).
# Stop every running ForkMesh daemon on this host. Matches by exact process
# name AND by the known binary/source paths, then SIGKILLs stragglers. If a
# daemon survives (almost always because it is owned by another user, e.g. a
# root/sudo install), retries once with `sudo -n` and, failing that, warns
# loudly rather than leaving a phantom old node reporting to the network.
# $1: a short context label for the log line (unused beyond readability).
stop_forkmesh_daemons() {
  local running=0
  _fm_alive() { pgrep -x forkmesh >/dev/null 2>&1 \
    || pgrep -f -- "$BIN" >/dev/null 2>&1 \
    || pgrep -f -- "$SRC" >/dev/null 2>&1; }
  if _fm_alive; then
    running=1
    pkill -x forkmesh   2>/dev/null || true
    pkill -f -- "$BIN"  2>/dev/null || true
    pkill -f -- "$SRC"  2>/dev/null || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do _fm_alive || break; sleep 0.5; done
    if _fm_alive; then
      pkill -9 -x forkmesh   2>/dev/null || true
      pkill -9 -f -- "$BIN"  2>/dev/null || true
      pkill -9 -f -- "$SRC"  2>/dev/null || true
    fi
  fi
  # Still alive after SIGKILL => not ours to signal. Try a non-interactive sudo
  # (never prompts, so `curl | bash` can't hang), then give up with a warning.
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
  elif [ "$running" = "1" ]; then
    say "Stopped the running ForkMesh daemon"
  fi
}

uninstall_forkmesh() {
  local mode="${1:-full}"; shift || true
  local data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
  local config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
  local cache_home="${XDG_CACHE_HOME:-$HOME/.cache}"

  # Directories ForkMesh owns. QSettings org+app are both "ForkMesh", so the
  # config/data/cache live under a capitalised "ForkMesh" dir; the installer's
  # own checkout lives under the lowercase "forkmesh".
  local dirs=(
    "$config_home/ForkMesh"          # settings (node name, server, prefs)
    "$data_home/ForkMesh"            # identity key, mirrors, repos, chat, actions
    "$cache_home/ForkMesh"          # caches
    "$HOME/.forkmesh"               # IDE-extension handoff dir
    "$data_home/forkmesh"           # installer source checkout (parent of $SRC)
  )
  # Loose files: binary, desktop launcher, autostart entry, installed icons.
  local files=(
    "$BIN"
    "$data_home/applications/forkmesh.desktop"
    "$config_home/autostart/forkmesh.desktop"
    "$data_home/icons/forkmesh.png"
  )

  if [ "$mode" = "reinstall" ]; then
    printf '\033[31mReinstall: removing the existing ForkMesh install and ALL of its data first:\033[0m\n'
  else
    printf '\033[31mThis removes ForkMesh and ALL of its data from this computer:\033[0m\n'
  fi
  printf '  • the forkmesh binary and installed source\n'
  printf '  • settings, the node identity key (the account cannot be recovered)\n'
  printf '  • every mirrored repository, and all chat history\n'
  printf '  • the desktop launcher, icons, and login-autostart entry\n\n'

  # Honour a non-interactive confirm so `curl | bash` works: pass --yes (or set
  # FORKMESH_ASSUME_YES=1). Otherwise prompt when a terminal is attached. A
  # reinstall skips this gate entirely — it is already an explicit, pre-confirmed
  # action (the Qt-side dialog / the --reinstall flag).
  if [ "$mode" != "reinstall" ] \
     && [ "${FORKMESH_ASSUME_YES:-0}" != "1" ] && [ "${1:-}" != "--yes" ]; then
    if [ -t 0 ]; then
      printf 'Type DELETE to continue: '
      local answer=""; read -r answer || answer=""
      [ "$answer" = "DELETE" ] || die "Uninstall cancelled."
    else
      die "Refusing to uninstall without confirmation. Re-run with --yes (or set FORKMESH_ASSUME_YES=1) to proceed:  curl -fsSL $FORKMESH_HOST/install.sh | bash -s -- --uninstall --yes"
    fi
  fi

  # A headless install (e.g. a VPS) runs the binary as a plain background
  # process (nohup, no systemd unit — see the daemon launch below). Deleting
  # the binary out from under it just unlinks the inode: the running process
  # keeps executing from the deleted file and keeps reporting its (now stale)
  # presence/version to the network, which is why mirrors could still show an
  # old version after "uninstalling" the host. Stop it first — and match it
  # more than one way, because the process we must kill may NOT be the binary
  # at the current $BIN path:
  #   • by exact process name ("forkmesh")  — catches an OLD install that lived
  #     at a different path, and a copy still executing from a deleted/replaced
  #     inode after an in-app update (the classic "still on an old version").
  #   • by $BIN and by the source build dir — the normal and in-place locations.
  stop_forkmesh_daemons "installer uninstall"

  local d f
  for d in "${dirs[@]}"; do
    if [ -e "$d" ]; then rm -rf -- "$d" && say "Removed $d"; fi
  done
  for f in "${files[@]}"; do
    if [ -e "$f" ]; then rm -f -- "$f" && say "Removed $f"; fi
  done
  # Every hicolor icon bucket the installer may have written.
  find "$data_home/icons/hicolor" -name 'forkmesh.png' -delete 2>/dev/null || true
  # Refresh desktop caches so the launcher disappears promptly.
  command -v update-desktop-database >/dev/null 2>&1 \
    && update-desktop-database "$data_home/applications" >/dev/null 2>&1 || true
  command -v gtk-update-icon-cache >/dev/null 2>&1 \
    && gtk-update-icon-cache -f -t "$data_home/icons/hicolor" >/dev/null 2>&1 || true
  if [ "$mode" = "reinstall" ]; then
    say "Previous ForkMesh install removed; installing a fresh copy now."
    return 0
  fi
  say "ForkMesh has been completely removed."
  exit 0
}

# Dispatch uninstall before any install work (and before the diag EXIT trap can
# misreport a clean uninstall as a failed install step). --reinstall wipes the
# old install then falls through into the normal install below (adhoc #258).
for arg in "$@"; do
  case "$arg" in
    --uninstall|--remove|-u) CURRENT_STEP="uninstall"; trap - EXIT; shift || true
      uninstall_forkmesh full "$@" ;;
    --reinstall) FORKMESH_REINSTALL=1; shift || true ;;
  esac
done
if [ "$FORKMESH_REINSTALL" = "1" ]; then
  CURRENT_STEP="reinstall"
  say "Reinstall requested — clearing the existing install before reinstalling."
  uninstall_forkmesh reinstall
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
run_pm() {
  if [ "$PM" = "brew" ]; then
    say "Running: $*"
    "$@"
  else
    say "Running: ${SUDO:+$SUDO }$*"
    $SUDO "$@"
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
    apt:cmake)      echo cmake ;;
    apt:compiler)   echo "g++" ;;
    apt:qt)         echo "qt6-base-dev qt6-svg-dev" ;;
    apt:openssl)    echo libssl-dev ;;

    dnf:git|yum:git)            echo git ;;
    dnf:cmake|yum:cmake)        echo cmake ;;
    dnf:compiler|yum:compiler)  echo "gcc-c++" ;;
    dnf:qt|yum:qt)              echo "qt6-qtbase-devel qt6-qtsvg-devel" ;;
    dnf:openssl|yum:openssl)    echo openssl-devel ;;

    pacman:git)       echo git ;;
    pacman:cmake)     echo cmake ;;
    pacman:compiler)  echo gcc ;;
    pacman:qt)        echo "qt6-base qt6-svg" ;;
    pacman:openssl)   echo openssl ;;

    zypper:git)       echo git ;;
    zypper:cmake)     echo cmake ;;
    zypper:compiler)  echo "gcc-c++" ;;
    zypper:qt)        echo "qt6-base-devel qt6-svg-devel" ;;
    zypper:openssl)   echo libopenssl-devel ;;

    apk:git)       echo git ;;
    apk:cmake)     echo "cmake make" ;;
    apk:compiler)  echo "g++" ;;
    apk:qt)        echo "qt6-qtbase-dev qt6-qtsvg-dev" ;;
    apk:openssl)   echo "openssl-dev" ;;

    brew:git)       echo git ;;
    brew:cmake)     echo cmake ;;
    brew:qt)        echo qt ;;
    brew:openssl)   echo "openssl@3" ;;
    brew:compiler)  echo "" ;;  # provided by Xcode CLT, handled separately
  esac
}

# Ensure a prerequisite is present, installing it if missing and possible.
# Args: <logical name> <test command...>
ensure() {
  local what="$1"; shift
  if "$@" >/dev/null 2>&1; then
    say "  $what: already present"
    return 0
  fi

  if [ "${FORKMESH_NO_INSTALL_DEPS:-0}" = "1" ]; then
    die "'$what' is required but not installed (auto-install disabled via FORKMESH_NO_INSTALL_DEPS)."
  fi
  if [ -z "$PM" ]; then
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

  "$@" >/dev/null 2>&1 || die "Installed $pkgs but '$what' is still unavailable."
}

# A C++ compiler can come from any of cc/clang/g++ (or Xcode CLT on macOS).
have_compiler() {
  command -v cc >/dev/null 2>&1 || command -v clang >/dev/null 2>&1 \
    || command -v g++ >/dev/null 2>&1 || command -v c++ >/dev/null 2>&1
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

if detect_pm; then
  say "Detected package manager: $PM (${PM_INSTALL[*]})"
else
  warn "No supported package manager found; missing tools cannot be auto-installed."
fi

CURRENT_STEP="deps"
say "Checking build prerequisites (git, cmake, compiler, Qt 6, OpenSSL)…"
ensure git    command -v git

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
  # Release channel: the directory under releases/ the workflow publishes into.
  RELEASE_CHANNEL="${FORKMESH_RELEASE:-latest}"
  ASSET_REL_PATH="releases/${RELEASE_CHANNEL}/${ASSET_NAME}"
}

# Sparse-fetch a single committed file (repo-relative path $3) from clone URL $1
# into dir $2, without checking out the whole tree. Prefers a blobless clone
# (fetches only that one blob); if the mirror/relay does not honour partial-clone
# filters, retries with a plain shallow clone so the fast path still works (it
# just transfers more). Leaves the file at "$2/$3" on success.
_sparse_fetch_file() {
  local repo="$1" tmp="$2"; shift 2
  # The first path is required — success is gated on it being present and
  # non-empty. Any extra paths are fetched best-effort in the SAME checkout (e.g.
  # release.json alongside SHASUMS256.txt), so the canonical owner/repo can be
  # read without a second clone.
  local paths=("$@") mode
  for mode in "--filter=blob:none" ""; do
    rm -rf "$tmp"; mkdir -p "$tmp" || return 1
    # shellcheck disable=SC2086
    if git clone --quiet --depth 1 $mode --no-checkout "$repo" "$tmp" >/dev/null 2>&1 \
        && git -C "$tmp" sparse-checkout set --no-cone "${paths[@]}" >/dev/null 2>&1 \
        && git -C "$tmp" checkout --quiet >/dev/null 2>&1 \
        && [ -s "$tmp/${paths[0]}" ]; then
      return 0
    fi
  done
  return 1
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

# Echo the sha256 of file $1 (Linux sha256sum / macOS shasum), or empty if no
# checksum tool is available (download then installs unverified, with a warning).
_sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}'
  else echo ""; fi
}

# Install binary file $1 to $BIN (mode 0755), creating $BIN_DIR. Non-zero on fail.
_install_binary() {
  mkdir -p "$BIN_DIR" || return 1
  install -m 0755 "$1" "$BIN" 2>/dev/null || { cp "$1" "$BIN" && chmod 0755 "$BIN"; }
}

# Install the prebuilt binary for this platform. New model (issue #304): release
# binaries are NOT committed to git. The installer reads the tiny committed
# release manifest (SHASUMS256.txt, fetched over the git proxy) to learn the
# platform asset's content hash, downloads the bytes from the relay's
# content-addressed release endpoint, and VERIFIES the sha256 before installing.
# Falls back to a legacy release that still committed the binary into
# releases/<channel>/, and then (via the caller) to a source build. Returns
# non-zero when git is unavailable or no mirror can serve a verified asset.
install_prebuilt_release() {
  command -v git >/dev/null 2>&1 || return 1
  ensure_mirror_candidates
  local tmp repo sums manifest canon hash url bin got
  tmp="$(mktemp -d "${TMPDIR:-/tmp}/forkmesh-prebuilt.XXXXXX" 2>/dev/null)" || return 1
  sums="releases/${RELEASE_CHANNEL}/SHASUMS256.txt"
  manifest="releases/${RELEASE_CHANNEL}/release.json"
  for repo in "${REPO_CANDIDATES[@]}"; do
    # New model: manifest checksum + content-addressed download (+ verify). Fetch
    # release.json in the same checkout so the blob can be requested from the repo
    # that staged it — not the mirror that happened to serve this clone.
    if command -v curl >/dev/null 2>&1 && _sparse_fetch_file "$repo" "$tmp" "$sums" "$manifest"; then
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
        if curl -fsSL "$url" -o "$bin" 2>/dev/null && [ -s "$bin" ]; then
          got="$(_sha256_file "$bin")"
          if [ -n "$got" ] && [ "$got" != "$hash" ]; then
            warn "Checksum mismatch for $ASSET_NAME (expected $hash, got $got); skipping."
          elif _install_binary "$bin"; then
            [ -n "$got" ] || warn "No sha256 tool found; installed $ASSET_NAME unverified."
            REPO="$repo"; rm -rf "$tmp"
            say "Installed prebuilt ForkMesh ${ASSET_OS}/${ASSET_ARCH} binary to $BIN"
            return 0
          fi
        fi
      fi
    fi
    # Legacy model: binary committed directly into releases/<channel>/.
    if _sparse_fetch_file "$repo" "$tmp" "$ASSET_REL_PATH" && _install_binary "$tmp/$ASSET_REL_PATH"; then
      REPO="$repo"; rm -rf "$tmp"
      say "Installed prebuilt ForkMesh ${ASSET_OS}/${ASSET_ARCH} binary to $BIN"
      return 0
    fi
  done
  rm -rf "$tmp"
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
  _install_binary "$FORKMESH_LOCAL_BINARY" || return 1
  say "Installed the directly-uploaded ForkMesh binary to $BIN"
  return 0
}

detect_release_asset
if [ "${FORKMESH_FROM_SOURCE:-0}" != "1" ]; then
  CURRENT_STEP="prebuilt"
  if install_local_binary; then
    INSTALLED_PREBUILT=1
    ensure_qt_runtime
    diag prebuilt 1 "uploaded"
  elif install_prebuilt_release; then
    INSTALLED_PREBUILT=1
    ensure_qt_runtime
    diag prebuilt 1 "$ASSET_NAME"
  elif [ "$FORKMESH_NO_SOURCE_FALLBACK" = "1" ]; then
    diag prebuilt 0 "$ASSET_NAME"
    die "No prebuilt ForkMesh binary is published for ${ASSET_OS}/${ASSET_ARCH}, and falling back to a source build is disabled (FORKMESH_NO_SOURCE_FALLBACK=1, the default on headless Linux). Publish a prebuilt binary for this platform, or re-run with FORKMESH_NO_SOURCE_FALLBACK=0 to allow a source build."
  else
    say "No prebuilt binary published for ${ASSET_OS}/${ASSET_ARCH}; building from source."
    diag prebuilt 0 "$ASSET_NAME"
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

# Qt 6 (Widgets, Network, Svg) and OpenSSL are required by CMake. Install the
# dev packages up front. This is a hard requirement: if the package manager
# can't provide them the build is guaranteed to fail at configure time with a
# confusing "Could NOT find Qt6Svg" error, so fail here with a clear message
# instead. The qt6 dev metapackages pull in the Svg/SvgWidgets components.
if [ -n "$PM" ] && [ "${FORKMESH_NO_INSTALL_DEPS:-0}" != "1" ]; then
  qt_pkgs="$(pkg_for qt)"
  if [ -n "$qt_pkgs" ]; then
    say "Installing Qt 6 dev libraries ($qt_pkgs)"
    # shellcheck disable=SC2086
    pm_install $qt_pkgs || die "Failed to install Qt 6 dev packages ($qt_pkgs) via $PM."
  fi
  ssl_pkgs="$(pkg_for openssl)"
  if [ -n "$ssl_pkgs" ]; then
    say "Installing OpenSSL dev libraries ($ssl_pkgs)"
    # shellcheck disable=SC2086
    pm_install $ssl_pkgs || die "Failed to install OpenSSL dev packages ($ssl_pkgs) via $PM."
  fi
else
  say "ForkMesh requires Qt 6 (Widgets, Network, Svg) and OpenSSL."
  say "  Debian/Ubuntu: sudo apt install qt6-base-dev qt6-svg-dev libssl-dev cmake g++"
  say "  Fedora:        sudo dnf install qt6-qtbase-devel qt6-qtsvg-devel openssl-devel cmake gcc-c++"
  say "  macOS:         brew install qt openssl@3 cmake"
fi
# detail records which prerequisites had to be installed ("none" = all present),
# so the funnel shows how often a machine already met the requirements.
diag deps 1 "${DIAG_MISSING:-none}"

# --- fetch + build + install (clean-reclone retry on any failure) -----------
# A leftover checkout from an interrupted earlier run can be stale or incomplete
# (e.g. the directory exists but qt_client/CMakeLists.txt is missing), which
# breaks the build in confusing ways. Each phase below returns non-zero instead
# of aborting, so on ANY failure we can wipe the source tree and run the whole
# pipeline once more from a clean clone.

# Marker written into a checkout this installer created. It is only used to tell
# an installer-made checkout (which we can fast-forward) apart from anything else
# sitting on $SRC (which we just re-clone). $SRC is a dedicated, installer-owned
# build path, so it is always safe to wipe — there is nothing precious to guard.
MANAGED_MARKER=".forkmesh-managed"
owns_src() { [ -f "$SRC/$MANAGED_MARKER" ]; }

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
    *"Host timed out"*|*"error: 504"*|*" 504"*)                 echo "mirror host timed out (HTTP 504)" ;;
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

# Replace $SRC with a fresh shallow clone. Clone into a temporary sibling first
# and swap it into place only after the clone fully succeeds, so a failed clone
# (e.g. no mirror currently serving the repo) can never leave the user with a
# half-deleted or missing $SRC. Tries each resolved mirror in REPO_CANDIDATES in
# turn, so one unreachable mirror (504/timeout) falls through to the next online
# one instead of dead-ending the install.
clean_clone() {
  local tmp="$SRC.new.$$"
  local repo out rc reason node total="${#REPO_CANDIDATES[@]}" idx=0
  CLONE_FAIL_REASON=""
  for repo in "${REPO_CANDIDATES[@]}"; do
    idx=$((idx + 1))
    node="$(repo_node "$repo")"
    rm -rf "$tmp"
    if [ "$total" -gt 1 ]; then
      say "Cloning $repo  (mirror $idx of $total)"
    else
      say "Cloning $repo"
    fi
    # Capture output so we can recognise the relay's integrity-gate rejection and
    # classify the failure; the output is still echoed so normal progress shows.
    out="$(git clone --depth 1 "$repo" "$tmp" 2>&1)"; rc=$?
    printf '%s\n' "$out"
    if [ "$rc" -eq 0 ]; then
      : > "$tmp/$MANAGED_MARKER"
      rm -rf "$SRC"
      mv "$tmp" "$SRC"
      REPO="$repo"   # remember the mirror that actually served the clone
      return 0
    fi
    rm -rf "$tmp"
    reason="$(classify_clone_failure "$out")"
    CLONE_FAIL_REASON="$reason"
    # A failed integrity pin is the relay refusing every mirror of this repo, not
    # a per-mirror outage, so trying the rest is pointless — stop and let the
    # caller surface the owner-actionable pin help.
    case "$out" in
      *"failed integrity check"*|*"repository failed integrity"*) PIN_FAILURE=1; return 1 ;;
    esac
    warn "Mirror '$node' could not be cloned: $reason."
    [ "$idx" -lt "$total" ] && say "  Falling back to the next online mirror…"
  done
  return 1
}

fetch_source() {
  mkdir -p "$(dirname "$SRC")" || return 1
  if [ -d "$SRC/.git" ]; then
    if ! owns_src; then
      warn "$SRC is an existing checkout not created by this installer; replacing it with a fresh clone."
      clean_clone || return 1
      return 0
    fi
    say "Updating existing checkout in $SRC"
    # The stored remote was baked with the node id live at the original install.
    # That node may now be offline while a different mirror is online, so pulling
    # the old URL returns 503. Repoint origin at the freshly resolved live mirror
    # before pulling, and fall back to a clean re-clone if the pull still fails.
    git -C "$SRC" remote set-url origin "$REPO" 2>/dev/null || true
    if ! git -C "$SRC" pull --ff-only; then
      warn "Could not update from $REPO; re-cloning from the current live mirror."
      clean_clone || return 1
    fi
  else
    clean_clone || return 1
  fi
  # git pull can say "Already up to date" yet leave a tree missing the Qt sources
  # CMake builds from (stale/partial mirror). Success of the fetch is not proof
  # the build inputs exist, so verify the actual file and re-clone if it is gone.
  if [ ! -f "$SRC/qt_client/CMakeLists.txt" ]; then
    warn "Checkout in $SRC is incomplete (no qt_client/CMakeLists.txt); re-cloning."
    clean_clone || return 1
  fi
  [ -f "$SRC/qt_client/CMakeLists.txt" ] || return 1
}

build_client() {
  local jobs build_dir
  jobs="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )"
  build_dir="$SRC/qt_client/build"
  local cmake_args=(-S "$SRC/qt_client" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DFORKMESH_BUILD_TESTS=OFF)
  if command -v brew >/dev/null 2>&1; then
    local qt_prefix ssl_prefix
    qt_prefix="$(brew --prefix qt 2>/dev/null || true)"
    ssl_prefix="$(brew --prefix openssl@3 2>/dev/null || true)"
    [ -n "$qt_prefix" ] && cmake_args+=("-DCMAKE_PREFIX_PATH=$qt_prefix")
    [ -n "$ssl_prefix" ] && cmake_args+=("-DOPENSSL_ROOT_DIR=$ssl_prefix")
  fi
  say "Configuring"
  cmake "${cmake_args[@]}" || return 1
  say "Building (this can take a few minutes)"
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
  warn "Install failed; retrying once from a clean clone."
  rm -rf "$SRC"
  if ! attempt_install; then
    [ "$PIN_FAILURE" = "1" ] && pin_failure_help
    # Name the last failure reason and every mirror that was tried so the cause
    # is obvious from the final line alone (e.g. all mirrors returned a 504).
    if [ "${CLONE_FAIL_REASON:-}" ]; then
      warn "Mirrors tried: ${FORKMESH_NODES:-$REPO}"
      warn "Re-run with FORKMESH_DEBUG=1 for the full git/HTTP trace."
      die "Could not fetch the source from any online mirror ($CLONE_FAIL_REASON). All mirrors are unreachable right now — please try again shortly."
    fi
    die "Install failed again after a clean re-clone; see the messages above for the cause."
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
# qt_client/install.sh that ships in the cloned source, so the launcher entry
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
  local script="$SRC/qt_client/install.sh"
  if [ ! -f "$script" ]; then
    # The prebuilt fast path never clones the source, so the helper that writes
    # the .desktop entry isn't present — that's expected, not an error. The app
    # still runs from $BIN on the PATH.
    if [ "$INSTALLED_PREBUILT" = "1" ]; then
      say "Installed the prebuilt binary; skipping app-menu registration (run:  forkmesh)."
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
# On a desktop this opens the GUI. On a headless box — the usual case for a
# deployed mirror node — it starts the node as a detached BACKGROUND DAEMON
# instead of just printing a hint: otherwise nothing runs, so the node never
# joins the network or appears in the Mirror nodes list even though the install
# "succeeded". LAUNCH_MODE records which path ran so the caller prints the right
# message.
LAUNCH_MODE=""
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
      # Headless: bring the node up as a background daemon. Reading stdin from
      # /dev/null makes the headless console drop straight into daemon mode (it
      # keeps serving once stdin closes) rather than blocking at a prompt nothing
      # is attached to. Running as root needs --allow-root to clear the built-in
      # root refusal. FORKMESH_NODE_NAME hands the app the operator's chosen name
      # so the fresh node adopts it and auto-connects.
      LAUNCH_MODE="daemon"
      local args="" log rand
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
        # Clear the built-in root refusal both for this launch (the arg) AND for
        # any process the node later relaunches as itself — an in-app update does
        # QProcess::startDetached(forkmesh) with NO args, which would otherwise hit
        # the refusal and silently kill the root daemon. Exporting the env override
        # makes that relaunched child inherit the allowance; the bare arg would not
        # survive it.
        args="--allow-root"
        export FORKMESH_ALLOW_ROOT=1
      fi
      log="${XDG_DATA_HOME:-$HOME/.local/share}/forkmesh/node.log"
      mkdir -p "$(dirname "$log")" 2>/dev/null || true
      if command -v setsid >/dev/null 2>&1; then
        FORKMESH_NODE_NAME="$FORKMESH_NODE_NAME" FORKMESH_LINK_CODE="$FORKMESH_LINK_CODE" setsid "$BIN" $args >"$log" 2>&1 < /dev/null &
      else
        FORKMESH_NODE_NAME="$FORKMESH_NODE_NAME" FORKMESH_LINK_CODE="$FORKMESH_LINK_CODE" nohup "$BIN" $args >"$log" 2>&1 < /dev/null &
      fi
      return 0
      ;;
  esac
  return 1
}

CURRENT_STEP="launch"
LOG_PATH="${XDG_DATA_HOME:-$HOME/.local/share}/forkmesh/node.log"
if [ "${FORKMESH_NO_LAUNCH:-0}" = "1" ]; then
  say "Done. Launch it with:  forkmesh"
  say "  On a server with no display, forkmesh opens an interactive CLI."
  diag launch 1 "skipped"
elif launch_forkmesh; then
  if [ "$LAUNCH_MODE" = "daemon" ]; then
    say "Done — ForkMesh is running as a background daemon."
    say "  The node will join the network and appear in the Mirror nodes list shortly."
    say "  Logs: $LOG_PATH    Stop: pkill -f '$BIN'"
    # The ForkMesh desktop app watches an SSH install's stream for this exact
    # line and pops up a link dialog prefilled with the code (adhoc #53).
    say ""
    say "FORKMESH LINK CODE: $FORKMESH_LINK_CODE"
    if [ -n "$FORKMESH_OWNER" ]; then
      say "  This links the new node to owner \"$FORKMESH_OWNER\"."
    fi
    say "  Enter this code in your ForkMesh desktop app to link the new node"
    say "  to your account (a popup opens during a Hosts-panel install; the"
    say "  code expires 30 minutes after the node registers)."
    # Stream the freshly-started daemon's own log into this SSH session for a
    # short bounded window (adhoc #226). Without this the node's live startup —
    # connecting to the relay, registering, syncing the catalog — vanishes into
    # a log file on the remote box and the operator's installer screen just
    # shows "running as a background daemon" then stops. Tailing it here lets
    # the desktop app's Live output box show the node actually coming alive.
    if command -v tail >/dev/null 2>&1; then
      say ""
      say "--- Live node output (first few seconds) ---"
      # Wait briefly for the daemon to create/populate the log, then follow it.
      _w=0
      while [ ! -s "$LOG_PATH" ] && [ "$_w" -lt 40 ]; do sleep 0.25; _w=$((_w+1)); done
      tail -n +1 -f "$LOG_PATH" 2>/dev/null &
      _tail_pid=$!
      sleep 15
      # kill+wait deliberately end the tail early; under `set -e` the SIGTERM
      # exit status (143) would otherwise trip errexit and make the whole
      # install report failure despite the daemon having started fine.
      kill "$_tail_pid" 2>/dev/null || true
      wait "$_tail_pid" 2>/dev/null || true
      say "--- (live output continues in $LOG_PATH) ---"
    fi
  else
    say "Done — launching ForkMesh now. (Next time, just run:  forkmesh)"
  fi
  diag launch 1 "$LAUNCH_MODE"
else
  say "Done. Launch it with:  forkmesh"
  say "  No display detected — forkmesh opens an interactive CLI here."
  say "  Type 'help' once it starts; on a dedicated VM you can run it as root"
  say "  with:  forkmesh --allow-root"
  diag launch 1 "manual"
fi

# Whole install finished successfully; the EXIT trap only fires on failure.
CURRENT_STEP="done"
diag done 1
