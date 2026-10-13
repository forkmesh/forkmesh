#!/usr/bin/env bash
# Capture a dated clean-machine transcript of the published ForkMesh desktop
# release on Linux x86_64 (issue #705): signed-manifest and blob-hash lines,
# install, first launch, node identity, provider CLI discovery, one continual
# cycle stopping at the human decision card, Stop work, and uninstall (default,
# then the --purge prompt text).
#
#   tools/clean_machine_transcript.sh verify   # release checks only; safe anywhere
#   tools/clean_machine_transcript.sh run      # full transcript; FRESH VM ONLY
#
# NEVER run `run` on a machine with a live ForkMesh node: uninstall.sh stops
# every process named `forkmesh` (pkill -x), and install.sh stops managed
# daemons. `run` refuses when a forkmesh process or ForkMesh data already
# exists. Steps that need a person at the GUI are recorded as operator
# checkpoints (UTC time plus the observation typed at the prompt); a checkpoint
# answered with an empty line is logged as NOT OBSERVED, never as a pass.
#
# Every banner carries the manifest checksums_sha256 so each step is bound to
# the exact release it exercised. The transcript is written to
# $FORKMESH_TRANSCRIPT (default ./forkmesh-clean-machine-<utc>.log).
#
# Unattended runs (tools/clean_machine_vm.py) set FORKMESH_TRANSCRIPT_AUTOMATED=1
# and FORKMESH_SCREENSHOT_HOOK=<command>. The hook is called as
# `<command> <name> terminal|app` at each step; checkpoints then log the
# screenshot name and stay NOT OBSERVED, because no person read the screen.
set -euo pipefail

FORKMESH_HOST="${FORKMESH_HOST:-https://forkmesh.com}"
# Version-agnostic by default: whatever the signed `latest` manifest publishes
# is the release under test. Set these to pin a particular one; an unpinned run
# still verifies the signature and the checksum binding, and stamps every step
# with the tag and checksum it actually verified.
EXPECTED_TAG="${FORKMESH_EXPECTED_TAG:-}"
EXPECTED_CHECKSUMS_SHA256="${FORKMESH_EXPECTED_CHECKSUMS_SHA256:-}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
TRANSCRIPT="${FORKMESH_TRANSCRIPT:-$PWD/forkmesh-clean-machine-$STAMP.log}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/forkmesh-transcript.XXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT

CHECKSUMS_SHA256="unverified"
RELEASE_TAG="unverified"
LINUX_ASSET=""
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"

utc() { date -u +%Y-%m-%dT%H:%M:%SZ; }
log() { printf '%s\n' "$*" | tee -a "$TRANSCRIPT"; }
die() { log "ABORT $(utc): $*"; exit 1; }
banner() { log ""; log "== $1 $(utc) release=$RELEASE_TAG checksums_sha256=$CHECKSUMS_SHA256 =="; }
# Run a command, keeping its combined output and exit status in the transcript.
record() {
  log "\$ $*"
  local rc
  set +e
  "$@" 2>&1 | tee -a "$TRANSCRIPT"
  rc="${PIPESTATUS[0]}"
  set -e
  log "exit=$rc"
  return 0
}

# Ask the screen observer (if any) to capture this step and check it against
# tools/clean_machine_expectations.json. The hook prints the observer's verdict:
#   @@VERDICT <step> observed|not-observed|unchecked <what it read, or missed>
# LAST_SCREEN_VERDICT keeps it for the checkpoint that follows.
LAST_SCREEN_VERDICT=""
shot() {
  LAST_SCREEN_VERDICT=""
  [ -n "${FORKMESH_SCREENSHOT_HOOK:-}" ] || return 0
  local reply
  reply="$("$FORKMESH_SCREENSHOT_HOOK" "$1" "${2:-terminal}" 2>/dev/null)" || true
  if [ -z "$reply" ]; then
    log "screenshot $(utc): $1 (${2:-terminal}); the observer did not answer"
    LAST_SCREEN_VERDICT="not-observed the observer did not answer"
    return 0
  fi
  LAST_SCREEN_VERDICT="${reply#@@VERDICT "$1" }"
  log "screen check $(utc): $1 — $LAST_SCREEN_VERDICT"
}

