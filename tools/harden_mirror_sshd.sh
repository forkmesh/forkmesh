#!/usr/bin/env bash
# Lock a mirror host's sshd to key logins only.
#
#   tools/harden_mirror_sshd.sh <host-ip> [<host-ip>...]
#
# Installs server/packaging/ssh/10-forkmesh-hardening.conf (no passwords, no
# keyboard-interactive, root by key only, fewer tries and a shorter grace
# period), which is what new Vultr mirrors get at install. Password guessing
# against a host then ends before PAM is ever asked.
#
# Idempotent. Refuses a host whose root account has no authorized key (that
# would lock everybody out), applies the drop-in only after `sshd -t`
# validates it, and reloads sshd, which keeps every open session. Afterwards
# it proves a fresh key login still works and a password login is refused.
set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: harden_mirror_sshd.sh <host-ip> [<host-ip>...]" >&2; exit 2; }

cd "$(dirname "$0")/.."
# shellcheck source=tools/forkmesh-paths.sh
. tools/forkmesh-paths.sh
SSH_KEY="${FORKMESH_FLEET_SSH_KEY:-$(forkmesh_ssh_dir)/vultr_mirror_ed25519}"
CONF="server/packaging/ssh/10-forkmesh-hardening.conf"
SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 -i "$SSH_KEY" -o IdentitiesOnly=yes)

failed=0
for host in "$@"; do
    echo "== $host"
    if ! ssh "${SSH_OPTS[@]}" "root@$host" 'bash -s' <<REMOTE; then
set -eu
dropin=/etc/ssh/sshd_config.d/10-forkmesh-hardening.conf
if [ ! -s /root/.ssh/authorized_keys ]; then
    echo "ERROR: root has no authorized key here; refusing to turn passwords off." >&2
    exit 1
fi
install -d -m 0755 /etc/ssh/sshd_config.d
previous="\$(mktemp)"
had=0
if [ -e "\$dropin" ]; then cp "\$dropin" "\$previous"; had=1; fi
cat > "\$dropin" <<'SSHD'
$(cat "$CONF")
SSHD
chmod 0644 "\$dropin"
# At the top when it is missing: sshd keeps the first value it reads, so an
# Include after the main file's own settings would change nothing.
if ! grep -q '^Include /etc/ssh/sshd_config.d/\*.conf' /etc/ssh/sshd_config; then
    sed -i '1i Include /etc/ssh/sshd_config.d/*.conf' /etc/ssh/sshd_config
fi
if ! sshd -t || ! sshd -T | grep -qx 'passwordauthentication no'; then
    if [ "\$had" = 1 ]; then cp "\$previous" "\$dropin"; else rm -f "\$dropin"; fi
    rm -f "\$previous"
    echo "ERROR: sshd did not take the hardening; reverted, sshd untouched." >&2
    exit 1
fi
rm -f "\$previous"
systemctl reload ssh 2>/dev/null || systemctl reload sshd
sshd -T | grep -E '^(passwordauthentication|kbdinteractiveauthentication|permitrootlogin|maxauthtries|logingracetime|maxstartups) '
REMOTE
        echo "FAILED: $host was not changed or could not be reached" >&2
        failed=1
        continue
    fi
    # The reload applies to new connections: prove one still gets in by key,
    # and that a password is no longer even offered.
    if ! ssh "${SSH_OPTS[@]}" "root@$host" true; then
        echo "FAILED: a fresh key login to $host did not work - check it now" >&2
        failed=1
        continue
    fi
    refused="$(ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=15 \
        -o PubkeyAuthentication=no -o PreferredAuthentications=password,keyboard-interactive \
        "root@$host" true 2>&1 || true)"
    if printf '%s' "$refused" | grep -q 'Permission denied (publickey)'; then
        echo "ok: $host takes keys only"
    else
        echo "FAILED: $host still offers more than keys: $refused" >&2
        failed=1
    fi
done
exit "$failed"
