#!/bin/sh
[ -n "$FORKMESH_COMPILE_GATE_OFF" ] && exec "$@"
[ -r /proc/meminfo ] || exec "$@"
command -v flock >/dev/null 2>&1 || exec "$@"

dir="${FORKMESH_COMPILE_GATE_DIR:-${XDG_RUNTIME_DIR:-/tmp}/forkmesh-compile-gate-$(id -u)}"
[ -d "$dir" ] || mkdir -p "$dir" 2>/dev/null || exec "$@"

slots="$FORKMESH_COMPILE_SLOTS"
if [ -z "$slots" ]; then
    total_kb=$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)
    slots=$(( (total_kb - 6 * 1024 * 1024) / (2 * 1024 * 1024) ))
    cores=$(nproc 2>/dev/null || echo 2)
    [ "$slots" -gt "$cores" ] && slots=$cores
    [ "$slots" -gt 12 ] && slots=12
fi
[ "$slots" -lt 1 ] && slots=1

min_free_kb=$(( ${FORKMESH_COMPILE_MIN_FREE_MB:-2048} * 1024 ))

memory_ok() {
    available_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
    [ "${available_kb:-0}" -ge "$min_free_kb" ]
}

while :; do
    i=0
    while [ "$i" -lt "$slots" ]; do
        exec 9>>"$dir/slot-$i"
        if flock -n 9; then
            if [ "$i" -eq 0 ] || memory_ok; then
                exec "$@"
            fi
            flock -u 9
        fi
        exec 9>&-
        i=$((i + 1))
    done
    sleep 0.2
done
