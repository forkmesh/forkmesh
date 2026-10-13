#!/usr/bin/env bash
set -euo pipefail
url="$1"; sha256="$2"; dest="$3"
if [ -f "$dest" ] && echo "$sha256  $dest" | sha256sum -c --quiet - >/dev/null 2>&1; then
  exit 0
fi
mkdir -p "$(dirname "$dest")"
curl -fsSL --retry 3 --connect-timeout 20 -o "$dest.part" "$url"
if ! echo "$sha256  $dest.part" | sha256sum -c --quiet - >/dev/null 2>&1; then
  rm -f "$dest.part"
  echo "$(basename "$dest") does not match the pinned SHA-256" >&2
  exit 1
fi
mv "$dest.part" "$dest"
