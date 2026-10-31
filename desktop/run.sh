#!/usr/bin/env bash
# Build and run ForkMesh locally.
#
#   ./run.sh          build (incremental) and launch the app
#   ./run.sh launch   launch the last build without building
#   ./run.sh clean    clear the build cache (removes build/)
#   ./run.sh rebuild  clear the cache, then build and launch
#   ./run.sh test     build and run the sub-minute critical contracts
#   ./run.sh --quiet  any of the above without the step and timing log
set -euo pipefail
FM_RUN_QUIET="${FORKMESH_RUN_QUIET:-0}"
fm_run_args=()
for fm_arg in "$@"; do
    case "$fm_arg" in
        -q|--quiet) FM_RUN_QUIET=1 ;;
        *) fm_run_args+=("$fm_arg") ;;
    esac
done
set -- "${fm_run_args[@]+"${fm_run_args[@]}"}"
# Preserve the caller's directory before this build helper changes into
# desktop. The KVM workspace setting uses this exact directory as its only
# writable project mount, and self-relaunches inherit it unchanged.
FORKMESH_LAUNCH_DIR="${FORKMESH_START_DIRECTORY:-$PWD}"
export FORKMESH_START_DIRECTORY="$FORKMESH_LAUNCH_DIR"
cd "$(dirname "$0")"
# shellcheck source=tools/forkmesh-safe-rm.sh
. ../tools/forkmesh-safe-rm.sh

# The giant MainWindow translation units peak around 1.6 GiB each. Reserve RAM
# for the running desktop/browser/deploy tools, size from currently available
# memory at 2 GiB per job, and keep a hard ceiling of twelve (the same figures
# as ramCappedBuildJobs() in MainWindowInternal.h). Ninja also has a matching
# job pool; this function is the guard for Make, which ignores pools.
build_jobs() {
    local cores=2 total_kb=0 available_kb=0 reserve_kb usable_kb memory_jobs
    if command -v nproc >/dev/null 2>&1; then
        cores="$(nproc)"
    elif command -v sysctl >/dev/null 2>&1; then
        cores="$(sysctl -n hw.logicalcpu)"
    fi
    [ "$cores" -gt 12 ] && cores=12
    if [ -r /proc/meminfo ]; then
        total_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
        available_kb="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)"
    elif command -v sysctl >/dev/null 2>&1; then
        total_kb="$(($(sysctl -n hw.memsize 2>/dev/null || printf '0') / 1024))"
    fi
    reserve_kb=$((6 * 1024 * 1024))
    if [ "${total_kb:-0}" -gt 0 ] && [ "$reserve_kb" -gt $((total_kb / 2)) ]; then
        reserve_kb=$((total_kb / 2))
    fi
    if [ "${total_kb:-0}" -gt 0 ]; then
        memory_jobs=$(((total_kb - reserve_kb) / (2 * 1024 * 1024)))
        [ "$memory_jobs" -lt 1 ] && memory_jobs=1
        [ "$memory_jobs" -lt "$cores" ] && cores="$memory_jobs"
    fi
    if [ "${available_kb:-0}" -gt 0 ]; then
        usable_kb=$((available_kb - reserve_kb))
        [ "$usable_kb" -lt 0 ] && usable_kb=0
        memory_jobs=$((usable_kb / (2 * 1024 * 1024)))
        [ "$memory_jobs" -lt 1 ] && memory_jobs=1
        [ "$memory_jobs" -lt "$cores" ] && cores="$memory_jobs"
    fi
    printf '%s\n' "$cores"
}

# Ninja when installed: it honours the heavy-compile job pool and starts the
# giant units first. Shared with the desktop's own Rebuild & restart, which
# configures the same build/ tree the same way.
cmake_generator() {
    if command -v ninja >/dev/null 2>&1; then
        printf '%s\n' Ninja
    fi
}

