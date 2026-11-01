#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
# shellcheck source=tools/forkmesh-safe-rm.sh
. "$root/tools/forkmesh-safe-rm.sh"

binary="${1:?usage: deb.sh <forkmesh binary> [outdir]}"
outdir="${2:-.}"
arch="${FORKMESH_PKG_ARCH:-x86_64}"
version="${FORKMESH_PKG_VERSION:?FORKMESH_PKG_VERSION is required}"
build="${FORKMESH_PKG_BUILD:-0}"
install_root="${FORKMESH_PKG_INSTALL_ROOT:-}"
keyring="${FORKMESH_APT_KEYRING:-$here/apt/forkmesh-archive-keyring.asc}"
sources="$here/apt/forkmesh.sources"
icon="$root/www/public/favicon/android-chrome-512x512.png"

log() { echo "[deb] $*" >&2; }
die() { log "$*"; exit 1; }

case "$arch" in
  x86_64|amd64) deb_arch=amd64 ;;
  arm64|aarch64) deb_arch=arm64 ;;
  *) die "unsupported architecture $arch" ;;
esac
case "$build" in
  ''|*[!0-9]*) die "FORKMESH_PKG_BUILD must be a number (got '$build')" ;;
esac
printf '%s' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || die "version '$version' is not X.Y.Z"
deb_version="$version"
[ "$build" -gt 0 ] && deb_version="$version+$build"
name="forkmesh-linux-${arch}.deb"

for tool in dpkg-deb dpkg readelf objdump; do
  command -v "$tool" >/dev/null 2>&1 || die "$tool is required to build the Debian package"
done
[ -x "$binary" ] || die "no executable at $binary"
[ -f "$keyring" ] || die "the apt archive key $keyring is missing (tools/forkmesh-apt-repo.py init-key)"
[ -f "$sources" ] || die "$sources is missing"
[ -f "$icon" ] || die "$icon is missing"
resources="${install_root%/}/share/forkmesh"
[ -n "$install_root" ] && [ -f "$resources/tools/cloudflare_bootstrap.py" ] \
  || die "CMake-installed ForkMesh resources are missing under '${install_root}'"

mkdir -p "$outdir"
outdir="$(cd "$outdir" && pwd -P)"
scratch="${TMPDIR:-/tmp}"
stage="$(mktemp -d "$scratch/forkmesh-deb.XXXXXX")"
trap 'fm_safe_rm_tree "$scratch" "$stage"' EXIT
pkg="$stage/pkg"
mkdir -p "$pkg/DEBIAN" "$pkg/usr/bin" "$pkg/usr/share/applications" \
  "$pkg/usr/share/icons/hicolor/512x512/apps" "$pkg/usr/share/keyrings" \
  "$pkg/usr/share/doc/forkmesh" "$pkg/etc/apt/sources.list.d"

install -m 0755 "$binary" "$pkg/usr/bin/forkmesh"
[ -x "$install_root/bin/forkmesh-term" ] && install -m 0755 "$install_root/bin/forkmesh-term" "$pkg/usr/bin/forkmesh-term"
cp -R "$resources" "$pkg/usr/share/forkmesh"
install -m 0644 "$icon" "$pkg/usr/share/icons/hicolor/512x512/apps/forkmesh.png"
install -m 0644 "$keyring" "$pkg/usr/share/keyrings/forkmesh-archive-keyring.asc"
install -m 0644 "$sources" "$pkg/etc/apt/sources.list.d/forkmesh.sources"
install -m 0644 "$root/LICENSE" "$pkg/usr/share/doc/forkmesh/copyright"
cat > "$pkg/usr/share/applications/forkmesh.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=ForkMesh
Exec=forkmesh %u
Icon=forkmesh
Categories=Development;
Terminal=false
MimeType=x-scheme-handler/forkmesh;
EOF
find "$pkg" -type d -exec chmod 0755 {} +
find "$pkg/usr/share" "$pkg/etc" -type f -exec chmod go-w {} +

lib_packages() {
  local soname="$1"
  { dpkg -S "*/$soname" 2>/dev/null || true; } | awk -F': ' -v n="$soname" '
    {
      path = $2; owners = $1
      count = split(path, parts, "/")
      if (parts[count] != n) next
      if (path !~ /^(\/usr)?\/lib(64)?(\/x86_64-linux-gnu|\/aarch64-linux-gnu)?\//) next
      split(owners, list, ", ")
      for (i in list) { sub(/:.*/, "", list[i]); print list[i] }
    }' | sort -u
}

depends=()
while read -r soname; do
  [ -n "$soname" ] || continue
  owners="$(lib_packages "$soname")"
  [ -n "$owners" ] || die "no installed package provides $soname"
  [ "$(printf '%s\n' "$owners" | wc -l)" = 1 ] || die "$soname is provided by several packages: $(echo $owners)"
  depends+=("$owners")
done < <(readelf -d "$binary" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
glibc="$(objdump -T "$binary" | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | sort -uV | tail -n1)"
[ -n "$glibc" ] || die "could not read the glibc version $binary needs"
depends_line="$(printf '%s\n' "${depends[@]}" | sort -u | awk -v g="$glibc" '
  $0 == "libc6" { $0 = "libc6 (>= " g ")" }
  { line = line (NR > 1 ? ", " : "") $0 }
  END { print line }')"

installed_kb="$(du -sk "$pkg" | cut -f1)"
cat > "$pkg/DEBIAN/control" <<EOF
Package: forkmesh
Version: $deb_version
Architecture: $deb_arch
Maintainer: ForkMesh <founders@forkmesh.com>
Installed-Size: $installed_kb
Depends: $depends_line
Section: devel
Priority: optional
Homepage: https://forkmesh.com
Description: collaborate on, mirror and preserve Git repositories
 ForkMesh lets developers collaborate on, mirror, and preserve source-code
 repositories across independent hosts, without relying on any single
 hosting provider. This package installs the ForkMesh desktop app and adds
 the ForkMesh apt repository, so apt upgrade keeps it current.
EOF
printf '/etc/apt/sources.list.d/forkmesh.sources\n' > "$pkg/DEBIAN/conffiles"
( cd "$pkg" && find usr -type f -print0 | sort -z | xargs -0 md5sum ) > "$pkg/DEBIAN/md5sums"

out="$outdir/$name"
rm -f "$out"
log "dpkg-deb: building $name (forkmesh $deb_version, $deb_arch)"
dpkg-deb --root-owner-group -Zxz --build "$pkg" "$out" >&2
[ "$(dpkg-deb -f "$out" Version)" = "$deb_version" ] || die "the package does not carry version $deb_version"
log "done: $out"
echo "$out"