checkpoint() {
  banner "CHECKPOINT: $1"
  log "operator instruction: $2"
  if [ "${FORKMESH_TRANSCRIPT_AUTOMATED:-0}" = "1" ]; then
    shot "$3" "${4:-app}"
    case "$LAST_SCREEN_VERDICT" in
      observed*)
        log "observed $(utc): the screen observer ${LAST_SCREEN_VERDICT#observed }"
        ;;
      *)
        log "NOT OBSERVED $(utc): unattended run; the screen observer ${LAST_SCREEN_VERDICT:-was not running}"
        ;;
    esac
    return 0
  fi
  local note=""
  if [ -r /dev/tty ]; then
    printf '\n>>> %s\n>>> Type what you observed (exact on-screen text where possible), then Enter: ' "$2" > /dev/tty
    IFS= read -r note < /dev/tty || note=""
  fi
  if [ -n "$note" ]; then
    log "observed $(utc): $note"
  else
    log "NOT OBSERVED $(utc)"
  fi
}

release_verify() {
  banner "release manifest verification ($FORKMESH_HOST)"
  local repo="$WORK/release" dir key
  record git clone -q --depth 1 --filter=blob:none --no-checkout \
    "${FORKMESH_HOST%/}/forkmesh/forkmesh" "$repo"
  [ -d "$repo/.git" ] || die "could not clone the release repository"
  git -C "$repo" sparse-checkout set --no-cone /.forkmesh/releases/latest/ >/dev/null
  git -C "$repo" checkout -q
  dir="$repo/.forkmesh/releases/latest"
  record git -C "$repo" log -1 --format='release checkout commit %H %cI'

  # The trust anchor is the Ed25519 key the public installer itself embeds.
  record curl -fsSL -o "$WORK/install.sh" "${FORKMESH_HOST%/}/install.sh"
  record sha256sum "$WORK/install.sh"
  key="$(grep -Eo 'MCowBQYDK2VwAyEA[A-Za-z0-9+/]{43}=' "$WORK/install.sh" | head -1)"
  [ -n "$key" ] || die "no release public key in install.sh"
  printf -- '-----BEGIN PUBLIC KEY-----\n%s\n-----END PUBLIC KEY-----\n' "$key" > "$WORK/release-publisher.pem"
  log "release public key (from install.sh): $key"

  record openssl pkeyutl -verify -pubin -rawin -inkey "$WORK/release-publisher.pem" \
    -in "$dir/release.json" -sigfile "$dir/release.json.sig"
  openssl pkeyutl -verify -pubin -rawin -inkey "$WORK/release-publisher.pem" \
    -in "$dir/release.json" -sigfile "$dir/release.json.sig" >/dev/null 2>&1 ||
    die "release.json signature does not verify"

  local manifest_sums sums_hash tag
  manifest_sums="$(sed -n 's/.*"checksums_sha256"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' "$dir/release.json" | head -1)"
  sums_hash="$(sha256sum "$dir/SHASUMS256.txt" | awk '{print $1}')"
  tag="$(sed -n 's/.*"tag"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$dir/release.json" | head -1)"
  log "manifest tag:              $tag${EXPECTED_TAG:+ (pinned to $EXPECTED_TAG)}"
  log "manifest checksums_sha256: $manifest_sums"
  log "sha256(SHASUMS256.txt):    $sums_hash${EXPECTED_CHECKSUMS_SHA256:+ (pinned to $EXPECTED_CHECKSUMS_SHA256)}"
  [ -n "$tag" ] || die "the manifest names no tag"
  [ "$manifest_sums" = "$sums_hash" ] || die "SHASUMS256.txt is not bound to the manifest"
  if [ -n "$EXPECTED_CHECKSUMS_SHA256" ] && [ "$manifest_sums" != "$EXPECTED_CHECKSUMS_SHA256" ]; then
    die "latest publishes $manifest_sums, not the pinned $EXPECTED_CHECKSUMS_SHA256"
  fi
  if [ -n "$EXPECTED_TAG" ] && [ "$tag" != "$EXPECTED_TAG" ]; then
    die "latest tag is $tag, not the pinned $EXPECTED_TAG"
  fi
  [ -n "$EXPECTED_TAG$EXPECTED_CHECKSUMS_SHA256" ] ||
    log "no release pinned: this transcript covers whatever latest published, $tag"
  CHECKSUMS_SHA256="$manifest_sums"
  RELEASE_TAG="$tag"

  # Every Linux x86_64 asset the manifest carries, so the installed bytes can be
  # matched whichever one the installer chose in this release.
  MANIFEST_LINUX_ASSETS="$(python3 - "$dir/release.json" <<'PY'
