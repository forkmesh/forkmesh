#!/usr/bin/env bash
# Package the Windows ForkMesh binary as an NSIS installer and Authenticode-sign
# it (issue #370). Emits:
#   <outdir>/forkmesh-windows-<arch>-setup.exe
#
# Signing env. Optional for local packaging; fail-closed for public releases
# (FORKMESH_REQUIRE_NATIVE_SIGNING=1). The signature is verified against the
# chain inside the PFX and must carry an RFC 3161 countersignature; whether
# Windows trusts the publisher is decided by the certificate's issuer (a
# self-issued one still shows "Unknown publisher"). NSIS and windeployqt are
# mandatory because the installer must carry its runtime:
#   On a Windows runner: signtool is used automatically if on PATH.
#   Cross-platform (osslsigncode):
#     FORKMESH_WIN_CERT_PFX        base64 of the .pfx code-signing cert
#     FORKMESH_WIN_CERT_PASSWORD   its password
#     FORKMESH_WIN_TIMESTAMP_URL   RFC-3161 timestamp URL (default: DigiCert)
#   Cross build from Linux (tools/package/windows-toolchain.sh): windeployqt
#   does not exist there, so set FORKMESH_WIN_QT_ROOT to the Qt kit prefix
#   (and FORKMESH_WIN_CXX to the cross g++ whose runtime DLLs should ship).
#
# The app does not update itself on Windows; users run the newer setup.exe.
# See docs/desktop/windows.md.
set -euo pipefail

binary="$1"; outdir="${2:-.}"
here="$(cd "$(dirname "$0")" && pwd)"
arch="${FORKMESH_PKG_ARCH:-x86_64}"
version="${FORKMESH_PKG_VERSION:-0.0.0}"
[ "$version" = "" ] && version="0.0.0"
name="forkmesh-windows-${arch}-setup.exe"
out="${outdir%/}/${name}"

log() { echo "[windows] $*" >&2; }

command -v makensis >/dev/null 2>&1 || {
  log "makensis is required to build the self-contained Windows installer"
  exit 1
}
# The Qt runtime is bundled by windeployqt on a Windows runner. When the
# installer is cross-built on Linux (tools/package/windows-toolchain.sh) there
# is no windeployqt binary, so the same deployment is done by hand from the
# kit named in FORKMESH_WIN_QT_ROOT: the Qt modules the client links, the
# plugins those modules load at runtime, and the MinGW C++ runtime.
if ! command -v windeployqt >/dev/null 2>&1 && [ -z "${FORKMESH_WIN_QT_ROOT:-}" ]; then
  log "windeployqt (or FORKMESH_WIN_QT_ROOT for a cross build) is required to bundle the Qt runtime"
  exit 1
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
payload="$workdir/payload"
mkdir -p "$payload"
cp "$binary" "$payload/forkmesh.exe"
# ForkMesh is a GUI application. A release commit whose CMake did not yet mark
# the target WIN32_EXECUTABLE links a console-subsystem PE, which opens a
# terminal window beside the client on every launch; the cross toolchain's
# objcopy rewrites that header field without relinking.
for objcopy in x86_64-w64-mingw32-objcopy "${FORKMESH_WIN_MINGW_ROOT:-/nonexistent}/usr/bin/x86_64-w64-mingw32-objcopy"; do
  if command -v "$objcopy" >/dev/null 2>&1; then
    "$objcopy" --subsystem=windows:6.0 "$payload/forkmesh.exe"
    log "objcopy: forkmesh.exe now uses the Windows GUI subsystem"
    break
  fi
