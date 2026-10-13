#!/usr/bin/env bash
# Package the Linux ForkMesh binary as a self-contained AppImage with zsync
# delta-update support (issue #370). Emits:
#   <outdir>/forkmesh-linux-<arch>.AppImage         the installer
#   <outdir>/forkmesh-linux-<arch>.AppImage.zsync   AppImageUpdate control file
#
# Signing/update env (all optional):
#   FORKMESH_GPG_KEY   GPG key id/email — detached-sign the AppImage if set.
#   FORKMESH_HOST      base URL baked into the embedded update-information so
#                      AppImageUpdate knows where to fetch deltas from
#                      (default: https://forkmesh.com).
#
# Release packaging is deliberately fail-closed. An AppDir archive is not the
# single-click installer promised to desktop users, so a runner without the
# AppImage toolchain must fail before publishing incomplete release metadata.
# See /docs#installers-updates.
set -euo pipefail

binary="$1"; outdir="${2:-.}"
arch="${FORKMESH_PKG_ARCH:-x86_64}"
version="${FORKMESH_PKG_VERSION:-0.0.0}"
channel="${FORKMESH_PKG_CHANNEL:-latest}"
host="${FORKMESH_HOST:-https://forkmesh.com}"
name="forkmesh-linux-${arch}.AppImage"

log() { echo "[appimage] $*" >&2; }

# appimagetool writes the automatic .zsync companion beside its current
# working directory even when the AppImage output itself names another path.
# Anchor both artifacts in the caller's requested release directory.
mkdir -p "$outdir"
outdir="$(cd "$outdir" && pwd -P)"
out="$outdir/$name"

# Actions deliberately expose no /dev/fuse.  AppImage build tools support
# extracting themselves to a temporary directory, which is equivalent for this
# non-interactive packaging use and lets the release toolchain stay rootless.
export APPIMAGE_EXTRACT_AND_RUN=1

case "${FORKMESH_PKG_QT_LINKAGE:-auto}" in
  shared) shared_qt=1 ;;
  static) shared_qt=0 ;;
  *)
    shared_qt=1
    if command -v readelf >/dev/null 2>&1 && ! readelf -d "$binary" | grep -q 'NEEDED.*libQt6Core'; then
      shared_qt=0
    fi
    ;;
esac
required_tools="linuxdeploy appimagetool"
[ "$shared_qt" = 1 ] && required_tools="linuxdeploy linuxdeploy-plugin-qt appimagetool"
for tool in $required_tools; do
  command -v "$tool" >/dev/null 2>&1 || {
    log "$tool is required to build the self-contained Linux installer"
    exit 1
  }
done

# linuxdeploy-plugin-qt discovers Qt's plugin and resource directories through
# qmake.  On hosts with both Qt 5 and Qt 6, the unversioned /usr/bin/qmake can
# still point at Qt 5 even though ForkMesh is linked against Qt 6.  That makes
# the plugin inspect the wrong installation and report that no Qt modules were
# found.  Prefer qmake6, while preserving an explicit caller override for
# nonstandard Qt installations.
if [ "$shared_qt" = 1 ]; then
  if [ -z "${QMAKE:-}" ]; then
    QMAKE="$(command -v qmake6 2>/dev/null || true)"
  fi
  if [ -z "$QMAKE" ] || [ ! -x "$QMAKE" ]; then
    log "a Qt 6 qmake is required to bundle the Qt runtime; set QMAKE or install qmake6"
    exit 1
  fi
  qmake_version="$("$QMAKE" -query QT_VERSION 2>/dev/null || true)"
  case "$qmake_version" in
    6.*) ;;
    *)
      log "QMAKE must select Qt 6 (got ${qmake_version:-unknown} from $QMAKE)"
      exit 1
      ;;
  esac
  export QMAKE
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
appdir="$workdir/ForkMesh.AppDir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/share/applications" \
         "$appdir/usr/share/icons/hicolor/512x512/apps"

cp "$binary" "$appdir/usr/bin/forkmesh"
chmod 0755 "$appdir/usr/bin/forkmesh"

