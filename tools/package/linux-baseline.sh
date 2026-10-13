#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
recipe="$here/linux-baseline"
tools_root="$(cd "$here/../.." && pwd)"
. "$tools_root/tools/forkmesh-paths.sh"
. "$recipe/image.env"

cache="${FORKMESH_LINUX_BASELINE_CACHE:-$(forkmesh_shared_dir toolchain-linux "$HOME/.cache/forkmesh-linux-baseline")}"
stages=(packages openssl qt nghttp3 appimage-tools)
stage_prefix() {
  case "$1" in
    openssl) echo /opt/forkmesh/openssl ;;
    qt) echo /opt/forkmesh/qt ;;
    nghttp3) echo /opt/forkmesh/deps ;;
    appimage-tools) echo /opt/forkmesh/appimage-tools ;;
  esac
}
smoke_images=(
  "docker.io/library/ubuntu:22.04"
  "docker.io/library/ubuntu:24.04"
  "docker.io/library/debian:12"
)
smoke_packages="ca-certificates libx11-6 libx11-xcb1 libxcb1 libxcb-glx0 libxcb-randr0 libxcb-render0 \
libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xinerama0 libxcb-xinput0 libxcb-xkb1 \
libxkbcommon0 libxkbcommon-x11-0 libfontconfig1 libfreetype6 libgl1 libegl1 libopengl0 libglx0 zlib1g"

log() { echo "[linux-baseline] $*" >&2; }
die() { log "$*"; exit 1; }

usage() {
  cat >&2 <<'EOF'
usage: tools/package/linux-baseline.sh <command> [options]

  engine                          print the container engine that would be used
  prepare                         build (or reuse) the cached Debian 12 builder image
  build --source DIR --out DIR --version X.Y.Z --commit SHA [--channel NAME]
                                  build forkmesh-linux-x86_64 and its AppImage (+ .zsync)
  check FILE...                   fail when a binary needs a newer glibc than the baseline
  smoke --out DIR --version X.Y.Z launch both assets in Ubuntu 22.04, Ubuntu 24.04 and Debian 12

Environment: FORKMESH_LINUX_BASELINE_ENGINE (podman|docker|bwrap),
FORKMESH_LINUX_BASELINE_CACHE, FORKMESH_BASELINE_JOBS.
EOF
  exit 2
}

pick_engine() {
  local wanted="${FORKMESH_LINUX_BASELINE_ENGINE:-}"
  if [ -n "$wanted" ]; then
    echo "$wanted"
    return
  fi
  if command -v podman >/dev/null 2>&1 && podman info >/dev/null 2>&1; then
    echo podman
  elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    echo docker
  elif command -v bwrap >/dev/null 2>&1 && bwrap --unshare-user --uid 0 --bind / / true 2>/dev/null; then
    echo bwrap
  else
    die "no usable container engine: install podman, or rootless docker (needs the uidmap package), or bubblewrap"
  fi
}

default_jobs() {
  local cpus mem_gb jobs per="${1:-1}"
  cpus="$(nproc 2>/dev/null || echo 4)"
  mem_gb="$(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo 2>/dev/null || echo 8)"
  jobs=$(( (mem_gb - 2) / per ))
  [ "$jobs" -gt "$cpus" ] && jobs="$cpus"
  [ "$jobs" -gt 16 ] && jobs=16
  [ "$jobs" -lt 1 ] && jobs=1
  echo "$jobs"
}

recipe_key() {
  local upto="${1:-}" hash
  hash="$(cat "$recipe/image.env" "$recipe/fetch.sh" | sha256sum | cut -c1-64)"
  for stage in "${stages[@]}"; do
    hash="$( { echo "$hash"; cat "$recipe/stage-$stage.sh"; } | sha256sum | cut -c1-64)"
    [ "$stage" = "$upto" ] && break
  done
  echo "${hash:0:16}"
}

image_tag() { echo "forkmesh-linux-baseline:$(recipe_key)"; }