import json, sys
for asset in json.load(open(sys.argv[1]))["assets"]:
    if asset.get("os") == "linux" and asset.get("arch") == "x86_64":
        print(asset["blob_sha256"], asset["size"], asset["name"])
PY
)"
  [ -n "$MANIFEST_LINUX_ASSETS" ] || die "$tag publishes no linux/x86_64 asset"
  log "manifest linux/x86_64 assets (blob_sha256 size name):"
  printf '%s\n' "$MANIFEST_LINUX_ASSETS" | while read -r line; do log "  $line"; done
  banner "release manifest verified"
  shot release-verified terminal
}

require_fresh_machine() {
  if pgrep -x forkmesh >/dev/null 2>&1 || pgrep -x forkmesh-mirror-node >/dev/null 2>&1; then
    die "a ForkMesh process is running; uninstall.sh would kill it. Use a fresh VM."
  fi
  local d
  for d in "$data_home/ForkMesh" "$config_home/ForkMesh" "$HOME/.forkmesh" "$HOME/.local/bin/forkmesh"; do
    [ ! -e "$d" ] || die "$d already exists; this is not a clean machine."
  done
  [ "$(uname -s)-$(uname -m)" = "Linux-x86_64" ] || die "Linux x86_64 only"
}

machine_header() {
  banner "machine"
  record uname -a
  record cat /etc/os-release
  record id -un
  log "DISPLAY=${DISPLAY:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-}"
  record sh -c 'command -v git openssl curl python3 script || true'
}

# Unattended first launch: the installer starts the client itself when a
# display is present; otherwise start it, then wait for its window to map.
automated_first_launch() {
  banner "first launch (unattended)"
  local waited=0 limit="${FORKMESH_FIRST_LAUNCH_WAIT:-900}"
  if ! pgrep -x forkmesh >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
    log "installer did not leave a running client; starting $HOME/.local/bin/forkmesh"
    setsid "$HOME/.local/bin/forkmesh" >/dev/null 2>&1 < /dev/null &
  fi
  while [ "$waited" -lt "$limit" ]; do
    if command -v xdotool >/dev/null 2>&1 &&
       xdotool search --onlyvisible --class forkmesh >/dev/null 2>&1; then
      break
    fi
    sleep 5; waited=$((waited + 5))
  done
  log "client window visible after ${waited}s (limit ${limit}s)"
  record sh -c 'pgrep -a -x forkmesh || echo "no forkmesh process"'
  if command -v xdotool >/dev/null 2>&1; then
    record sh -c 'for w in $(xdotool search --onlyvisible --class forkmesh 2>/dev/null); do xdotool getwindowname "$w"; done'
  fi
  # Let first-run setup (identity creation, mainnode connection) settle.
  sleep "${FORKMESH_FIRST_LAUNCH_SETTLE:-60}"
}

