#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/image.env"
downloads="${FORKMESH_BASELINE_DOWNLOADS:-/var/cache/forkmesh-downloads}"
tools=/opt/forkmesh/appimage-tools
mkdir -p "$tools"
"$here/fetch.sh" \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/$FORKMESH_BASELINE_LINUXDEPLOY_TAG/linuxdeploy-x86_64.AppImage" \
  "$FORKMESH_BASELINE_LINUXDEPLOY_SHA256" "$downloads/linuxdeploy-x86_64.AppImage"
"$here/fetch.sh" \
  "https://github.com/AppImage/appimagetool/releases/download/$FORKMESH_BASELINE_APPIMAGETOOL_TAG/appimagetool-x86_64.AppImage" \
  "$FORKMESH_BASELINE_APPIMAGETOOL_SHA256" "$downloads/appimagetool-x86_64.AppImage"
install -m 0755 "$downloads/linuxdeploy-x86_64.AppImage" "$tools/linuxdeploy"
install -m 0755 "$downloads/appimagetool-x86_64.AppImage" "$tools/appimagetool"
