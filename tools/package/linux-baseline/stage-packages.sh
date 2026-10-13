#!/usr/bin/env bash
set -euo pipefail
. "$(dirname "$0")/image.env"

export DEBIAN_FRONTEND=noninteractive
snapshot="${FORKMESH_BASELINE_APT_MIRROR:-http://snapshot.debian.org/archive/debian/$FORKMESH_BASELINE_APT_SNAPSHOT}"
security="${FORKMESH_BASELINE_APT_SECURITY_MIRROR:-http://snapshot.debian.org/archive/debian-security/$FORKMESH_BASELINE_APT_SNAPSHOT}"
rm -f /etc/apt/sources.list.d/debian.sources
cat > /etc/apt/sources.list <<EOF
deb [check-valid-until=no] $snapshot bookworm main
deb [check-valid-until=no] $snapshot bookworm-updates main
deb [check-valid-until=no] $security bookworm-security main
EOF
cat > /etc/apt/apt.conf.d/90forkmesh <<'EOF'
APT::Sandbox::User "root";
APT::Install-Recommends "false";
Acquire::Retries "5";
Acquire::http::Timeout "60";
EOF

apt-get update
apt-get install -y \
  build-essential ca-certificates ccache cmake curl file git ninja-build patchelf \
  perl pkg-config python3 xz-utils zsync binutils desktop-file-utils squashfs-tools \
  zlib1g-dev libfontconfig-dev libfreetype-dev libdbus-1-dev libwebp-dev \
  libgl-dev libegl-dev libglx-dev libopengl-dev libdrm-dev \
  libx11-dev libx11-xcb-dev libxext-dev libxfixes-dev libxi-dev libxrender-dev \
  libxcb1-dev libxcb-cursor-dev libxcb-glx0-dev libxcb-icccm4-dev libxcb-image0-dev \
  libxcb-keysyms1-dev libxcb-randr0-dev libxcb-render-util0-dev libxcb-render0-dev \
  libxcb-shape0-dev libxcb-shm0-dev libxcb-sync-dev libxcb-util-dev \
  libxcb-xfixes0-dev libxcb-xinerama0-dev libxcb-xinput-dev libxcb-xkb-dev \
  libxkbcommon-dev libxkbcommon-x11-dev
apt-get clean
rm -rf /var/lib/apt/lists/*

libdir=/usr/lib/x86_64-linux-gnu
for lib in xcb-cursor xcb-icccm xcb-image xcb-keysyms xcb-render-util xcb-util; do
  [ -f "$libdir/lib$lib.a" ] || { echo "lib$lib.a is missing" >&2; exit 1; }
  rm -f "$libdir/lib$lib.so"
done
