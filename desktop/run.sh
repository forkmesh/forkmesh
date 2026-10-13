#!/usr/bin/env bash
# Build and run ForkMesh locally.
#
#   ./run.sh          build (incremental) and launch the app
#   ./run.sh clean    clear the build cache (removes build/)
#   ./run.sh rebuild  clear the cache, then build and launch
#   ./run.sh test     build and run the sub-minute critical contracts
set -euo pipefail
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
        fm_safe_rm_tree "$PWD" build
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

build() {
    check_stale_cache
    local args=()
    while IFS= read -r -d '' arg; do
        args+=("$arg")
    done < <(cmake_args)
    cmake -B build "${args[@]}"
    # An optional target ($1) builds just that (e.g. the tests) independently of
    # the app; with no argument the default target (the app) is built.
    if [ -n "${1:-}" ]; then
        cmake --build build --parallel "$(build_jobs)" --target "$1"
    else
        cmake --build build --parallel "$(build_jobs)"
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
        fm_safe_rm_tree "$PWD" build
        echo "Build cache cleared."
        ;;
    rebuild)
        fm_safe_rm_tree "$PWD" build
        build
        exec "$(forkmesh_bin)"
        ;;
    test)
        # Build and run only the critical trust, encrypted-mirror, and agent
        # transport contracts. Force the test option on so a cached
        # FORKMESH_BUILD_TESTS=OFF can't hide the targets.
        check_stale_cache
        args=()
        while IFS= read -r -d '' arg; do
            args+=("$arg")
        done < <(cmake_args)
        # Later -D wins, so this overrides the OFF in cmake_args.
        cmake -B build "${args[@]}" -DFORKMESH_BUILD_TESTS=ON
        # critical-tests tracks the "critical" CTest label in CMakeLists.txt;
        # naming the suites here let this list fall behind the label instead.
        cmake --build build --parallel "$(build_jobs)" --target critical-tests
        exec python3 ../tools/run_critical_tests.py qt --build-dir build
        ;;
    run)
        build
        exec "$(forkmesh_bin)"
        ;;
    *)
        echo "Usage: $0 [run|clean|rebuild|test]" >&2
        exit 2
        ;;
esac
