#!/usr/bin/env bash
# Build desktop/ against the exact Qt 6.4.2 and GCC that Ubuntu 24.04 LTS and
# Debian 12 ship, without root, Docker, or a second machine.
#
# Both distributions ship Qt 6.4.2, so find_package(Qt6 6.4) configures happily
# and the build then fails partway through on any API that landed in 6.5 or
# later. A developer box running Qt 6.8 cannot see those failures: the code
# compiles there, and reading the sources only catches the symbols someone
# thought to look for. This script closes that gap by fetching each
# distribution's own packages, unpacking them into a private sysroot, and
# running the same CMake configure and build install.sh runs for a source
# install.
#
#   tools/qt64-build-check.sh                 # both profiles
#   tools/qt64-build-check.sh ubuntu-24.04    # one profile
#   tools/qt64-build-check.sh debian-12
#
# Downloads (~250 MB per profile) and the unpacked sysroots are cached under
# $HOME/.forkmesh/toolchain/qt64, so re-runs only pay for the build.
#
# The sysroot is a build target, not a runnable desktop: it holds the Qt and
# GCC pieces a compile and link need and borrows the rest from the host, and it
# is amd64-only. The binary it produces is evidence the sources build on that
# distribution; run the real client from a normal host build.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=tools/forkmesh-paths.sh
. "$REPO_ROOT/tools/forkmesh-paths.sh"
# shellcheck source=tools/forkmesh-safe-rm.sh
. "$REPO_ROOT/tools/forkmesh-safe-rm.sh"
TOOLCHAIN_DIR="${FORKMESH_QT64_DIR:-$(forkmesh_toolchain_dir)/qt64}"

# Qt 6.4.2 is what both distributions ship. Anything else means the archive
# moved on and a run would no longer answer the question this check exists for.
EXPECTED_QT_VERSION="6.4.2"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# Preserve live working memory and never start more than four heavy compilers,
# matching run.sh. Ninja also gates those behind CMakeLists.txt's job pool, but
# the Makefile generator ignores pools.
build_jobs() {
  local cores=2 total_kb=0 available_kb=0 reserve_kb usable_kb memory_jobs
  if command -v nproc >/dev/null 2>&1; then
    cores="$(nproc)"
  fi
  [ "$cores" -gt 4 ] && cores=4
  if [ -r /proc/meminfo ]; then
    total_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
    available_kb="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)"
  fi
  if [ "${total_kb:-0}" -gt 0 ]; then
    memory_jobs=$((total_kb / (4 * 1024 * 1024)))
    [ "$memory_jobs" -lt 1 ] && memory_jobs=1
    [ "$memory_jobs" -lt "$cores" ] && cores="$memory_jobs"
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
    [ "$memory_jobs" -lt "$cores" ] && cores="$memory_jobs"
  fi
  printf '%s\n' "$cores"
}

# Ubuntu renamed the Qt runtime packages during the 64-bit time_t transition
# ("t64"), and the two distributions pin different ICU and GCC majors, so each
# profile carries a full package list rather than sharing one with holes in it.
profile_suite()      { case "$1" in ubuntu-24.04) echo noble ;; debian-12) echo bookworm ;; esac; }
profile_mirror()     { case "$1" in ubuntu-24.04) echo https://archive.ubuntu.com/ubuntu ;; debian-12) echo https://deb.debian.org/debian ;; esac; }
profile_components() { case "$1" in ubuntu-24.04) echo "main universe" ;; debian-12) echo "main" ;; esac; }
profile_gcc()        { case "$1" in ubuntu-24.04) echo 13 ;; debian-12) echo 12 ;; esac; }

profile_packages() {
  local qt_runtime gcc_extra gcc
  case "$1" in
    ubuntu-24.04)
      qt_runtime="libqt6core6t64 libqt6gui6t64 libqt6widgets6t64 libqt6network6t64
                  libqt6concurrent6t64 libqt6dbus6t64 libqt6opengl6t64
                  libqt6openglwidgets6t64 libqt6printsupport6t64 libqt6sql6t64
                  libqt6test6t64 libqt6xml6t64 libicu74 libssl3t64"
      # Ubuntu splits the compiler backends into per-architecture packages; the
      # unsuffixed ones are thin wrappers with no cc1plus in them.
      gcc_extra="cpp-13-x86-64-linux-gnu gcc-13-x86-64-linux-gnu g++-13-x86-64-linux-gnu"
      ;;
    debian-12)
      qt_runtime="libqt6core6 libqt6gui6 libqt6widgets6 libqt6network6
                  libqt6concurrent6 libqt6dbus6 libqt6opengl6 libqt6openglwidgets6
                  libqt6printsupport6 libqt6sql6 libqt6test6 libqt6xml6 libicu72 libssl3"
      gcc_extra=""
      ;;
  esac
  gcc="$(profile_gcc "$1")"
  echo "qt6-base-dev qt6-base-dev-tools qt6-base-private-dev qmake6
        qt6-svg-dev libqt6svg6 libqt6svgwidgets6 libssl-dev
        qt6-multimedia-dev libqt6multimedia6 libqt6multimediawidgets6
        $qt_runtime
        gcc-$gcc-base cpp-$gcc gcc-$gcc g++-$gcc libgcc-$gcc-dev libstdc++-$gcc-dev
        $gcc_extra"
}