cmake_args() {
    printf '%s\0' -DCMAKE_BUILD_TYPE=Release
    # FORKMESH_BUILD_TESTS defaults to ON, and a cached ON (left by
    # `./run.sh test`) made every later run compile all the test programs on
    # top of the app. Running the app builds the app; `test` turns them on.
    # The desktop's own Rebuild & restart configures OFF the same way.
    printf '%s\0' -DFORKMESH_BUILD_TESTS=OFF
    local generator
    generator="$(cmake_generator)"
    [ -n "$generator" ] && printf '%s\0' -G "$generator"
    if [ "$(uname -s)" = "Darwin" ] && command -v brew >/dev/null 2>&1; then
        local qt_prefix openssl_prefix
        qt_prefix="$(brew --prefix qt 2>/dev/null || true)"
        openssl_prefix="$(brew --prefix openssl@3 2>/dev/null || true)"
        [ -n "$qt_prefix" ] && printf '%s\0' "-DCMAKE_PREFIX_PATH=$qt_prefix"
        [ -n "$openssl_prefix" ] && printf '%s\0' "-DOPENSSL_ROOT_DIR=$openssl_prefix"
    fi
}

# A CMakeCache.txt records the absolute source and binary dirs it was generated
# in. When a checkout is copied/merged from another machine (or moved), those
# paths no longer match and cmake aborts. Detect that and wipe the cache.
check_stale_cache() {
    local cache="build/CMakeCache.txt"
    [ -f "$cache" ] || return 0

    local cached_src cached_bin
    cached_src="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$cache")"
    cached_bin="$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "$cache")"

    if { [ -n "$cached_src" ] && [ "$cached_src" != "$PWD" ]; } ||
       { [ -n "$cached_bin" ] && [ "$cached_bin" != "$PWD/build" ]; }; then
        echo "Stale build cache (generated in ${cached_src:-$cached_bin}); cleaning." >&2
        fm_wipe_build
        return 0
    fi

    # CMake will not switch an existing tree to another generator. Drop only
    # the generator's own entries: the objects stay warm and the built binary
    # stays in place until the next link replaces it.
    local wanted cached_generator
    wanted="$(cmake_generator)"
    cached_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache")"
    if [ -n "$wanted" ] && [ -n "$cached_generator" ] && [ "$cached_generator" != "$wanted" ]; then
        echo "Build tree was generated for $cached_generator; switching to $wanted." >&2
        fm_safe_rm_tree "$PWD/build" build/CMakeCache.txt build/CMakeFiles build/Makefile \
               build/cmake_install.cmake build/build.ninja build/.ninja_deps \
               build/.ninja_log
    fi
}

fm_now_ms() {
    local now
    if [ -n "${EPOCHREALTIME:-}" ]; then
        now="${EPOCHREALTIME/[.,]/}"
        printf '%s\n' "$((10#$now / 1000))"
        return
    fi
    now="$(date +%s%N 2>/dev/null || true)"
    case "$now" in
        ''|*[!0-9]*) printf '%s\n' "$(($(date +%s) * 1000))" ;;
        *) printf '%s\n' "$((now / 1000000))" ;;
    esac
}

FM_RESTART_CLOCK="$(fm_now_ms)"
FM_STEP_TIMES=()
FM_STEP_LOG=""
FM_BUILD_LOCK="$PWD/build/.run-sh.lock"
FM_BUILD_LOCK_HELD=0

fm_release_build_lock() {
    [ "$FM_BUILD_LOCK_HELD" = 1 ] || return 0
    FM_BUILD_LOCK_HELD=0
    rm -f "$FM_BUILD_LOCK/pid"
    rmdir "$FM_BUILD_LOCK" 2>/dev/null || true
}

fm_take_build_lock() {
    local holder announced=0 unknown=0
    [ "$FM_BUILD_LOCK_HELD" = 1 ] && return 0
    mkdir -p "$(dirname "$FM_BUILD_LOCK")"
    until mkdir "$FM_BUILD_LOCK" 2>/dev/null; do
        holder="$(cat "$FM_BUILD_LOCK/pid" 2>/dev/null || true)"
        if [ -n "$holder" ]; then
            unknown=0
        else
            unknown=$((unknown + 1))
        fi
        if { [ -n "$holder" ] && ! kill -0 "$holder" 2>/dev/null; } || [ "$unknown" -gt 5 ]; then
            fm_log_restart "removing the stale build lock (pid ${holder:-unknown})"
            rm -f "$FM_BUILD_LOCK/pid"
            rmdir "$FM_BUILD_LOCK" 2>/dev/null || true
            continue
        fi
        if [ "$announced" = 0 ]; then
            fm_log_restart "waiting for the build already running in $PWD/build (pid ${holder:-unknown})"
            printf '==> Waiting for the build already running in %s (pid %s)...\n' \
                "$PWD/build" "${holder:-unknown}" >&2
            announced=1
        fi
        sleep 1
    done
    printf '%s\n' "$$" > "$FM_BUILD_LOCK/pid"
    FM_BUILD_LOCK_HELD=1
}