done
deploy_qt_by_hand() {
  local qt="${FORKMESH_WIN_QT_ROOT%/}" module plugin runtime src
  [ -f "$qt/bin/Qt6Core.dll" ] || {
    log "FORKMESH_WIN_QT_ROOT ($qt) holds no Qt6Core.dll"
    return 1
  }
  for module in Core Gui Widgets Network Svg Concurrent; do
    cp "$qt/bin/Qt6${module}.dll" "$payload/"
  done
  # Plugin directories sit beside the executable: that is where Qt's default
  # plugin search looks, and exactly the layout windeployqt produces.
  for plugin in \
      platforms/qwindows.dll \
      styles/qmodernwindowsstyle.dll \
      imageformats/qgif.dll imageformats/qico.dll imageformats/qjpeg.dll \
      imageformats/qsvg.dll iconengines/qsvgicon.dll \
      tls/qschannelbackend.dll tls/qopensslbackend.dll tls/qcertonlybackend.dll \
      networkinformation/qnetworklistmanager.dll; do
    if [ -f "$qt/plugins/$plugin" ]; then
      mkdir -p "$payload/${plugin%/*}"
      cp "$qt/plugins/$plugin" "$payload/$plugin"
    else
      case "$plugin" in
        platforms/*|tls/qschannelbackend.dll)
          log "Qt kit lacks required plugin $plugin"; return 1 ;;
        *) log "Qt kit lacks optional plugin $plugin; skipping" ;;
      esac
    fi
  done
  # The C++ runtime must be at least as new as the compiler that built
  # forkmesh.exe, so prefer the cross compiler's own copies over the kit's.
  for runtime in libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll; do
    src=""
    if [ -n "${FORKMESH_WIN_CXX:-}" ]; then
      src="$("$FORKMESH_WIN_CXX" -print-file-name="$runtime" 2>/dev/null || true)"
      [ -f "$src" ] || src=""
    fi
    [ -n "$src" ] || src="$qt/bin/$runtime"
    [ -f "$src" ] || { log "MinGW runtime $runtime not found"; return 1; }
    cp "$src" "$payload/$runtime"
  done
  local objdump
  objdump="$(pe_objdump)" || {
    log "a PE objdump (x86_64-w64-mingw32-objdump) is required to resolve the Qt runtime"
    return 1
  }
  local -a pending=("$payload/forkmesh.exe")
  while IFS= read -r src; do pending+=("$src"); done \
    < <(find "$payload" -type f -iname '*.dll')
  local file dll
  while [ "${#pending[@]}" -gt 0 ]; do
    file="${pending[0]}"
    pending=("${pending[@]:1}")
    while IFS= read -r dll; do
      [ -e "$payload/$dll" ] && continue
      if [ -f "$qt/bin/$dll" ]; then
        cp "$qt/bin/$dll" "$payload/$dll"
        log "bundled $dll (imported by ${file##*/})"
        pending+=("$payload/$dll")
      fi
    done < <(pe_imports "$objdump" "$file")
  done
}

pe_objdump() {
  local tool
  for tool in x86_64-w64-mingw32-objdump \
      "${FORKMESH_WIN_MINGW_ROOT:-/nonexistent}/usr/bin/x86_64-w64-mingw32-objdump" \
      objdump; do
    command -v "$tool" >/dev/null 2>&1 || continue
    "$tool" -p "$payload/forkmesh.exe" 2>/dev/null | grep 'DLL Name:' >/dev/null || continue
    command -v "$tool"
    return 0
  done
  return 1
}

pe_imports() {
  "$1" -p "$2" | sed -n 's/^[[:space:]]*DLL Name:[[:space:]]*//p' | tr -d '\r'
}

verify_payload_imports() {
  local objdump file dll missing=0
  objdump="$(pe_objdump)" || {
    [ -n "${FORKMESH_WIN_QT_ROOT:-}" ] || return 0
    log "a PE objdump is required to verify the installer payload"
    return 1
  }
  while IFS= read -r file; do
    while IFS= read -r dll; do
      case "$dll" in
        [Qq][Tt]6*|[Ll][Ii][Bb]*) ;;
        *) continue ;;
      esac
      [ -n "$(find "$payload" -maxdepth 1 -iname "$dll" -print -quit)" ] && continue
      log "${file#"$payload"/} imports $dll, which the installer does not carry"
      missing=1
    done < <(pe_imports "$objdump" "$file")
  done < <(find "$payload" -type f \( -iname '*.exe' -o -iname '*.dll' \))
  [ "$missing" = 0 ] || { log "refusing to build an installer that cannot start"; return 1; }
  log "every Qt and runtime DLL the payload imports is bundled"
}

if command -v windeployqt >/dev/null 2>&1; then
  log "windeployqt: bundling the Qt runtime"
  windeployqt --release --no-translations --compiler-runtime \
    "$payload/forkmesh.exe" >&2
else
  log "bundling the Qt runtime by hand from $FORKMESH_WIN_QT_ROOT"
  deploy_qt_by_hand
fi