require_host() {
  [ "$(uname -s)" = "Linux" ] || die "this check unpacks amd64 Linux packages; run it on Linux"
  [ "$(uname -m)" = "x86_64" ] || die "this check is amd64-only (host is $(uname -m))"
  local missing=""
  for tool in curl xz cmake awk; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
  done
  command -v dpkg-deb >/dev/null 2>&1 || command -v ar >/dev/null 2>&1 ||
    missing="$missing dpkg-deb-or-ar"
  [ -z "$missing" ] || die "missing required tools:$missing"
}

# Resolve package names through the suite's own index rather than hardcoding
# pool URLs, so a security rebuild that bumps a package revision does not stale
# this script out.
resolve_urls() {
  local profile="$1" out="$2" suite mirror index indexes="" wanted
  suite="$(profile_suite "$profile")"
  mirror="$(profile_mirror "$profile")"

  # shellcheck disable=SC2086  # the component list is deliberately word-split
  for component in $(profile_components "$profile"); do
    index="$TOOLCHAIN_DIR/index/$suite-$component.txt"
    if [ ! -s "$index" ]; then
      mkdir -p "$(dirname "$index")"
      say "$profile: fetching the $suite/$component package index"
      curl -fsSL "$mirror/dists/$suite/$component/binary-amd64/Packages.xz" |
        xz -dc > "$index.part" || { rm -f "$index.part"; die "$profile: could not fetch the $suite/$component index"; }
      mv "$index.part" "$index"
    fi
    indexes="$indexes $index"
  done

  wanted="$(profile_packages "$profile" | tr -s ' \n' ' ')"
  # shellcheck disable=SC2086  # $indexes is a deliberate list of file arguments
  awk -v mirror="$mirror" -v wanted="$wanted" '
    BEGIN { split(wanted, list, " "); for (i in list) if (list[i] != "") want[list[i]] = 1 }
    /^Package: / { pkg = $2 }
    /^Filename: / { if ((pkg in want) && !(pkg in seen)) { seen[pkg] = 1; print pkg, mirror "/" $2 } }
    END { for (p in want) if (!(p in seen)) print "missing:" p > "/dev/stderr" }
  ' $indexes > "$out" 2> "$out.missing"

  if [ -s "$out.missing" ]; then
    die "$profile: $suite no longer publishes $(tr '\n' ' ' < "$out.missing")"
  fi
  rm -f "$out.missing"
}

unpack_deb() {
  local deb="$1" root="$2" scratch
  if command -v dpkg-deb >/dev/null 2>&1; then
    dpkg-deb -x "$deb" "$root"
    return
  fi
  # Hosts without dpkg still have binutils sitting next to the compiler.
  scratch="$(mktemp -d)"
  ( cd "$scratch" && ar x "$deb" ) || { fm_safe_rm_tree "${TMPDIR:-/tmp}" "$scratch"; return 1; }
  tar -xf "$scratch"/data.tar.* -C "$root"
  fm_safe_rm_tree "${TMPDIR:-/tmp}" "$scratch"
}

