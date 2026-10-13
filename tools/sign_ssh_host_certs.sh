#!/usr/bin/env bash
# Give every round-robin git gateway ONE SSH identity, via a host certificate.
#
# ssh.forkmesh.com resolves to several mirrors (see
# www/docs/operations/git-ssh-gateway.md). Each of them has its own host key, so a
# client that pinned one key hits "REMOTE HOST IDENTIFICATION HAS CHANGED" the
# moment DNS hands it a different member. The fix is not to share a private
# host key between machines — it is to have each machine keep its own key and
# present a certificate, signed by one CA, that asserts the shared name.
# Clients then trust the CA once:
#
#   @cert-authority ssh.forkmesh.com ssh-ed25519 AAAA...
#
#   tools/sign_ssh_host_certs.sh <host-ip> [<host-ip> ...]
#
# Idempotent: re-running re-signs (which is also how certificates are rotated
# before they expire). The CA private key never leaves this machine; only the
# resulting certificate is uploaded.
set -euo pipefail

cd "$(dirname "$0")/.."

# shellcheck source=tools/forkmesh-paths.sh
. tools/forkmesh-paths.sh
FLEET_KEY="${FORKMESH_FLEET_SSH_KEY:-$(forkmesh_ssh_dir)/vultr_mirror_ed25519}"
CA_KEY="${FORKMESH_SSH_HOST_CA:-$(forkmesh_ssh_dir)/host_ca_ed25519}"
GATEWAY_NAME="${FORKMESH_GATEWAY_NAME:-ssh.forkmesh.com}"
# A year, so a forgotten rotation surfaces as a warning window rather than a
# silent forever-valid credential.
VALIDITY="${FORKMESH_HOST_CERT_VALIDITY:--5m:+52w}"

SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=accept-new -i "$FLEET_KEY")

if [ "$#" -lt 1 ]; then
    echo "usage: sign_ssh_host_certs.sh <host-ip> [<host-ip> ...]" >&2
    exit 2
fi

if [ ! -f "$CA_KEY" ]; then
    echo "== creating the SSH host CA at $CA_KEY (private key stays here)"
    install -d -m 0700 "$(dirname "$CA_KEY")"
    ssh-keygen -t ed25519 -f "$CA_KEY" -N "" \
        -C "forkmesh-ssh-host-ca" >/dev/null
    chmod 0600 "$CA_KEY"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

for host in "$@"; do
    echo "== signing host certificate for $host"
    # Take the host's own public key; its private half never moves.
    ssh "${SSH_OPTS[@]}" "root@$host" 'cat /etc/ssh/ssh_host_ed25519_key.pub' \
        > "$work/$host.pub"
    [ -s "$work/$host.pub" ] || { echo "ERROR: no host key from $host" >&2; exit 1; }
    node="$(ssh "${SSH_OPTS[@]}" "root@$host" 'hostname')"

    # Principals: the shared round-robin name plus this member's own
    # addresses, so both `ssh ssh.forkmesh.com` and a direct connection to the
    # member validate against the same certificate.
    ssh-keygen -s "$CA_KEY" -I "forkmesh-gateway-$node" -h \
        -n "$GATEWAY_NAME,$host,$node.forkmesh.com,$node" \
        -V "$VALIDITY" "$work/$host.pub" >/dev/null
    cert="$work/$host-cert.pub"
    [ -s "$cert" ] || { echo "ERROR: signing produced no certificate" >&2; exit 1; }

    scp "${SSH_OPTS[@]}" -q "$cert" "root@$host:/etc/ssh/ssh_host_ed25519_key-cert.pub"
    ssh "${SSH_OPTS[@]}" "root@$host" bash -s <<'REMOTE'
set -euo pipefail
chown root:root /etc/ssh/ssh_host_ed25519_key-cert.pub
chmod 0644 /etc/ssh/ssh_host_ed25519_key-cert.pub
# HostCertificate is a global directive (it cannot live inside a Match block),
# so it gets its own include file rather than joining the gateway's.
install -d -m 0755 /etc/ssh/sshd_config.d
cat > /etc/ssh/sshd_config.d/50-forkmesh-hostcert.conf <<'CONF'
# Presented alongside the host's own key so clients that trust the ForkMesh
# host CA accept every round-robin member of ssh.forkmesh.com.
HostCertificate /etc/ssh/ssh_host_ed25519_key-cert.pub
CONF
if ! grep -q '^Include /etc/ssh/sshd_config.d/\*.conf' /etc/ssh/sshd_config; then
    printf '\nInclude /etc/ssh/sshd_config.d/*.conf\n' >> /etc/ssh/sshd_config
fi
if ! sshd -t; then
    rm -f /etc/ssh/sshd_config.d/50-forkmesh-hostcert.conf
    echo "ERROR: sshd rejected the certificate configuration; reverted." >&2
    exit 1
fi
systemctl reload ssh 2>/dev/null || systemctl reload sshd
ssh-keygen -L -f /etc/ssh/ssh_host_ed25519_key-cert.pub |
    sed -n 's/^ *\(Valid:.*\)/  \1/p;s/^ *\(Principals:\)/  \1/p'
REMOTE
done

echo
echo "== trust this CA on clients (one line in known_hosts):"
printf '@cert-authority %s %s\n' "$GATEWAY_NAME" "$(cat "$CA_KEY.pub" | cut -d' ' -f1,2)"