# The token-only deployment flow is part of the installed application, not a
# source-checkout feature. Copy the exact CMake-installed resource tree into the
# AppDir before linuxdeploy/appimagetool seal it.
install_root="${FORKMESH_PKG_INSTALL_ROOT:-}"
resource_root="${install_root%/}/share/forkmesh"
if [ -z "$install_root" ] || [ ! -f "$resource_root/tools/cloudflare_bootstrap.py" ] \
   || [ ! -f "$resource_root/app/src/entry.py" ] \
   || [ ! -x "$resource_root/app/deploy.sh" ] \
   || [ ! -d "$resource_root/app/migrations" ] \
   || [ ! -d "$resource_root/www/public" ] \
   || [ ! -d "$resource_root/world/public" ]; then
  log "CMake-installed ForkMesh deployment resources are incomplete"
  exit 1
fi
mkdir -p "$appdir/usr/share"
cp -R "$resource_root" "$appdir/usr/share/forkmesh"

cat > "$appdir/forkmesh.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=ForkMesh
Exec=forkmesh %u
Icon=forkmesh
Categories=Development;
Terminal=false
MimeType=x-scheme-handler/forkmesh;
EOF
cp "$appdir/forkmesh.desktop" "$appdir/usr/share/applications/forkmesh.desktop"

# linuxdeploy accepts freedesktop icon sizes only up to 512x512. The desktop
# source icon is intentionally 1024x1024 for high-density native builds, so use
# the checked-in 512x512 application favicon for the AppImage instead of asking
# each offline release runner to have an image-resizing utility installed.
icon="$appdir/forkmesh.png"
icon_source="www/public/favicon/android-chrome-512x512.png"
[ -f "$icon_source" ] || {
  log "the checked-in 512x512 AppImage icon is missing"
  exit 1
}
cp "$icon_source" "$icon"
cp "$icon" "$appdir/usr/share/icons/hicolor/512x512/apps/forkmesh.png"

# AppImageUpdate reads this "update-information" from the built AppImage and uses
# it to fetch a zsync binary delta instead of the whole file on each update.
update_info="zsync|${host%/}/${channel}/${name}.zsync"
export UPDATE_INFORMATION="$update_info"

# Bundle Qt and its platform plugins so the result runs without a local Qt SDK.
log "linuxdeploy: bundling Qt runtime into AppDir"
qt_plugin=()
[ "$shared_qt" = 1 ] && qt_plugin=(--plugin qt)
linuxdeploy --appdir "$appdir" \
  --desktop-file "$appdir/forkmesh.desktop" \
  --icon-file "$icon" \
  "${qt_plugin[@]}" >&2

log "appimagetool: building $name (update-info: $update_info)"
( cd "$outdir" &&
  ARCH="$arch" appimagetool \
    --updateinformation "$update_info" "$appdir" "$name" >&2 )
[ -s "$out" ] || { log "appimagetool produced no installer"; exit 1; }

# zsync control file for delta updates (only meaningful for a real AppImage).
if command -v zsyncmake >/dev/null 2>&1 && [ "${out##*.}" = "AppImage" ]; then
  log "zsyncmake: writing ${name}.zsync"
  ( cd "$outdir" && zsyncmake -u "$name" "$name" >&2 ) || \
    log "zsyncmake failed; AppImage ships without a delta control file"
fi

# Optional detached GPG signature (Authenticode/Gatekeeper equivalents live in
# the Windows/macOS packagers).
if [ -n "${FORKMESH_GPG_KEY:-}" ] && command -v gpg >/dev/null 2>&1; then
  log "gpg: detached-signing $out with key ${FORKMESH_GPG_KEY}"
  gpg --batch --yes --local-user "$FORKMESH_GPG_KEY" \
      --output "${out}.sig" --detach-sign "$out" >&2 || \
    log "gpg signing failed; shipping unsigned AppImage"
else
  log "no FORKMESH_GPG_KEY (or gpg missing) — shipping UNSIGNED AppImage"
fi

log "done: $out (version $version)"
echo "$out"