trap fm_release_build_lock EXIT

fm_wipe_build() {
    local held="$FM_BUILD_LOCK_HELD"
    FM_BUILD_LOCK_HELD=0
    fm_safe_rm_tree "$PWD" build
    if [ "$held" = 1 ] && [ "${1:-}" != release ]; then
        fm_take_build_lock
    fi
}

fm_configure() {
    local stamp="build/run-sh-configure.args" wanted
    wanted="$(printf '%s\n' "$@" "ccache=$(command -v ccache || true)")"
    if [ -f build/CMakeCache.txt ] && { [ -f build/build.ninja ] || [ -f build/Makefile ]; } &&
       [ -f "$stamp" ] && ! [ build/CMakeCache.txt -nt "$stamp" ] &&
       [ "$(cat "$stamp")" = "$wanted" ]; then
        fm_log_restart "build tree already configured; skipping the configure step"
        return 0
    fi
    fm_status "Configuring..."
    fm_step configure cmake -B build "$@"
    printf '%s\n' "$wanted" > "$stamp"
}

fm_log_restart() {
    [ "$FM_RUN_QUIET" = 1 ] && return 0
    printf '[restart +%5dms] %s\n' "$(($(fm_now_ms) - FM_RESTART_CLOCK))" "$1" >&2
}

fm_status() {
    if [ "${2:-}" = error ]; then
        fm_log_restart "ERROR: $1"
        printf '!! %s\n' "$1" >&2
        return 0
    fi
    fm_log_restart "$1"
    [ "$FM_RUN_QUIET" = 1 ] || printf '==> %s\n' "$1"
}

fm_failure_summary() {
    local log="$1" line
    if grep -q 'No space left on device' "$log"; then
        printf '%s\n' "the disk filled up during the build. Free space on the volume holding the build directory, then build again."
        return 0
    fi
    line="$(grep -m1 -E 'error:|^[[:space:]]*fatal:' "$log" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' | cut -c1-300 || true)"
    if [ -n "$line" ]; then
        printf '%s\n' "$line"
        return 0
    fi
    tail -c 300 "$log" |
        awk 'NR == 1 { first = $0; next } { rest = rest $0 "\n" } END { printf "%s", (rest == "" ? first "\n" : rest) }' |
        sed '/^[[:space:]]*$/d'
}

fm_duration_text() {
    local ms="$1"
    if [ "$ms" -lt 1000 ]; then
        printf '%sms\n' "$ms"
    elif [ "$ms" -lt 60000 ]; then
        printf '%s.%ss\n' "$((ms / 1000))" "$((ms % 1000 / 100))"
    else
        printf '%sm %ss\n' "$((ms / 60000))" "$((ms % 60000 / 1000))"
    fi
}

