#!/usr/bin/env bash
# Run a Gradle command on a host whose IPv4 loopback is broken.
#
# Every Gradle task needs a TCP connection to 127.0.0.1: the Gradle client
# talks to its daemon over a loopback socket (`--no-daemon` only forks a
# single-use daemon and still connects the same way). On a host that drops
# 127.0.0.1 traffic, it fails in a way that looks like the tree is broken:
#
#   FAILURE: Build failed with an exception. Could not connect to the Gradle daemon.
#
# The message does not mention loopback, so this wrapper exists to keep the next
# reader from re-deriving it. A private network namespace gets its own working
# loopback; slirp4netns then gives that namespace NAT egress, which Gradle's
# dependency downloads still need.
#
# Usage:  tools/loopback-netns.sh <command> [args...]
#
# On a host with a healthy loopback this exec's the command unchanged, so it is
# safe to leave in a build script permanently.
set -uo pipefail

_fm_loopback_works() {
  # Bind a listener and dial it in the same process. A host that drops loopback
  # traffic times out here rather than refusing, so cap the wait.
  python3 - <<'PY' >/dev/null 2>&1
import socket, sys, threading
s = socket.socket()
s.bind(('127.0.0.1', 0))
s.listen(1)
threading.Thread(target=s.accept, daemon=True).start()
c = socket.socket()
c.settimeout(3)
try:
    c.connect(('127.0.0.1', s.getsockname()[1]))
except OSError:
    sys.exit(1)
PY
}

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <command> [args...]" >&2
  exit 2
fi

if _fm_loopback_works; then
  exec "$@"
fi

echo "IPv4 loopback is unreachable on this host; running in a private" \
     "network namespace." >&2

for tool in unshare slirp4netns; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "ERROR: IPv4 loopback is broken and '$tool' is unavailable, so" \
         "Gradle cannot run here." >&2
    echo "Fix the host's loopback (127.0.0.1 must accept TCP) or install" \
         "util-linux and slirp4netns." >&2
    exit 1
  fi
done

_fm_tmp="$(mktemp -d)"
trap 'rm -rf "$_fm_tmp"' EXIT
_fm_fifo="$_fm_tmp/go"
_fm_pidfile="$_fm_tmp/pid"
mkfifo "$_fm_fifo"

# `unshare -r` maps us to uid 0 inside the namespace, which makes the JVM and
# the Android SDK resolve ~ as /root. Pin the caller's HOME so Gradle keeps
# using its real, already-warmed caches instead of trying to build a new one
# under an unwritable /root.
_fm_home="${HOME:?HOME must be set}"

unshare -rn -- bash -c '
  echo $$ > "$1"
  ip link set lo up
  read -r _ < "$2"   # block until slirp4netns has configured tap0
  export HOME="$3"
  export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$3/.gradle}"
  shift 3
  exec "$@"
' bash "$_fm_pidfile" "$_fm_fifo" "$_fm_home" "$@" &
_fm_child=$!

for _ in $(seq 1 200); do
  [ -s "$_fm_pidfile" ] && break
  sleep 0.05
done
if [ ! -s "$_fm_pidfile" ]; then
  echo "ERROR: the namespaced child never reported its pid." >&2
  kill "$_fm_child" 2>/dev/null
  exit 1
fi

slirp4netns --configure --mtu=65520 --ready-fd=3 \
  "$(cat "$_fm_pidfile")" tap0 3>"$_fm_tmp/ready" >/dev/null 2>&1 &
_fm_slirp=$!
for _ in $(seq 1 200); do
  [ -s "$_fm_tmp/ready" ] && break
  sleep 0.05
done

# Release the child even if slirp4netns never came up: a hermetic test run does
# not need egress, and a real dependency download will fail with its own,
# clearer error.
echo go > "$_fm_fifo"

wait "$_fm_child"
_fm_rc=$?
kill "$_fm_slirp" 2>/dev/null
exit "$_fm_rc"
