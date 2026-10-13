#!/usr/bin/env bash
# Ban addresses that keep failing SSH on a mirror host.
#
#   tools/mirror_fail2ban.sh <host-ip> [<host-ip>...]
#
# Installs fail2ban and server/packaging/fail2ban/forkmesh-sshd.local (sshd
# jail read from the systemd journal, aggressive mode, 5 failures in 10 min ->
# 1 h ban, longer for repeat offenders), which is what new Vultr mirrors get at
# install. Run tools/harden_mirror_sshd.sh first: with passwords off, every
# failure is a bot. Idempotent; it only ever adds the jail, and proves the
# jail is running and that a fresh key login from here still works.
set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: mirror_fail2ban.sh <host-ip> [<host-ip>...]" >&2; exit 2; }

cd "$(dirname "$0")/.."
# shellcheck source=tools/forkmesh-paths.sh
. tools/forkmesh-paths.sh
SSH_KEY="${FORKMESH_FLEET_SSH_KEY:-$(forkmesh_ssh_dir)/vultr_mirror_ed25519}"
CONF="server/packaging/fail2ban/forkmesh-sshd.local"
SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 -i "$SSH_KEY" -o IdentitiesOnly=yes)

failed=0
for host in "$@"; do
    echo "== $host"
    if ! ssh "${SSH_OPTS[@]}" "root@$host" 'bash -s' <<REMOTE; then
set -eu
export DEBIAN_FRONTEND=noninteractive
if ! command -v fail2ban-client >/dev/null 2>&1; then
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends fail2ban python3-systemd >/dev/null
fi
cat > /etc/fail2ban/jail.d/forkmesh-sshd.local <<'JAIL'
$(cat "$CONF")
JAIL
chmod 0644 /etc/fail2ban/jail.d/forkmesh-sshd.local
systemctl enable fail2ban >/dev/null 2>&1
systemctl restart fail2ban
for i in 1 2 3 4 5 6 7 8 9 10; do
    fail2ban-client status sshd >/dev/null 2>&1 && break
    sleep 1
done
fail2ban-client status sshd
REMOTE
        echo "FAILED: $host was not changed or could not be reached" >&2
        failed=1
        continue
    fi
    if ssh "${SSH_OPTS[@]}" "root@$host" true; then
        echo "ok: $host bans repeat SSH failures; key login still works"
    else
        echo "FAILED: a fresh key login to $host did not work - check it now" >&2
        failed=1
    fi
done
exit "$failed"