fm_summary() {
    [ "$FM_RUN_QUIET" = 1 ] && return 0
    local total parts="" entry
    total="$(($(fm_now_ms) - FM_RESTART_CLOCK))"
    for entry in "${FM_STEP_TIMES[@]+"${FM_STEP_TIMES[@]}"}"; do
        parts="${parts:+$parts, }${entry% *} $(fm_duration_text "${entry##* }")"
    done
    fm_log_restart "summary: ${parts:-no steps}; total ${total}ms"
    printf '==> %s in %s (%s)\n' "$1" "$(fm_duration_text "$total")" "${parts:-no steps}"
}

fm_step() {
    local label="$1"
    shift
    if [ "$FM_RUN_QUIET" = 1 ]; then
        "$@"
        return
    fi
    local command_line="$*" started took status
    [ -n "$FM_STEP_LOG" ] || FM_STEP_LOG="$PWD/build/run-sh-last-step.log"
    mkdir -p "$(dirname "$FM_STEP_LOG")"
    fm_log_restart "run: $command_line"
    printf '\n$ %s\n  (in %s)\n' "$command_line" "$PWD"
    started="$(fm_now_ms)"
    set +e
    "$@" 2>&1 | tee "$FM_STEP_LOG"
    status="${PIPESTATUS[0]}"
    set -e
    took="$(($(fm_now_ms) - started))"
    FM_STEP_TIMES+=("$label $took")
    fm_log_restart "done in ${took}ms (exit $status): $command_line"
    printf '\xE2\x80\x94 finished in %sms (exit %s)\n' "$took" "$status"
    if [ "$status" -ne 0 ]; then
        fm_status "Build failed: $(fm_failure_summary "$FM_STEP_LOG")" error
        fm_summary "Failed"
        exit "$status"
    fi
}

fm_build_pass() {
    local head
    head="$(git rev-parse --short HEAD 2>/dev/null || printf 'unknown')"
    fm_log_restart "build pass 1: $1 parallel jobs, HEAD $head"
}

fm_launch() {
    local bin
    bin="$(forkmesh_bin)"
    fm_summary "Built"
    fm_status "Launching..."
    fm_log_restart "launching $bin"
    fm_release_build_lock
    exec "$bin"
}

build() {
    fm_take_build_lock
    check_stale_cache
    local args=() jobs
    while IFS= read -r -d '' arg; do
        args+=("$arg")
    done < <(cmake_args)
    fm_configure "${args[@]}"
    jobs="$(build_jobs)"
    fm_status "Rebuilding..."
    fm_build_pass "$jobs"
    # An optional target ($1) builds just that (e.g. the tests) independently of
    # the app; with no argument the default target (the app) is built.
    if [ -n "${1:-}" ]; then
        fm_step build cmake --build build --parallel "$jobs" --target "$1"
    else
        fm_step build cmake --build build --parallel "$jobs"
    fi
}

forkmesh_bin() {
    if [ -x ./build/ForkMesh.app/Contents/MacOS/ForkMesh ]; then
        printf '%s\n' ./build/ForkMesh.app/Contents/MacOS/ForkMesh
    else
        printf '%s\n' ./build/forkmesh
    fi
}

case "${1:-run}" in
    clean)
        fm_take_build_lock
        fm_wipe_build release
        echo "Build cache cleared."
        ;;
    rebuild)
        fm_log_restart "run.sh rebuild: clean rebuild & launch started"
        fm_take_build_lock
        fm_wipe_build
        build
        fm_launch
        ;;
    launch)
        fm_log_restart "run.sh launch: launch without building started"
        fm_take_build_lock
        bin="$(forkmesh_bin)"
        if [ ! -x "$bin" ]; then
            fm_status "Nothing is built in $PWD/build yet; run ./run.sh to build and launch." error
            exit 1
        fi
        fm_status "Launching..."
        fm_log_restart "launching $bin"
        fm_release_build_lock
        exec "$bin"
        ;;
    test)
        fm_log_restart "run.sh test: build & run critical tests started"
        # Build and run only the critical trust, encrypted-mirror, and agent
        # transport contracts. Force the test option on so a cached
        # FORKMESH_BUILD_TESTS=OFF can't hide the targets.
        fm_take_build_lock
        check_stale_cache
        args=()
        while IFS= read -r -d '' arg; do
            args+=("$arg")
        done < <(cmake_args)
        # Later -D wins, so this overrides the OFF in cmake_args.
        fm_configure "${args[@]}" -DFORKMESH_BUILD_TESTS=ON
        # critical-tests tracks the "critical" CTest label in CMakeLists.txt;
        # naming the suites here let this list fall behind the label instead.
        jobs="$(build_jobs)"
        fm_status "Rebuilding..."
        fm_build_pass "$jobs"
        fm_step build cmake --build build --parallel "$jobs" --target critical-tests
        fm_summary "Built"
        fm_status "Running the critical tests..."
        fm_release_build_lock
        exec python3 ../tools/run_critical_tests.py qt --build-dir build
        ;;
    run)
        fm_log_restart "run.sh run: build & launch started"
        build
        fm_launch
        ;;
    *)
        echo "Usage: $0 [--quiet] [run|launch|clean|rebuild|test]" >&2
        exit 2
        ;;
esac