inside_cache() {
  case "$(realpath -m "$1")" in
    "$(realpath -m "$cache")"/*) return 0 ;;
  esac
  return 1
}

chown_shim() {
  local shim="$cache/shim"
  mkdir -p "$shim"
  for tool in chown chgrp; do
    printf '#!/bin/sh\n/bin/%s "$@" 2>/dev/null || true\n' "$tool" > "$shim/$tool"
    chmod 0755 "$shim/$tool"
  done
  printf 'DPkg::Path "/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin";\n' > "$shim/apt.conf"
  echo "$shim"
}

bwrap_run() {
  local rootfs="$1" shim; shift
  shim="$(chown_shim)"
  local args=(--unshare-user --uid 0 --gid 0 --unshare-pid --unshare-ipc --unshare-uts
              --hostname forkmesh-baseline --die-with-parent
              --bind "$rootfs" / --proc /proc --dev /dev
              --ro-bind "$(realpath /etc/resolv.conf)" /etc/resolv.conf
              --ro-bind "$shim" /usr/local/sbin
              --ro-bind "$shim/apt.conf" /etc/apt/apt.conf
              --clearenv
              --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
              --setenv HOME /root --setenv LANG C.UTF-8)
  while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
    args+=("$1"); shift
  done
  shift
  bwrap "${args[@]}" "$@"
}

pull_rootfs() {
  local reference="$1" digest="$2" dest="$3"
  inside_cache "$dest" || die "refusing to replace $dest outside $cache"
  rm -rf "$dest"
  python3 "$recipe/oci_rootfs.py" "$reference" "$dest" ${digest:+--digest "$digest"}
}

prepare_bwrap() {
  local rootfs="$cache/rootfs/builder" stamps jobs key prefix fresh=0
  stamps="$rootfs/.forkmesh-stages"
  jobs="${FORKMESH_BASELINE_JOBS:-$(default_jobs 1)}"
  mkdir -p "$cache/downloads" "$cache/rootfs"
  if [ ! -f "$stamps" ] || ! grep -qxF "packages $(recipe_key packages)" "$stamps"; then
    log "pulling $FORKMESH_BASELINE_IMAGE@$FORKMESH_BASELINE_IMAGE_DIGEST"
    pull_rootfs "$FORKMESH_BASELINE_IMAGE" "$FORKMESH_BASELINE_IMAGE_DIGEST" "$rootfs" >/dev/null
    : > "$stamps"
    fresh=1
  fi
  for stage in "${stages[@]}"; do
    key="$stage $(recipe_key "$stage")"
    if [ "$fresh" = 0 ] && grep -qxF "$key" "$stamps"; then
      continue
    fi
    fresh=1
    prefix="$(stage_prefix "$stage")"
    [ -n "$prefix" ] && rm -rf "${rootfs:?}$prefix"
    sed -i "/^$stage /,\$d" "$stamps"
    log "stage $stage (jobs $jobs)"
    local started=$SECONDS
    bwrap_run "$rootfs" \
      --ro-bind "$recipe" /opt/forkmesh/recipe \
      --bind "$cache/downloads" /var/cache/forkmesh-downloads \
      --setenv FORKMESH_BASELINE_DOWNLOADS /var/cache/forkmesh-downloads \
      --setenv FORKMESH_BASELINE_JOBS "$jobs" \
      -- /bin/bash "/opt/forkmesh/recipe/stage-$stage.sh" \
      > "$cache/stage-$stage.log" 2>&1 \
      || die "stage $stage failed; log: $cache/stage-$stage.log"
    log "stage $stage done in $((SECONDS - started)) s"
    echo "$key" >> "$stamps"
  done
}

prepare_oci() {
  local engine="$1" tag
  tag="$(image_tag)"
  if "$engine" image inspect "$tag" >/dev/null 2>&1; then
    return
  fi
  log "building $tag with $engine"
  DOCKER_BUILDKIT=1 "$engine" build \
    --build-arg FORKMESH_BASELINE_JOBS="${FORKMESH_BASELINE_JOBS:-$(default_jobs 1)}" \
    -t "$tag" -f "$recipe/Containerfile" "$recipe"
}

cmd_prepare() {
  local engine
  engine="$(pick_engine)"
  case "$engine" in
    bwrap) prepare_bwrap ;;
    podman|docker) prepare_oci "$engine" ;;
    *) die "unknown engine $engine" ;;
  esac
}

cmd_build() {
  local source="" out="" version="" commit="" channel="latest" engine work jobs
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --source) source="$2"; shift 2 ;;
      --out) out="$2"; shift 2 ;;
      --version) version="$2"; shift 2 ;;
      --commit) commit="$2"; shift 2 ;;
      --channel) channel="$2"; shift 2 ;;
      *) usage ;;
    esac
  done
  [ -n "$source" ] && [ -n "$out" ] && [ -n "$version" ] && [ -n "$commit" ] || usage
  source="$(cd "$source" && pwd -P)"
  mkdir -p "$out"
  out="$(cd "$out" && pwd -P)"
  engine="$(pick_engine)"
  cmd_prepare
  work="$cache/work"
  mkdir -p "$work"
  jobs="${FORKMESH_BASELINE_APP_JOBS:-$(default_jobs 2)}"
  [ "$jobs" -gt 6 ] && jobs=6
  local envs=(FORKMESH_BASELINE_VERSION="$version" FORKMESH_BASELINE_COMMIT="$commit"
              FORKMESH_BASELINE_CHANNEL="$channel" FORKMESH_BASELINE_JOBS="$jobs")
  log "building ForkMesh $version ($commit) with $engine, -j$jobs"
  local started=$SECONDS
  case "$engine" in
    bwrap)
      local setenv=() pair
      for pair in "${envs[@]}"; do setenv+=(--setenv "${pair%%=*}" "${pair#*=}"); done
      bwrap_run "$cache/rootfs/builder" \
        --ro-bind "$source" /src --ro-bind "$tools_root" /tools --ro-bind "$recipe" /recipe \
        --bind "$work" /work --bind "$out" /out "${setenv[@]}" \
        -- /bin/bash /recipe/build-forkmesh.sh
      ;;
    podman|docker)
      local user=() envflags=() pair
      [ "$engine" = docker ] && user=(--user "$(id -u):$(id -g)")
      [ "$engine" = podman ] && user=(--userns=keep-id)
      for pair in "${envs[@]}"; do envflags+=(-e "$pair"); done
      "$engine" run --rm "${user[@]}" "${envflags[@]}" \
        -v "$source:/src:ro" -v "$tools_root:/tools:ro" -v "$recipe:/recipe:ro" \
        -v "$work:/work" -v "$out:/out" "$(image_tag)" /bin/bash /recipe/build-forkmesh.sh
      ;;
  esac
  log "built in $((SECONDS - started)) s"
  cmd_check "$out/forkmesh-linux-x86_64" "$out/forkmesh-linux-x86_64.AppImage"
}

cmd_check() {
  python3 "$recipe/check_glibc.py" --glibc-max "$FORKMESH_BASELINE_GLIBC_MAX" \
    --glibcxx-max "$FORKMESH_BASELINE_GLIBCXX_MAX" "$@"
}

smoke_rootfs() {
  local image="$1" name rootfs stamp
  name="smoke-$(echo "${image##*/}" | tr ':' '-')"
  rootfs="$cache/rootfs/$name"
  stamp="$(echo "$smoke_packages" | sha256sum | cut -c1-16)"
  if [ ! -f "$rootfs/.forkmesh-smoke" ] || [ "$(cat "$rootfs/.forkmesh-smoke")" != "$stamp" ]; then
    log "preparing $image"
    pull_rootfs "$image" "" "$rootfs" >/dev/null
    bwrap_run "$rootfs" -- /bin/bash -c "
      set -e
      export DEBIAN_FRONTEND=noninteractive
      echo 'APT::Sandbox::User \"root\";' > /etc/apt/apt.conf.d/90forkmesh
      apt-get update -qq
      apt-get install -y -qq --no-install-recommends $smoke_packages >/dev/null
      apt-get clean
      rm -rf /var/lib/apt/lists/*" > "$cache/$name.log" 2>&1 \
      || die "could not prepare $image; log: $cache/$name.log"
    echo "$stamp" > "$rootfs/.forkmesh-smoke"
  fi
  echo "$rootfs"
}

cmd_smoke() {
  local out="" version="" failed=0 rootfs scratch
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --out) out="$2"; shift 2 ;;
      --version) version="$2"; shift 2 ;;
      *) usage ;;
    esac
  done
  [ -n "$out" ] && [ -n "$version" ] || usage
  out="$(cd "$out" && pwd -P)"
  command -v bwrap >/dev/null 2>&1 || die "the smoke test needs bubblewrap"
  for image in "${smoke_images[@]}"; do
    rootfs="$(smoke_rootfs "$image")"
    scratch="$cache/smoke-tmp"
    inside_cache "$scratch" || die "refusing to use $scratch"
    rm -rf "$scratch"
    mkdir -p "$scratch"
    bwrap_run "$rootfs" --bind "$scratch" /tmp \
      --ro-bind "$out" /out --ro-bind "$recipe/smoke-test.sh" /smoke-test.sh \
      --setenv FORKMESH_BASELINE_VERSION "$version" \
      -- /bin/bash /smoke-test.sh || failed=1
    rm -rf "$scratch"
  done
  [ "$failed" = 0 ] || die "smoke test failed"
}

[ "$#" -gt 0 ] || usage
command="$1"; shift
case "$command" in
  engine) pick_engine ;;
  prepare) cmd_prepare ;;
  build) cmd_build "$@" ;;
  check) cmd_check "$@" ;;
  smoke) cmd_smoke "$@" ;;
  *) usage ;;
esac
