#!/usr/bin/env bash
set -uo pipefail
version="${FORKMESH_BASELINE_VERSION:?}"
dir="${FORKMESH_BASELINE_OUT:-/out}"
asset="$dir/forkmesh-linux-x86_64"
label="$(. /etc/os-release && echo "$PRETTY_NAME"), glibc $(getconf GNU_LIBC_VERSION | cut -d' ' -f2)"
failed=0
pass() { echo "PASS [$label] $*"; }
fail() { echo "FAIL [$label] $*"; failed=1; }

sandbox_env() {
  local home="$1"
  mkdir -p "$home/.config" "$home/.local/share" "$home/.cache" "$home/tmp"
  env -i PATH=/usr/local/bin:/usr/bin:/bin HOME="$home" \
    XDG_CONFIG_HOME="$home/.config" XDG_DATA_HOME="$home/.local/share" \
    XDG_CACHE_HOME="$home/.cache" XDG_RUNTIME_DIR="$home/tmp" TMPDIR="$home/tmp" \
    LANG=C.UTF-8 QT_QPA_PLATFORM=offscreen APPIMAGE_EXTRACT_AND_RUN=1 "${@:2}"
}

missing="$(ldd "$asset" 2>&1 | grep 'not found' || true)"
[ -z "$missing" ] && pass "binary: every shared library resolves" || fail "binary: missing libraries: $missing"

for target in "$asset" "$asset.AppImage"; do
  name="$(basename "$target")"
  home="$(mktemp -d /tmp/forkmesh-smoke.XXXXXX)"
  got="$(sandbox_env "$home" "$target" --version 2>&1 | tail -n1)"
  [ "$got" = "ForkMesh $version" ] && pass "$name --version: $got" || fail "$name --version: $got"
  sandbox_env "$home" timeout -s TERM 25 "$target" > "$home/run.log" 2>&1
  rc=$?
  if [ "$rc" = 124 ]; then
    pass "$name started headless (offscreen) and was still running after 25 s"
  else
    fail "$name headless (offscreen) exited with $rc: $(tail -n 5 "$home/run.log" | tr '\n' ' ')"
  fi
  rm -rf "$home"
done
deb="$asset.deb"
if [ -f "$deb" ]; then
  if dpkg --dry-run -i "$deb" > /tmp/forkmesh-deb-dry-run.log 2>&1; then
    pass "deb: every dependency is installable ($(dpkg-deb -f "$deb" Version))"
  else
    fail "deb: dpkg would not install it: $(tail -n 5 /tmp/forkmesh-deb-dry-run.log | tr '\n' ' ')"
  fi
  home="$(mktemp -d /tmp/forkmesh-smoke.XXXXXX)"
  dpkg-deb -x "$deb" "$home/root"
  got="$(sandbox_env "$home" "$home/root/usr/bin/forkmesh" --version 2>&1 | tail -n1)"
  [ "$got" = "ForkMesh $version" ] && pass "deb /usr/bin/forkmesh --version: $got" || fail "deb /usr/bin/forkmesh --version: $got"
  rm -rf "$home"
else
  fail "deb: $deb is missing"
fi
exit "$failed"
