#!/usr/bin/env bash
set -u

limit="$1"
shift

descendants() {
  local child
  for child in $(pgrep -P "$1"); do
    descendants "$child"
    echo "$child"
  done
}

"$@" &
command=$!
(
  trap 'kill "$sleeper" 2>/dev/null; exit 0' TERM
  sleep "$limit" &
  sleeper=$!
  wait "$sleeper"
  trap '' TERM
  echo "ERROR: $1 ran past $limit seconds; stopping it and every process it started." >&2
  pids="$(descendants "$command") $command"
  kill $pids 2>/dev/null || true
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 $pids 2>/dev/null || break
    sleep 1
  done
  kill -9 $pids 2>/dev/null || true
) &
watchdog=$!
status=0
wait "$command" || status=$?
kill "$watchdog" 2>/dev/null || true
wait "$watchdog" 2>/dev/null || true
exit "$status"
