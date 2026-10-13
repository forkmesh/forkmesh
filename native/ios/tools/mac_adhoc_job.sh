#!/usr/bin/env bash
set -euo pipefail

run="$1"
through="${2:-}"
tree="${3:-ci/tree}"
tools="$(cd "$(dirname "$0")" && pwd)"
key=all
if [ -n "$through" ]; then
  key="$(cut -f2 "$run/steps/index.tsv" | grep -Fxn -- "$through" | head -n 1 | cut -d: -f1 || true)"
  if [ -z "$key" ]; then
    echo "ERROR: no step named \"$through\" in $run/steps/index.tsv." >&2
    exit 1
  fi
fi
mkdir -p "$run/jobs"
job="$run/jobs/$key"

if mkdir "$job" 2>/dev/null; then
  : > "$job/log"
  nohup perl -MPOSIX -e 'POSIX::setsid(); exec @ARGV or die "exec: $!\n"' \
    bash -c 'bash "$1" "$2/steps" "$3" "$2/out" "$4" > "$5/log" 2>&1 < /dev/null
             echo $? > "$5/status.new"
             mv "$5/status.new" "$5/status"' \
    job "$tools/mac_adhoc_build.sh" "$run" "$tree" "$through" "$job" > /dev/null 2>&1 < /dev/null &
  pid=$!
  ps -o lstart= -p "$pid" > "$job/started" 2>/dev/null || true
  echo "$pid" > "$job/pid"
  if [ -d ci/lock ]; then
    echo "$pid" > ci/lock/pid
  fi
else
  waited=0
  until [ -s "$job/pid" ] || [ -s "$job/status" ]; do
    if [ "$waited" -ge 30 ]; then
      echo "ERROR: $job was started but never recorded its process." >&2
      exit 1
    fi
    sleep 1
    waited=$((waited + 1))
  done
  echo "Reconnected to the Mac's build job $job; its output so far:"
fi

pid="$(cat "$job/pid" 2>/dev/null || true)"
started="$(cat "$job/started" 2>/dev/null || true)"
alive() {
  local now
  [ -n "$pid" ] || return 1
  now="$(ps -o lstart= -p "$pid" 2>/dev/null || true)"
  [ -n "$now" ] && { [ -z "$started" ] || [ "$now" = "$started" ]; }
}

offset=0
while :; do
  result=""
  if [ -s "$job/status" ]; then
    result="$(cat "$job/status")"
  elif ! alive; then
    sleep 1
    if [ -s "$job/status" ]; then
      result="$(cat "$job/status")"
    else
      result=lost
    fi
  fi
  size="$(wc -c < "$job/log" | tr -d ' ')"
  if [ "$size" -gt "$offset" ]; then
    { tail -c "+$((offset + 1))" "$job/log" | head -c "$((size - offset))"; } || true
    offset="$size"
  fi
  case "$result" in
    "") sleep 1 ;;
    lost)
      echo "ERROR: the build job $job stopped without an exit status (was the Mac restarted?)." >&2
      exit 1
      ;;
    *) exit "$result" ;;
  esac
done
