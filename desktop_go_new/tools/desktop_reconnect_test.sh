#!/usr/bin/env bash
set -uo pipefail
APP=${1:?usage: desktop_reconnect_test.sh <forkmesh binary> <forkmesh-core binary> [sandbox dir]}
CORE=${2:?forkmesh-core binary}
SAND=${3:-$(mktemp -d "${TMPDIR:-/tmp}/fmrc.XXXXXX")}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
case "$SAND" in /|"$HOME"|"$HOME"/projects*) echo "refusing sandbox $SAND" >&2; exit 2 ;; esac
mkdir -p "$SAND/.config" "$SAND/.local/share" "$SAND/.cache" "$SAND/tmp"
STATE="$SAND/projects/forkmesh/state/core"
CORELOG="$SAND/projects/forkmesh/logs/core/forkmesh-core.log"
FAILED=0
ENVV=(env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$SAND" XDG_CONFIG_HOME="$SAND/.config"
      XDG_DATA_HOME="$SAND/.local/share" XDG_CACHE_HOME="$SAND/.cache" XDG_RUNTIME_DIR="$SAND/tmp"
      TMPDIR="$SAND/tmp" LANG=C.UTF-8 QT_QPA_PLATFORM=offscreen FORKMESH_CORE_BIN="$CORE")

pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILED=1; }
core_pid() { sed -n 's/.*"pid": *\([0-9]*\).*/\1/p' "$STATE/endpoint.json" 2>/dev/null | head -1; }
attached() { local n; n=$(grep -c ' attached (pid' "$CORELOG" 2>/dev/null); echo "${n:-0}"; }
wait_for() {
  local what=$1 seconds=$2; shift 2
  for _ in $(seq 1 $((seconds * 10))); do "$@" && return 0; sleep 0.1; done
  fail "timed out: $what"; return 1
}
start_app() { "${ENVV[@]}" "$APP" >> "$SAND/app-$1.log" 2>&1 & echo $!; }
attached_at_least() { [ "$(attached)" -ge "$1" ]; }
dead() { ! kill -0 "$1" 2>/dev/null; }
cli() { "${ENVV[@]}" "$CORE" "$@"; }

APP1=$(start_app 1)
wait_for "first desktop attaches" 60 attached_at_least 1 && pass "desktop launched the daemon and attached"
P=$(core_pid)

if [ -f "$SAND/projects/forkmesh/data/identity/ed25519.pem" ] && [ -d "$HERE/../examples/service-plugins/repo-pulse" ]; then
  PKG="$SAND/projects/forkmesh/data/marketplace/packages/repo-pulse"
  mkdir -p "$PKG" && cp -R "$HERE/../examples/service-plugins/repo-pulse/." "$PKG/"
  cli plugins sign "$PKG" > /dev/null && cli call core.plugins.rescan > /dev/null &&
    cli call core.plugins.enable '{"id":"repo-pulse"}' > /dev/null
  plugin_pid() { cli plugins list 2>/dev/null | python3 -c 'import json,sys;print(next((p.get("pid",0) for p in json.load(sys.stdin)["plugins"] if p["id"]=="repo-pulse"),0))'; }
  plugin_running() { [ "$(plugin_pid)" -gt 0 ]; }
  wait_for "service plug-in starts" 20 plugin_running && pass "service plug-in running (pid $(plugin_pid))"
  SVC=$(plugin_pid)
else
  SVC=0
  echo "SKIP service plug-in (no identity key or example)"
fi

kill -KILL "$APP1"; wait "$APP1" 2>/dev/null
sleep 2
if kill -0 "$P" 2>/dev/null; then pass "daemon $P survived a desktop crash"; else fail "daemon died with the desktop"; fi

APP2=$(start_app 2)
wait_for "second desktop attaches" 60 attached_at_least 2 && {
  if [ "$(core_pid)" = "$P" ]; then pass "new desktop re-attached to the same daemon $P"; else fail "daemon was replaced ($(core_pid) != $P)"; fi
}
if [ "$SVC" -gt 0 ]; then
  if [ "$(plugin_pid)" = "$SVC" ]; then pass "service plug-in $SVC kept running across the UI restart"; else fail "service plug-in restarted ($(plugin_pid) != $SVC)"; fi
fi

kill -KILL "$P"
wait_for "desktop relaunches the daemon" 60 bash -c "p=\$(sed -n 's/.*\"pid\": *\([0-9]*\).*/\1/p' '$STATE/endpoint.json' 2>/dev/null | head -1); [ -n \"\$p\" ] && [ \"\$p\" != '$P' ] && kill -0 \"\$p\" 2>/dev/null" && {
  Q=$(core_pid)
  pass "desktop relaunched the daemon after it was killed ($P -> $Q)"
  wait_for "relaunched daemon attaches" 30 attached_at_least 3 && pass "desktop re-attached to the relaunched daemon"
  if cli status | grep -q '"kind": "crash"'; then pass "daemon reports recovery from the unclean stop"; else fail "lastExit not crash"; fi
}

cli background on > /dev/null
kill -TERM "$APP2"; wait "$APP2" 2>/dev/null
Q=$(core_pid)
sleep 3
if [ -n "$Q" ] && kill -0 "$Q" 2>/dev/null; then pass "daemon keeps running after the window quits (background mode)"; else fail "daemon stopped with the window"; fi
cli stop > /dev/null
wait_for "daemon stops on request" 20 dead "$Q" && pass "daemon stopped cleanly on request"
[ "$SVC" -gt 0 ] && { pgrep -f "[r]epo_pulse.py" > /dev/null && fail "service plug-in outlived the daemon" || pass "service plug-in stopped with the daemon"; }

echo "core log: $CORELOG"
exit $FAILED
