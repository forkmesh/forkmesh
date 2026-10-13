#!/usr/bin/env bash
# Download the AppImage build tools used by the release workflow into a
# job-local directory. ForkMesh Actions sandboxes are rootless and start from a
# minimal image, so the distribution package manager cannot be relied upon.
#
# Prints the directory to prepend to PATH, or an empty line when all three
# tools are already available on PATH.
set -euo pipefail

arch="${1:-}"

for tool in linuxdeploy linuxdeploy-plugin-qt appimagetool; do
  command -v "$tool" >/dev/null 2>&1 || { missing_tool=1; break; }
done
if [ "${missing_tool:-0}" = "0" ]; then
  printf '\n'
  exit 0
fi

case "$arch" in
  x86_64|amd64|x64) tool_arch="x86_64" ;;
  arm64|aarch64)    tool_arch="aarch64" ;;
  *)
    echo "[appimage-tools] unsupported Linux architecture: ${arch:-unknown}" >&2
    exit 1
    ;;
esac

command -v curl >/dev/null 2>&1 || {
  echo "[appimage-tools] curl is required to provision the AppImage toolchain" >&2
  exit 1
}

# Pin published releases so a rebuild of an existing tag cannot silently use a
# different packager. The upstream repositories publish architecture-specific
# AppImage assets with these names.
linuxdeploy_tag="1-alpha-20251107-1"
qt_plugin_tag="1-alpha-20250213-1"
appimagetool_tag="1.9.1"
tool_dir="${FORKMESH_APPIMAGE_TOOL_DIR:-${TMPDIR:-/tmp}/forkmesh-appimage-tools-${tool_arch}}"
mkdir -p "$tool_dir"

download() {
  local tool="$1" url="$2" destination
  destination="$tool_dir/$tool"
  if [ -x "$destination" ]; then
    return
  fi
  echo "[appimage-tools] downloading $tool" >&2
  curl --fail --location --retry 3 --retry-delay 2 --connect-timeout 20 \
    --output "${destination}.tmp" "$url"
  chmod 0755 "${destination}.tmp"
  mv "${destination}.tmp" "$destination"
}

download linuxdeploy \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/${linuxdeploy_tag}/linuxdeploy-${tool_arch}.AppImage"
download linuxdeploy-plugin-qt \
  "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${qt_plugin_tag}/linuxdeploy-plugin-qt-${tool_arch}.AppImage"
download appimagetool \
  "https://github.com/AppImage/appimagetool/releases/download/${appimagetool_tag}/appimagetool-${tool_arch}.AppImage"

printf '%s\n' "$tool_dir"
