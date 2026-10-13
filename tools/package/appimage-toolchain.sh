#!/usr/bin/env bash
# Provide the rootless AppImage build tools used by release.yml.  Actions run
# with a fresh $HOME, so relying on a manually provisioned /usr/local binary
# makes a release fail only after its desktop build has completed.
#
# Source this file rather than executing it.  It exports:
#   FORKMESH_APPIMAGE_TOOLCHAIN_READY   1 when all tools are ready, else 0
#   FORKMESH_APPIMAGE_TOOLCHAIN_STATUS  ready | offline | failed
#   FORKMESH_APPIMAGE_TOOLCHAIN_REASON  explanation when not ready
#
# The download is deliberately gated by the runner's Actions egress policy.
# It never tries to modify the host or use sudo.

FORKMESH_APPIMAGE_TOOLCHAIN_READY=0
FORKMESH_APPIMAGE_TOOLCHAIN_STATUS=failed
FORKMESH_APPIMAGE_TOOLCHAIN_REASON=""

# shellcheck source=tools/forkmesh-paths.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/forkmesh-paths.sh"
_fm_appimage_toolchain_dir="$(forkmesh_toolchain_dir)/appimage"

_fm_appimage_tools_ready() {
  local tool
  for tool in linuxdeploy linuxdeploy-plugin-qt appimagetool; do
    command -v "$tool" >/dev/null 2>&1 || return 1
  done
  return 0
}

_fm_appimage_have_network() {
  if timeout 5 getent ahosts github.com >/dev/null 2>&1; then
    return 0
  fi
  # DNS is intentionally absent in an unshared Actions network namespace.
  # Try a raw HTTPS socket too, so a node with egress but custom DNS is not
  # incorrectly treated as offline.
  timeout 5 bash -c 'exec 3<>/dev/tcp/1.1.1.1/443' >/dev/null 2>&1
}

_fm_appimage_download() {
  local url="$1" destination="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL --connect-timeout 10 --max-time 180 -o "$destination" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -q --timeout=30 --tries=3 -O "$destination" "$url"
  else
    echo "neither curl nor wget is available" >&2
    return 1
  fi
}

if _fm_appimage_tools_ready; then
  FORKMESH_APPIMAGE_TOOLCHAIN_READY=1
  FORKMESH_APPIMAGE_TOOLCHAIN_STATUS=ready
  return 0
fi

if ! _fm_appimage_have_network; then
  FORKMESH_APPIMAGE_TOOLCHAIN_STATUS=offline
  FORKMESH_APPIMAGE_TOOLCHAIN_REASON="the AppImage tools are not installed and this Actions run has no network egress"
  return 0
fi

case "$(uname -m)" in
  x86_64|amd64|x64) _fm_appimage_arch="x86_64" ;;
  aarch64|arm64)    _fm_appimage_arch="aarch64" ;;
  *)
    FORKMESH_APPIMAGE_TOOLCHAIN_REASON="unsupported Linux architecture: $(uname -m)"
    return 0
    ;;
esac

_fm_appimage_stage="$(mktemp -d "${TMPDIR:-/tmp}/forkmesh-appimage-tools.XXXXXX")" || {
  FORKMESH_APPIMAGE_TOOLCHAIN_REASON="could not create an AppImage toolchain staging directory"
  return 0
}
_fm_appimage_cleanup() { rm -rf "$_fm_appimage_stage"; }

# These versioned upstream release URLs avoid the mutable `continuous` channel.
# AppImage tools are executable build inputs; fetch them only into the
# disposable, unprivileged Action sandbox.
if ! _fm_appimage_download \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-${_fm_appimage_arch}.AppImage" \
    "$_fm_appimage_stage/linuxdeploy" ||
   ! _fm_appimage_download \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-${_fm_appimage_arch}.AppImage" \
    "$_fm_appimage_stage/linuxdeploy-plugin-qt" ||
   ! _fm_appimage_download \
    "https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-${_fm_appimage_arch}.AppImage" \
    "$_fm_appimage_stage/appimagetool"; then
  _fm_appimage_cleanup
  FORKMESH_APPIMAGE_TOOLCHAIN_REASON="could not download the AppImage build tools"
  return 0
fi

for _fm_appimage_tool in linuxdeploy linuxdeploy-plugin-qt appimagetool; do
  if [ ! -s "$_fm_appimage_stage/$_fm_appimage_tool" ]; then
    _fm_appimage_cleanup
    FORKMESH_APPIMAGE_TOOLCHAIN_REASON="downloaded AppImage tool $_fm_appimage_tool is empty"
    return 0
  fi
  chmod 0755 "$_fm_appimage_stage/$_fm_appimage_tool"
done

mkdir -p "$_fm_appimage_toolchain_dir" &&
  mv "$_fm_appimage_stage"/* "$_fm_appimage_toolchain_dir/" || {
    _fm_appimage_cleanup
    FORKMESH_APPIMAGE_TOOLCHAIN_REASON="could not install the AppImage build tools under $HOME"
    return 0
  }
_fm_appimage_cleanup
export PATH="$_fm_appimage_toolchain_dir:$PATH"

if ! _fm_appimage_tools_ready; then
  FORKMESH_APPIMAGE_TOOLCHAIN_REASON="the downloaded AppImage build tools are not executable"
  return 0
fi

# An Action sandbox has no /dev/fuse.  Every downloaded tool is an AppImage,
# so force its supported extract-and-run path instead of attempting a FUSE
# mount.  appimage.sh inherits this while it invokes the same tools.
export APPIMAGE_EXTRACT_AND_RUN=1
FORKMESH_APPIMAGE_TOOLCHAIN_READY=1
FORKMESH_APPIMAGE_TOOLCHAIN_STATUS=ready
FORKMESH_APPIMAGE_TOOLCHAIN_REASON=""
