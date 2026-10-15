#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/office-mac.sh [--find | --help | COMMAND...]

Finds the office build Mac on the LAN and logs in as `build`.

  (no arguments)  open an interactive shell on the Mac
  COMMAND...      run COMMAND on the Mac
  --find          print the Mac's address and exit

The Mac is looked for at the address it was last found at, then at
MAC_BUILD_HOST when set, then by scanning this machine's /24
for ssh and checking each host against the Mac's pinned host key.
See docs/native/MAC_BUILD.md.
EOF
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$here/forkmesh-paths.sh"

key_dir="${MAC_BUILD_KEY_DIR:-$(forkmesh_ssh_dir)/mac-build}"
address_file="$(forkmesh_home_dir)/state/mac-build/address"
key="$key_dir/id_ed25519"
known_hosts="$key_dir/known_hosts"

case "${1:-}" in
  -h|--help) usage; exit 0 ;;
esac

if [ ! -f "$key" ] || [ ! -f "$known_hosts" ]; then
  echo "office-mac: no Mac key in $key_dir (docs/native/MAC_BUILD.md)" >&2
  exit 1
fi

ssh_opts=(-i "$key" -o IdentitiesOnly=yes -o BatchMode=yes
          -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$known_hosts"
          -o HostKeyAlias=forkmesh-mac
          -o ServerAliveInterval=30 -o ServerAliveCountMax=10)

is_mac() {
  ssh -n "${ssh_opts[@]}" -o ConnectTimeout=5 "build@$1" true >/dev/null 2>&1
}

remember() {
  mkdir -p "$(dirname "$address_file")"
  printf '%s\n' "$1" > "$address_file"
}

find_mac() {
  local tried=" " candidate route prefix own host
  for candidate in "$(cat "$address_file" 2>/dev/null || true)" "${MAC_BUILD_HOST:-}"; do
    candidate="$(printf '%s' "$candidate" | tr -d '[:space:]')"
    [ -n "$candidate" ] || continue
    case "$tried" in *" $candidate "*) continue ;; esac
    tried="$tried$candidate "
    if is_mac "$candidate"; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  route="$(ip -4 -o route get 1.1.1.1 2>/dev/null || true)"
  if [[ ! "$route" =~ src\ ([0-9]+\.[0-9]+\.[0-9]+)\.([0-9]+) ]]; then
    echo "office-mac: cannot tell which LAN this machine is on" >&2
    return 1
  fi
  prefix="${BASH_REMATCH[1]}"
  own="${BASH_REMATCH[2]}"
  echo "office-mac: not at${tried% }; scanning $prefix.0/24 for its host key" >&2
  local open
  open="$(
    for host in $(seq 1 254); do
      candidate="$prefix.$host"
      [ "$host" = "$own" ] && continue
      case "$tried" in *" $candidate "*) continue ;; esac
      (timeout 2 bash -c ": </dev/tcp/$candidate/22" 2>/dev/null && echo "$candidate") &
    done
    wait
  )"
  for candidate in $open; do
    if is_mac "$candidate"; then
      echo "office-mac: found the Mac at $candidate" >&2
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  echo "office-mac: the Mac is not on $prefix.0/24 (is it on and on the Wi-Fi?)" >&2
  return 1
}

address="$(find_mac)"
remember "$address"

if [ "${1:-}" = "--find" ]; then
  printf '%s\n' "$address"
  exit 0
fi
if [ "$#" -eq 0 ]; then
  exec ssh -t "${ssh_opts[@]}" -o ConnectTimeout=15 "build@$address"
fi
exec ssh "${ssh_opts[@]}" -o ConnectTimeout=15 "build@$address" "$@"