# windeployqt deploys Qt's TLS plugin but not the OpenSSL runtime that both the
# plugin and ForkMesh's direct OpenSSL::Crypto link need. Locate the official
# OpenSSL 3 DLLs from the configured SDK or PATH, accepting native, MSYS, and
# Wine paths, and fail closed instead of producing an installer that cannot
# start on a clean machine.
windows_path_to_host() {
  local value="$1" converted=""
  [ -n "$value" ] || return 1
  if [ -f "$value" ]; then
    printf '%s' "$value"
    return 0
  fi
  if [[ "$value" =~ ^([A-Za-z]):[/\\](.*)$ ]]; then
    local drive="${BASH_REMATCH[1],,}"
    local tail="${BASH_REMATCH[2]//\\//}"
    converted="${WINEPREFIX:-$HOME/.wine}/drive_${drive}/${tail}"
    if [ -f "$converted" ]; then
      printf '%s' "$converted"
      return 0
    fi
  fi
  if command -v cygpath >/dev/null 2>&1; then
    converted="$(cygpath -u "$value" 2>/dev/null || true)"
  elif command -v winepath >/dev/null 2>&1; then
    converted="$(winepath -u "$value" 2>/dev/null || true)"
  fi
  [ -n "$converted" ] && [ -f "$converted" ] || return 1
  printf '%s' "$converted"
}

copy_openssl_runtime() {
  local name="$1" candidate="" resolved="" bin=""
  for bin in \
    "${FORKMESH_WIN_OPENSSL_BIN:-}" \
    "${OPENSSL_ROOT_DIR:-}/bin" \
    "C:/Qt/Tools/OpenSSLv3/Win_x64/bin"; do
    [ "$bin" != "/bin" ] || continue
    candidate="${bin%/}/$name"
    resolved="$(windows_path_to_host "$candidate" || true)"
    if [ -n "$resolved" ]; then
      cp "$resolved" "$payload/$name"
      return 0
    fi
  done
  candidate="$(command -v "$name" 2>/dev/null || true)"
  resolved="$(windows_path_to_host "$candidate" || true)"
  if [ -n "$resolved" ]; then
    cp "$resolved" "$payload/$name"
    return 0
  fi
  log "required OpenSSL runtime DLL is unavailable: $name"
  return 1
}

copy_openssl_runtime libcrypto-3-x64.dll
copy_openssl_runtime libssl-3-x64.dll
verify_payload_imports

install_root="${FORKMESH_PKG_INSTALL_ROOT:-}"
resource_root="${install_root%/}/share/forkmesh"
if [ -z "$install_root" ] || [ ! -f "$resource_root/tools/cloudflare_bootstrap.py" ] \
   || [ ! -f "$resource_root/app/src/entry.py" ] \
   || [ ! -f "$resource_root/app/deploy.sh" ] \
   || [ ! -d "$resource_root/app/migrations" ] \
   || [ ! -d "$resource_root/www/public" ] \
   || [ ! -d "$resource_root/world/public" ]; then
  log "CMake-installed ForkMesh deployment resources are incomplete"
  exit 1
fi
mkdir -p "$payload/resources"
cp -R "$resource_root" "$payload/resources/forkmesh"

# The uninstaller deletes exactly this payload by name, never recursively:
# %LOCALAPPDATA%\ForkMesh is also the parent of legacy user data.
uninstall_manifest="$workdir/forkmesh-uninstall-manifest.nsh"
"$here/nsis-uninstall-manifest.sh" "$payload" "$uninstall_manifest"

log "makensis: building $name (version $version)"
makensis \
  -DFORKMESH_VERSION="$version" \
  -DFORKMESH_PAYLOAD="$payload" \
  -DFORKMESH_UNINSTALL_MANIFEST="$uninstall_manifest" \
  -DFORKMESH_OUTFILE="$out" \
  "$here/forkmesh.nsi" >&2
[ -s "$out" ] || { log "makensis produced no installer"; exit 1; }

# The verify step re-checks the signature against the chain carried in the
# PFX and its RFC 3161 countersignature. Say what a failure means, because
# "Failed" alone sends the reader looking for a packaging bug that is not
# there.
signing_rejected() {
  log "the Authenticode signature on $name does not verify"
  log "FORKMESH_WIN_CERT_PFX must hold a code-signing certificate with its"
  log "chain, and the timestamp authority's reply must be intact"
}

