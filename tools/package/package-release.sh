#!/usr/bin/env bash
# Build a native, (optionally) signed installer from an already-built ForkMesh
# executable and publish it alongside the bare binary (issue #370).
#
# This is the packaging layer on top of the CAS release plumbing (issue #304):
# the bare `forkmesh-<os>-<arch>[.exe]` is still published for headless/SSH
# installs, and this wraps it into the platform's native installer for desktop
# users, then hands both to tools/forkmesh-release-publish.sh so the installer
# lands in the same .forkmesh/releases/<channel>/{SHASUMS256.txt,release.json,appcast.xml}
# metadata. See /docs#installers-updates.
#
# Signing is env-var driven (CI secrets). Certificates remain optional, but the
# native packaging/runtime toolchain is mandatory: a release must never replace
# a single-click installer with a bare executable or source archive.
#
# Usage:
#   tools/package/package-release.sh --os <os> --arch <arch> --binary <path> \
#       [--channel latest] [--tag vX.Y.Z] [--outdir DIR]
#       [--install-root <cmake-install-prefix>]
#
#   --os      linux | macos | windows   (defaults to autodetected uname)
#   --arch    x86_64 | arm64
#   --binary  the built executable (bare binary, .exe, or path inside .app)
#   --channel release channel (default: $RELEASE_CHANNEL or "latest")
#   --tag     release tag (default: $FORKMESH_TAG)
#   --outdir  where to drop the installer (default: current dir)
#   --install-root staged `cmake --install` prefix containing the bundled
#              ForkMesh Cloudflare deployment resources
#
# Prints the produced installer path on the last stdout line.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
os=""; arch=""; binary=""; outdir="."; install_root=""
channel="${RELEASE_CHANNEL:-latest}"; tag="${FORKMESH_TAG:-}"

while [ $# -gt 0 ]; do
  case "$1" in
    --os)      os="$2"; shift 2 ;;
    --arch)    arch="$2"; shift 2 ;;
    --binary)  binary="$2"; shift 2 ;;
    --channel) channel="$2"; shift 2 ;;
    --tag)     tag="$2"; shift 2 ;;
    --outdir)  outdir="$2"; shift 2 ;;
    --install-root) install_root="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

# Autodetect os/arch the same way .forkmesh/release.yml does when unset.
if [ -z "$os" ]; then
  case "$(uname -s)" in
    Linux)  os="linux" ;;
    Darwin) os="macos" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT) os="windows" ;;
    *) os="$(uname -s | tr '[:upper:]' '[:lower:]')" ;;
  esac
fi
if [ -z "$arch" ]; then
  case "$(uname -m)" in
    x86_64|amd64|x64) arch="x86_64" ;;
    aarch64|arm64)    arch="arm64" ;;
    *) arch="$(uname -m)" ;;
  esac
fi

[ -n "$binary" ] || { echo "Error: --binary is required" >&2; exit 2; }
[ -e "$binary" ] || { echo "Error: no such binary: $binary" >&2; exit 1; }
mkdir -p "$outdir"

export FORKMESH_PKG_OS="$os"
export FORKMESH_PKG_ARCH="$arch"
export FORKMESH_PKG_TAG="$tag"
export FORKMESH_PKG_CHANNEL="$channel"
export FORKMESH_PKG_VERSION="${tag#v}"
export FORKMESH_PKG_INSTALL_ROOT="$install_root"

case "$os" in
  linux)   installer="$(bash "$here/appimage.sh" "$binary" "$outdir")" ;;
  windows) installer="$(bash "$here/windows.sh"  "$binary" "$outdir")" ;;
  macos)   installer="$(bash "$here/macos.sh"    "$binary" "$outdir")" ;;
  *) echo "No native packager for os '$os'." >&2; exit 1 ;;
esac

[ -n "$installer" ] && [ -f "$installer" ] || {
  echo "Packager for $os produced no native installer." >&2
  exit 1
}

echo "Packaged installer: $installer" >&2
# The dispatcher only builds the installer; the caller (release.yml) publishes
# it — together with the bare binary — via forkmesh-release-publish.sh so both
# share one release.json. Emit the path as the final stdout line.
echo "$installer"
