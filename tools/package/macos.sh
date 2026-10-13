#!/usr/bin/env bash
# Package the macOS ForkMesh client as a codesigned, notarized .dmg (issue #370).
# Emits:
#   <outdir>/forkmesh-macos-<arch>.dmg
#
# The input may be a .app bundle or a bare Mach-O executable (wrapped into a
# minimal .app). Signing/notarization env (all optional — unsigned when absent):
#   FORKMESH_APPLE_DEV_ID       "Developer ID Application: NAME (TEAMID)" identity
#   FORKMESH_APPLE_ID           Apple ID for notarytool
#   FORKMESH_APPLE_TEAM_ID      Apple Developer team id
#   FORKMESH_APPLE_APP_PASSWORD app-specific password for notarytool
#   FORKMESH_SPARKLE_ED_KEY     Ed25519 pub key (SUPublicEDKey) baked into Info.plist
#
# The app does not update itself on macOS; users install the newer .dmg.
# See docs/release/README.md.
set -euo pipefail

src="$1"; outdir="${2:-.}"
arch="${FORKMESH_PKG_ARCH:-arm64}"
version="${FORKMESH_PKG_VERSION:-0.0.0}"
[ "$version" = "" ] && version="0.0.0"
channel="${FORKMESH_PKG_CHANNEL:-latest}"
host="${FORKMESH_HOST:-https://forkmesh.com}"
name="forkmesh-macos-${arch}.dmg"
out="${outdir%/}/${name}"

log() { echo "[macos] $*" >&2; }

if [ "$(uname -s)" != "Darwin" ]; then
  log "macOS installers must be built on macOS"
  exit 1
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT

# Resolve to a .app bundle, wrapping a bare executable if needed.
if [ -d "$src" ] && [[ "$src" == *.app ]]; then
  app="$workdir/ForkMesh.app"
  cp -R "$src" "$app"
else
  app="$workdir/ForkMesh.app"
  mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
  cp "$src" "$app/Contents/MacOS/ForkMesh"
  chmod 0755 "$app/Contents/MacOS/ForkMesh"
  cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>ForkMesh</string>
  <key>CFBundleExecutable</key><string>ForkMesh</string>
  <key>CFBundleIdentifier</key><string>com.forkmesh.desktop</string>
  <key>CFBundleShortVersionString</key><string>${version}</string>
  <key>CFBundleVersion</key><string>${version}</string>
  <key>CFBundleURLTypes</key><array><dict>
    <key>CFBundleURLName</key><string>ForkMesh local control links</string>
    <key>CFBundleURLSchemes</key><array><string>forkmesh</string></array>
  </dict></array>
  <key>SUFeedURL</key><string>${host%/}/${channel}/appcast.xml</string>
  <key>SUPublicEDKey</key><string>${FORKMESH_SPARKLE_ED_KEY:-}</string>
</dict></plist>
EOF
fi

# A CMake .app contains the executable and application resources but not the Qt
# frameworks/plugins.  Bundle those before signing so the DMG works on a Mac
# without Homebrew or a Qt SDK.
macdeployqt="${MACDEPLOYQT:-}"
if [ -z "$macdeployqt" ] && command -v macdeployqt >/dev/null 2>&1; then
  macdeployqt="$(command -v macdeployqt)"
elif [ -z "$macdeployqt" ] && command -v brew >/dev/null 2>&1; then
  qt_prefix="$(brew --prefix qt 2>/dev/null || true)"
  [ -z "$qt_prefix" ] || macdeployqt="$qt_prefix/bin/macdeployqt"
fi
[ -x "$macdeployqt" ] || {
  log "macdeployqt is required to build the self-contained macOS installer"
  exit 1
}
log "macdeployqt: bundling Qt frameworks and plugins"
"$macdeployqt" "$app" -verbose=1 >&2

# Qt deployment does not copy the Homebrew OpenSSL dylibs linked by ForkMesh.
# Relocate those beside the executable before the final hardened signature.
app_exec="$app/Contents/MacOS/ForkMesh"
openssl_prefix="${OPENSSL_ROOT_DIR:-${OPENSSL_PREFIX:-}}"
if [ -z "$openssl_prefix" ] && command -v brew >/dev/null 2>&1; then
  openssl_prefix="$(brew --prefix openssl@3 2>/dev/null || true)"