build_sysroot() {
  local profile="$1" base root debs stamp urls file
  base="$TOOLCHAIN_DIR/$profile"
  root="$base/root"
  debs="$base/debs"
  stamp="$base/.unpacked"

  if [ -f "$stamp" ] && [ "$(cat "$stamp")" = "$(profile_packages "$profile" | tr -s ' \n' ' ')" ]; then
    say "$profile: reusing the sysroot in $root"
    return
  fi

  mkdir -p "$debs" "$root"
  urls="$base/urls.txt"
  resolve_urls "$profile" "$urls"

  say "$profile: downloading $(wc -l < "$urls") packages"
  while read -r _ url; do
    file="$debs/$(basename "$url")"
    if [ ! -s "$file" ]; then
      curl -fsSL -o "$file.part" "$url" || { rm -f "$file.part"; die "$profile: could not download $url"; }
      mv -f "$file.part" "$file"
    fi
  done < "$urls"

  say "$profile: unpacking into $root"
  for file in "$debs"/*.deb; do
    unpack_deb "$file" "$root" || die "$profile: could not unpack $file"
  done
  profile_packages "$profile" | tr -s ' \n' ' ' > "$stamp"
}

# The distributions install Qt under a multiarch prefix, so the CMake package
# and the headers both hang off <root>/usr rather than a plain /usr/lib.
qt_prefix() { printf '%s\n' "$TOOLCHAIN_DIR/$1/root/usr"; }

verify_qt_version() {
  local profile="$1" header version
  # QT_VERSION_STR is what the compiler sees, and qconfig.h is where the
  # distribution's own build of it lands.
  header="$(qt_prefix "$profile")/include/x86_64-linux-gnu/qt6/QtCore/qconfig.h"
  [ -f "$header" ] || die "$profile: no Qt headers under $(qt_prefix "$profile")"
  version="$(sed -n 's/^#[[:space:]]*define QT_VERSION_STR[[:space:]]*"\(.*\)".*/\1/p' "$header" | head -1)"
  [ "$version" = "$EXPECTED_QT_VERSION" ] ||
    die "$profile: expected Qt $EXPECTED_QT_VERSION, the sysroot has ${version:-an unreadable version}"
  say "$profile: Qt $version"
}

run_profile() {
  local profile="$1" prefix build cxx jobs reported
  build_sysroot "$profile"
  verify_qt_version "$profile"

  prefix="$(qt_prefix "$profile")"
  cxx="$prefix/bin/g++-$(profile_gcc "$profile")"
  [ -x "$cxx" ] || die "$profile: the sysroot has no $(basename "$cxx")"
  say "$profile: $("$cxx" --version | head -1)"

  build="$TOOLCHAIN_DIR/$profile/build"
  fm_safe_rm_tree "$TOOLCHAIN_DIR" "$build"

  # Prefer Ninja: it honours the forkmesh_heavy job pool from CMakeLists.txt,
  # which keeps the big translation units from running on top of each other.
  local generator=()
  if command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
  fi
  # The Qt libraries name the distribution's own ICU in their NEEDED entries and
  # the host's ICU carries a different soname, so -rpath-link points the linker
  # at the sysroot copy for that transitive resolution. Every other argument is
  # what install.sh passes for a source install, so a clean run here is the
  # build a user on that distribution actually gets.
  cmake -S "$REPO_ROOT/desktop" -B "$build" "${generator[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFORKMESH_BUILD_TESTS=OFF \
    -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -DOPENSSL_ROOT_DIR="$prefix" \
    -DOPENSSL_INCLUDE_DIR="$prefix/include" \
    -DOPENSSL_CRYPTO_LIBRARY="$prefix/lib/x86_64-linux-gnu/libcrypto.so" \
    -DOPENSSL_SSL_LIBRARY="$prefix/lib/x86_64-linux-gnu/libssl.so" \
    -DCMAKE_CXX_FLAGS="-isystem $prefix/include/x86_64-linux-gnu" \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,-rpath-link,$prefix/lib/x86_64-linux-gnu" ||
    die "$profile: CONFIGURE FAILED"

  jobs="${FORKMESH_QT64_JOBS:-$(build_jobs)}"
  say "$profile: building with $jobs jobs (this takes a while)"
  cmake --build "$build" -j"$jobs" || die "$profile: BUILD FAILED"

  [ -x "$build/forkmesh" ] || die "$profile: the build reported success but produced no binary"
  reported="$(LD_LIBRARY_PATH="$prefix/lib/x86_64-linux-gnu" QT_QPA_PLATFORM=offscreen \
    "$build/forkmesh" --version 2>&1 | head -1)" || true
  say "$profile: OK — $build/forkmesh (${reported:-no --version output})"
}

require_host

profiles=("$@")
if [ "${#profiles[@]}" -eq 0 ]; then
  profiles=(ubuntu-24.04 debian-12)
fi
for profile in "${profiles[@]}"; do
  [ -n "$(profile_suite "$profile")" ] ||
    die "unknown profile '$profile' (expected ubuntu-24.04 or debian-12)"
done

for profile in "${profiles[@]}"; do
  say "profile $profile"
  run_profile "$profile"
done

say "every requested profile built cleanly against Qt $EXPECTED_QT_VERSION"