if [ "${FORKMESH_REQUIRE_NATIVE_SIGNING:-0}" = "1" ]; then
  [ -n "${FORKMESH_WIN_CERT_PFX:-}" ] || {
    log "FORKMESH_WIN_CERT_PFX is required for public Windows releases"
    exit 1
  }
  [ -n "${FORKMESH_WIN_CERT_PASSWORD:-}" ] || {
    log "FORKMESH_WIN_CERT_PASSWORD is required for public Windows releases"
    exit 1
  }
fi

if command -v signtool >/dev/null 2>&1 && [ -n "${FORKMESH_WIN_CERT_PFX:-}" ]; then
  pfx="$workdir/cert.pfx"
  printf '%s' "$FORKMESH_WIN_CERT_PFX" | base64 -d > "$pfx" || {
    log "FORKMESH_WIN_CERT_PFX is not valid base64"
    exit 1
  }
  [ -s "$pfx" ] || { log "FORKMESH_WIN_CERT_PFX decoded to an empty file"; exit 1; }
  ts="${FORKMESH_WIN_TIMESTAMP_URL:-http://timestamp.digicert.com}"
  log "signtool: Authenticode-signing $name"
  signtool sign /f "$pfx" /p "${FORKMESH_WIN_CERT_PASSWORD:-}" \
    /fd sha256 /tr "$ts" /td sha256 "$out" >&2
  signtool verify /pa "$out" >&2 || { signing_rejected; exit 1; }
elif command -v osslsigncode >/dev/null 2>&1 && [ -n "${FORKMESH_WIN_CERT_PFX:-}" ]; then
  pfx="$workdir/cert.pfx"
  printf '%s' "$FORKMESH_WIN_CERT_PFX" | base64 -d > "$pfx" || {
    log "FORKMESH_WIN_CERT_PFX is not valid base64"
    exit 1
  }
  [ -s "$pfx" ] || { log "FORKMESH_WIN_CERT_PFX decoded to an empty file"; exit 1; }
  signed="$workdir/signed.exe"
  # Sign with a fresh per-run copy of the tool. The release host's
  # application firewall keys its verdicts on the executable path and parks
  # connections from paths it has not been asked about, which turns the
  # timestamp request into a silent hang; a new path is always answered.
  signer="$workdir/osslsigncode-$$"
  cp "$(command -v osslsigncode)" "$signer"
  log "osslsigncode: Authenticode-signing $name (cross-platform)"
  # A trusted RFC 3161 timestamp keeps the signature valid after the
  # certificate expires. Public authorities and DNS both hiccup, so try each
  # authority a few times before failing the release.
  signed_ok=0
  for ts in ${FORKMESH_WIN_TIMESTAMP_URL:-http://timestamp.digicert.com http://timestamp.sectigo.com http://timestamp.globalsign.com/tsa/r6advanced1}; do
    for attempt in 1 2 3; do
      rm -f "$signed"
      if "$signer" sign -pkcs12 "$pfx" -pass "${FORKMESH_WIN_CERT_PASSWORD:-}" \
           -h sha256 -n "ForkMesh" -ts "$ts" -in "$out" -out "$signed" >&2; then
        signed_ok=1; break 2
      fi
      log "timestamping via $ts failed (attempt $attempt)"
      sleep 5
    done
  done
  [ "$signed_ok" = 1 ] || { log "could not obtain a trusted timestamp from any authority"; exit 1; }
  mv "$signed" "$out"
  # Verify against the certificate chain carried in the PFX itself: that
  # proves the signature and the timestamp countersignature are intact for a
  # CA-issued certificate and for a self-issued one alike (Windows decides
  # trust on the user's machine; a self-issued publisher shows as unknown).
  openssl pkcs12 -in "$pfx" -nokeys -passin "pass:${FORKMESH_WIN_CERT_PASSWORD:-}" \
    -out "$workdir/chain.pem" >/dev/null 2>&1 || { log "cannot read the certificate chain from FORKMESH_WIN_CERT_PFX"; exit 1; }
  "$signer" verify -CAfile "$workdir/chain.pem" -in "$out" >&2 || { signing_rejected; exit 1; }
elif [ "${FORKMESH_REQUIRE_NATIVE_SIGNING:-0}" = "1" ]; then
  log "a Windows signing certificate and signtool/osslsigncode are required for public releases"
  exit 1
else
  log "no code-signing cert (FORKMESH_WIN_CERT_PFX) or signing tool — shipping UNSIGNED installer"
fi

log "done: $out"
echo "$out"