fi
while IFS= read -r dylib; do
  [ -n "$dylib" ] || continue
  source="$dylib"
  case "$source" in
    @rpath/*|@loader_path/*|@executable_path/*)
      [ -n "$openssl_prefix" ] || continue
      source="$openssl_prefix/lib/$(basename "$source")" ;;
  esac
  [ -f "$source" ] || continue
  mkdir -p "$app/Contents/Frameworks"
  dest="$app/Contents/Frameworks/$(basename "$source")"
  cp "$source" "$dest"
  chmod u+w "$dest"
  loader_ref="@executable_path/../Frameworks/$(basename "$source")"
  install_name_tool -change "$dylib" "$loader_ref" "$app_exec"
  install_name_tool -id "$loader_ref" "$dest" 2>/dev/null || true
done < <(otool -L "$app_exec" | awk '/libcrypto|libssl/ {print $1}')

# macdeployqt follows only Qt's own dependency chain, and the OpenSSL copy above
# rewrites only the executable, so a bundled library can still reach back into
# Homebrew: libssl -> Cellar/openssl@3/.../libcrypto, or libbrotlicommon keeping
# its /opt/homebrew install name. Those load on the build Mac and fail on every
# other one. Point every such reference at the bundled copy, pulling in any
# library that is still missing, until nothing in the app names Homebrew.
frameworks="$app/Contents/Frameworks"
foreign_prefixes="${FORKMESH_PKG_FOREIGN_PREFIXES:-}${openssl_prefix:+:$openssl_prefix}"
is_homebrew_ref() {
  case "$1" in /opt/homebrew/*|/usr/local/*) return 0 ;; esac
  local prefix
  local IFS=:
  for prefix in $foreign_prefixes; do
    [ -n "$prefix" ] || continue
    case "$1" in "${prefix%/}"/*) return 0 ;; esac
  done
  return 1
}
pass=0
while :; do
  changed=0
  while IFS= read -r -d '' bin; do
    otool -h "$bin" >/dev/null 2>&1 || continue  # not Mach-O (scripts, data)
    id="$(otool -D "$bin" 2>/dev/null | sed -n 2p)"
    if [ -n "$id" ] && is_homebrew_ref "$id"; then
      install_name_tool -id "@executable_path/../Frameworks/$(basename "$id")" "$bin"
      changed=1
    fi
    while IFS= read -r ref; do
      [ "$ref" != "$id" ] || continue
      is_homebrew_ref "$ref" || continue
      base="$(basename "$ref")"
      if [ ! -f "$frameworks/$base" ]; then
        [ -f "$ref" ] || { log "bundled ${bin#"$app"/} needs $ref, which is not installed"; exit 1; }
        mkdir -p "$frameworks"
        cp "$ref" "$frameworks/$base"
        chmod u+w "$frameworks/$base"
      fi
      install_name_tool -change "$ref" "@executable_path/../Frameworks/$base" "$bin"
      changed=1
    done < <(otool -L "$bin" 2>/dev/null | tail -n +2 | awk '{print $1}')
    while IFS= read -r rpath; do
      is_homebrew_ref "$rpath" || continue
      install_name_tool -delete_rpath "$rpath" "$bin"
      changed=1
    done < <(otool -l "$bin" 2>/dev/null | awk '/cmd LC_RPATH/{getline; getline; print $2}' | sort -u)
  done < <(find "$app/Contents" -type f \( -name '*.dylib' -o -perm -u+x \) -print0)
  [ "$changed" = 1 ] || break
  pass=$((pass + 1))
  [ "$pass" -lt 10 ] || { log "Homebrew references remain after $pass passes"; exit 1; }
done

# CMake-produced .app bundles already have an Info.plist, while the bare-binary
# path above creates one. Register the same secret-free local-control scheme in
# both cases before signing so browser info-booth links reach the Qt client.
plist="$app/Contents/Info.plist"
plistbuddy="/usr/libexec/PlistBuddy"
"$plistbuddy" -c "Delete :CFBundleURLTypes" "$plist" >/dev/null 2>&1 || true
"$plistbuddy" -c "Add :CFBundleURLTypes array" "$plist"
"$plistbuddy" -c "Add :CFBundleURLTypes:0 dict" "$plist"
"$plistbuddy" -c "Add :CFBundleURLTypes:0:CFBundleURLName string ForkMesh local control links" "$plist"
"$plistbuddy" -c "Add :CFBundleURLTypes:0:CFBundleURLSchemes array" "$plist"
"$plistbuddy" -c "Add :CFBundleURLTypes:0:CFBundleURLSchemes:0 string forkmesh" "$plist"

resource_root="$app/Contents/Resources/forkmesh"
if [ ! -f "$resource_root/tools/cloudflare_bootstrap.py" ] \
   || [ ! -f "$resource_root/app/src/entry.py" ] \
   || [ ! -x "$resource_root/app/deploy.sh" ] \
   || [ ! -d "$resource_root/app/migrations" ] \
   || [ ! -d "$resource_root/www/public" ] \
   || [ ! -d "$resource_root/world/public" ]; then
  log "CMake-installed ForkMesh deployment resources are incomplete"
  exit 1
fi

if [ -n "${FORKMESH_APPLE_DEV_ID:-}" ]; then
  log "codesign: hardened-runtime signing with '${FORKMESH_APPLE_DEV_ID}'"
  codesign --force --deep --options runtime --timestamp \
    --sign "$FORKMESH_APPLE_DEV_ID" "$app" >&2
  codesign --verify --deep --strict --verbose=2 "$app" >&2
elif [ "${FORKMESH_REQUIRE_NATIVE_SIGNING:-0}" = "1" ]; then
  log "FORKMESH_APPLE_DEV_ID is required for public releases"
  exit 1
else
  # Apple Silicon refuses to run code without a valid signature, and
  # macdeployqt/install_name_tool above just invalidated the linker's ad-hoc
  # one: an unsigned bundle is killed at launch. Re-sign ad hoc so the app
  # runs (Gatekeeper still warns, since it is not a Developer ID signature).
  log "no FORKMESH_APPLE_DEV_ID — ad-hoc signing the .app (Gatekeeper will warn)"
  codesign --force --deep --sign - "$app" >&2
  codesign --verify --deep --strict "$app" >&2
fi

# Build the .dmg from the (signed) app.
if command -v create-dmg >/dev/null 2>&1; then
  create-dmg --volname "ForkMesh ${version}" --app-drop-link 400 120 \
    "$out" "$app" >&2 || hdiutil create -volname "ForkMesh ${version}" \
      -srcfolder "$app" -ov -format UDZO "$out" >&2
else
  hdiutil create -volname "ForkMesh ${version}" -srcfolder "$app" \
    -ov -format UDZO "$out" >&2
fi
[ -s "$out" ] || { log "DMG creation produced no installer"; exit 1; }

if [ -n "${FORKMESH_APPLE_ID:-}" ] && [ -n "${FORKMESH_APPLE_APP_PASSWORD:-}" ] \
   && [ -n "${FORKMESH_APPLE_TEAM_ID:-}" ] && command -v xcrun >/dev/null 2>&1; then
  log "notarytool: submitting $name for notarization"
  xcrun notarytool submit "$out" \
    --apple-id "$FORKMESH_APPLE_ID" \
    --team-id "$FORKMESH_APPLE_TEAM_ID" \
    --password "$FORKMESH_APPLE_APP_PASSWORD" --wait >&2
  log "stapler: attaching notarization ticket"
  if ! xcrun stapler staple "$out" >&2; then
    if [ "${FORKMESH_REQUIRE_NATIVE_SIGNING:-0}" = "1" ]; then
      log "stapler failed for a required public release"
      exit 1
    fi
    log "stapler failed; ticket will be fetched online"
  fi
elif [ "${FORKMESH_REQUIRE_NATIVE_SIGNING:-0}" = "1" ]; then
  log "complete Apple notarization credentials and xcrun are required for public releases"
  exit 1
else
  log "notarization credentials incomplete — shipping un-notarized .dmg"
fi

log "done: $out (version $version)"
echo "$out"