run_transcript() {
  require_fresh_machine
  : > "$TRANSCRIPT"
  log "ForkMesh clean-machine transcript (issue #705), started $(utc)"
  machine_header
  release_verify

  banner "provider CLIs present before install"
  record sh -c 'for c in claude codex gemini; do printf "%s: " "$c"; command -v "$c" || echo none; done'

  banner "install: curl -fsSL $FORKMESH_HOST/install.sh | bash"
  log "\$ curl -fsSL $FORKMESH_HOST/install.sh | bash"
  local rc
  set +e
  curl -fsSL "${FORKMESH_HOST%/}/install.sh" | bash 2>&1 | tee -a "$TRANSCRIPT"
  rc="${PIPESTATUS[1]}"
  set -e
  log "exit=$rc"
  [ "$rc" -eq 0 ] || die "installer failed"

  banner "installed binary hash"
  local bin="$HOME/.local/bin/forkmesh" got matched
  record ls -l "$bin"
  got="$(sha256sum "$bin" | awk '{print $1}')"
  log "sha256(installed forkmesh): $got"
  matched="$(printf '%s\n' "$MANIFEST_LINUX_ASSETS" | awk -v h="$got" '$1 == h {print $3}' | head -1)"
  if [ -n "$matched" ]; then
    log "installed binary matches manifest asset $matched of $RELEASE_TAG"
  else
    log "MISMATCH installed binary matches no linux/x86_64 asset of $RELEASE_TAG"
  fi
  shot installed terminal

  if [ "${FORKMESH_TRANSCRIPT_AUTOMATED:-0}" = "1" ]; then
    automated_first_launch
  fi
  checkpoint "first launch" \
    "If the installer did not open the desktop, run 'forkmesh' in a desktop session. Record the first window's title and any first-run prompt text." \
    first-launch
  banner "node identity files"
  record sh -c "find '$data_home/ForkMesh' '$config_home/ForkMesh' -maxdepth 3 -type f -printf '%TY-%Tm-%TdT%TH:%TM %s %p\n' 2>/dev/null | sort | head -60"
  record sh -c "grep -iE '^(node|nodeName|name|server)[^=]*=' '$config_home/ForkMesh/ForkMesh.conf' 2>/dev/null | head -20"
  shot node-identity terminal

  checkpoint "provider CLI discovery" \
    "Open the agent/provider picker. Record which providers the app lists as found (for example Claude Code, Codex) and the exact status text for any missing one." \
    provider-discovery
  checkpoint "continual cycle reaches the human decision card" \
    "Put one teammate in Continual mode on a scratch repository and let one cycle finish. Record the decision card text and confirm no second cycle started before you answered." \
    decision-card
  checkpoint "Stop work pauses the loop" \
    "Choose Stop work. Record the resulting status text and confirm no new cycle starts. Then quit the desktop app." \
    stop-work

  banner "uninstall (default: data kept)"
  record curl -fsSL -o "$WORK/uninstall.sh" "${FORKMESH_HOST%/}/uninstall.sh"
  record sha256sum "$WORK/uninstall.sh"
  record bash "$WORK/uninstall.sh"
  record sh -c "ls -d '$data_home/ForkMesh' '$config_home/ForkMesh' '$HOME/.forkmesh' '$bin' 2>&1 || true"
  shot uninstall-default terminal

  banner "uninstall --purge prompt (terminal attached, answered 'no')"
  if command -v script >/dev/null 2>&1; then
    log "\$ printf 'no\\n' | script -qec 'bash uninstall.sh --purge' /dev/null"
    printf 'no\n' | script -qec "bash '$WORK/uninstall.sh' --purge" /dev/null 2>&1 |
      tee -a "$TRANSCRIPT" || true
  else
    log "NOT OBSERVED: util-linux 'script' is missing, so no terminal prompt could be captured"
  fi
  banner "uninstall --purge without a terminal (curl | bash shape)"
  record bash -c "bash '$WORK/uninstall.sh' --purge < /dev/null"
  shot uninstall-purge-prompts terminal
  banner "uninstall --purge --yes (cleanup of the scratch identity)"
  record bash "$WORK/uninstall.sh" --purge --yes
  record sh -c "ls -d '$data_home/ForkMesh' '$config_home/ForkMesh' '$HOME/.forkmesh' 2>&1 || true"
  shot uninstall-purged terminal

  banner "transcript complete"
  log "transcript: $TRANSCRIPT"
}

case "${1:-}" in
  verify)
    : > "$TRANSCRIPT"
    log "ForkMesh release verification (issue #705), started $(utc)"
    release_verify
    log "transcript: $TRANSCRIPT"
    ;;
  run) run_transcript ;;
  *) sed -n '2,20p' "$0"; exit 2 ;;
esac
