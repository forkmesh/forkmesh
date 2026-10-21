#!/usr/bin/env bash
# Install (or remove) the Git SSH relay on a self-hosted ForkMesh instance.
#
#   sudo bash tools/install_ssh_relay.sh --host ssh.example.org [--port 22]
#   sudo bash tools/install_ssh_relay.sh --check
#   sudo bash tools/install_ssh_relay.sh --uninstall
#
# Run it on the instance itself, from the instance's source checkout
# (/opt/forkmesh-instance/src after www/public/install-instance.sh). Relay
# mode (www/docs/operations/git-ssh-gateway.md) accepts `git@host:owner/repo`
# clones and pushes with the account's registered SSH keys, authorizes each
# operation through the control plane, and relays it to the instance's own
# HTTPS Git routes; it needs no access to repository storage.
#
# What it changes, each step idempotent:
#   * accounts git (forced-command only), forkmesh-ssh-auth (holds the token),
#     forkmesh-ssh-lookup (AuthorizedKeysCommand), group forkmesh-ssh-gateway;
#   * /opt/forkmesh-ssh (the gateway and broker, root-owned);
#   * /etc/forkmesh-ssh (configuration; the gateway token, generated here
#     once and never printed);
#   * forkmesh-ssh-auth.service (the authorization broker);
#   * SSH_GATEWAY_* in the instance's app.env, then restarts the app;
#   * /etc/ssh/sshd_config.d/60-forkmesh-git-relay.conf: one `Match User git`
#     block, validated with `sshd -t` before a reload. Root's and every other
#     account's SSH settings are left exactly as they are.
# Backups go to /var/backups/forkmesh-ssh-relay/<timestamp>/ first.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
APP_ENV="${FORKMESH_APP_ENV:-/etc/forkmesh-instance/app.env}"
APP_UNIT="forkmesh-instance.service"
OPT=/opt/forkmesh-ssh
ETC=/etc/forkmesh-ssh
DROPIN=/etc/ssh/sshd_config.d/60-forkmesh-git-relay.conf
BROKER_UNIT=/etc/systemd/system/forkmesh-ssh-auth.service
TMPFILES=/etc/tmpfiles.d/forkmesh-ssh-relay.conf
LOG=/var/log/forkmesh-ssh-gateway.log
GIT_HOME=/var/lib/forkmesh-ssh-git
BACKUP_ROOT=/var/backups/forkmesh-ssh-relay

HOST=""
PORT=22
ORIGIN=""
CONNECT="127.0.0.1:443"
MODE=install

die() { echo "install_ssh_relay: $*" >&2; exit 1; }
say() { echo "== $*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST="${2:-}"; shift 2 ;;
    --port) PORT="${2:-}"; shift 2 ;;
    --origin) ORIGIN="${2:-}"; shift 2 ;;
    --connect) CONNECT="${2:-}"; shift 2 ;;
    --check) MODE=check; shift ;;
    --uninstall) MODE=uninstall; shift ;;
    *) die "unknown argument: $1" ;;
  esac
done

[ "$(id -u)" = 0 ] || die "run as root"
[ -r "$APP_ENV" ] || die "no instance settings at $APP_ENV"
for file in tools/ssh_gateway.py tools/ssh_authorization_broker.py \
    server/packaging/ssh/60-forkmesh-git-relay.conf \
    server/packaging/systemd/forkmesh-ssh-authorization-broker.service; do
  [ -s "$SRC/$file" ] || die "missing $SRC/$file (run from an instance checkout)"
done

env_value() { awk -F= -v key="$1" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$APP_ENV"; }

# Rewrite app.env keeping every line except the SSH_GATEWAY_* ones, then add
# the given KEY=VALUE pairs. Same owner, group and mode as before.
set_app_env() {
  python3 - "$APP_ENV" "$@" <<'PY'
import os, sys
path, pairs = sys.argv[1], sys.argv[2:]
st = os.stat(path)
lines = [line for line in open(path, encoding="utf-8").read().splitlines()
         if not line.startswith("SSH_GATEWAY_")]
lines += pairs
tmp = path + ".ssh-relay.new"
fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, st.st_mode & 0o7777)
with os.fdopen(fd, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines) + "\n")
os.chown(tmp, st.st_uid, st.st_gid)
os.replace(tmp, path)
PY
}

reload_sshd() {
  sshd -t || return 1
  systemctl reload ssh 2>/dev/null || systemctl reload sshd
}

check() {
  local ok=0
  say "gateway configuration"
  runuser -u git -- /opt/forkmesh-ssh/ssh-gateway-entrypoint check || ok=1
  say "broker"
  systemctl is-active --quiet forkmesh-ssh-auth.service && echo active || { echo inactive; ok=1; }
  say "sshd"
  sshd -t && echo "sshd -t: ok" || ok=1
  sshd -T -C user=git,host=check,addr=192.0.2.1 | grep -E '^(authorizedkeyscommand|permittty|disableforwarding|authenticationmethods) ' || ok=1
  echo "root (unchanged by this drop-in):"
  sshd -T -C user=root,host=check,addr=192.0.2.1 | grep -E '^(permitrootlogin|passwordauthentication|pubkeyauthentication) '
  say "app settings"
  for key in SSH_GATEWAY_HOST SSH_GATEWAY_PORT SSH_GATEWAY_REPOSITORIES; do
    echo "$key=$(env_value "$key")"
  done
  [ -n "$(env_value SSH_GATEWAY_TOKEN)" ] && echo "SSH_GATEWAY_TOKEN=<set>" || { echo "SSH_GATEWAY_TOKEN missing"; ok=1; }
  return "$ok"
}

uninstall() {
  local stamp backup
  stamp="$(date -u +%Y%m%dT%H%M%SZ)"
  backup="$BACKUP_ROOT/$stamp"
  install -d -m 0700 "$backup"
  cp -a "$APP_ENV" "$backup/app.env"
  [ -f "$DROPIN" ] && cp -a "$DROPIN" "$backup/"
  say "removing the sshd drop-in"
  rm -f "$DROPIN"
  reload_sshd || die "sshd -t failed after removing $DROPIN; restore from $backup"
  say "stopping the broker"
  systemctl disable --now forkmesh-ssh-auth.service 2>/dev/null || true
  rm -f "$BROKER_UNIT" "$TMPFILES"
  systemctl daemon-reload
  say "unpublishing SSH addresses"
  set_app_env
  systemctl restart "$APP_UNIT"
  # Accounts and files stay (locked, unreachable without the drop-in) so a
  # reinstall reuses the same token; delete $ETC to rotate it.
  usermod -L git 2>/dev/null || true
  say "removed; backup in $backup"
}

[ "$MODE" = check ] && { check; exit $?; }
[ "$MODE" = uninstall ] && { uninstall; exit 0; }

[ -n "$HOST" ] || die "--host is required (a DNS-only name for SSH, e.g. ssh.example.org)"
[[ "$PORT" =~ ^[0-9]{1,5}$ ]] && [ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || die "invalid --port"
[ -n "$ORIGIN" ] || ORIGIN="$(env_value PUBLIC_BASE_URL)"
[[ "$ORIGIN" =~ ^https://[A-Za-z0-9.-]+(:[0-9]+)?/?$ ]] || die "invalid --origin '$ORIGIN'"
ORIGIN="${ORIGIN%/}"
[[ "$HOST" =~ ^[A-Za-z0-9.-]+$ ]] || die "invalid --host"
command -v sshd >/dev/null || die "sshd is not installed"
command -v python3 >/dev/null || die "python3 is not installed"
PYTHON3="$(command -v python3)"
[ "$PYTHON3" = /usr/bin/python3 ] || die "expected /usr/bin/python3"

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
backup="$BACKUP_ROOT/$stamp"
install -d -m 0700 "$BACKUP_ROOT" "$backup"
cp -a "$APP_ENV" "$backup/app.env"
cp -a /etc/ssh/sshd_config "$backup/sshd_config"
[ -d /etc/ssh/sshd_config.d ] && cp -a /etc/ssh/sshd_config.d "$backup/sshd_config.d"
say "backup in $backup"

say "accounts"
getent group forkmesh-ssh-gateway >/dev/null || groupadd --system forkmesh-ssh-gateway
getent passwd forkmesh-ssh-auth >/dev/null || useradd --system --no-create-home \
  --home-dir /nonexistent --shell /usr/sbin/nologin --gid forkmesh-ssh-gateway forkmesh-ssh-auth
getent passwd forkmesh-ssh-lookup >/dev/null || useradd --system --no-create-home \
  --home-dir /nonexistent --shell /usr/sbin/nologin forkmesh-ssh-lookup
getent passwd git >/dev/null || useradd --system --create-home --home-dir "$GIT_HOME" \
  --shell /bin/sh git
usermod -aG forkmesh-ssh-gateway forkmesh-ssh-lookup
usermod -aG forkmesh-ssh-gateway git
# sshd runs the forced command through the account's shell and refuses a
# locked ("!") password field when PAM is off; "*" is a password that can
# never match. Keys come only from AuthorizedKeysCommand.
usermod -s /bin/sh -p '*' git
chmod 0750 "$GIT_HOME"

say "programs in $OPT"
install -d -m 0755 -o root -g root "$OPT"
install -m 0755 -o root -g root "$SRC/tools/ssh_gateway.py" "$OPT/ssh_gateway.py"
install -m 0755 -o root -g root "$SRC/tools/ssh_authorization_broker.py" "$OPT/ssh_authorization_broker.py"
cat > "$OPT/ssh-gateway-entrypoint.new" <<'SH'
#!/bin/sh
original_command=${SSH_ORIGINAL_COMMAND-}
git_protocol=${GIT_PROTOCOL-}
exec /usr/bin/env -i \
  PATH=/usr/bin:/bin \
  LANG=C \
  LC_ALL=C \
  SSH_ORIGINAL_COMMAND="$original_command" \
  GIT_PROTOCOL="$git_protocol" \
  /usr/bin/python3 -I \
  /opt/forkmesh-ssh/ssh_gateway.py \
  --config /etc/forkmesh-ssh/ssh-gateway.json \
  "$@"
SH
install -m 0755 -o root -g root "$OPT/ssh-gateway-entrypoint.new" "$OPT/ssh-gateway-entrypoint"
rm -f "$OPT/ssh-gateway-entrypoint.new"
# AuthorizedKeysCommand gets a guaranteed stderr (the gateway logs no key
# material, paths or addresses; only fixed failure lines).
cat > "$OPT/ssh-gateway-authorized-key.new" <<'SH'
#!/bin/sh
exec 2>>/var/log/forkmesh-ssh-gateway.log
exec /opt/forkmesh-ssh/ssh-gateway-entrypoint "$@"
SH
install -m 0755 -o root -g root "$OPT/ssh-gateway-authorized-key.new" "$OPT/ssh-gateway-authorized-key"
rm -f "$OPT/ssh-gateway-authorized-key.new"
[ -f "$LOG" ] || install -m 0620 -o root -g forkmesh-ssh-gateway /dev/null "$LOG"

say "configuration in $ETC"
install -d -m 0755 -o root -g root "$ETC"
if [ ! -s "$ETC/ssh-gateway.token" ]; then
  umask 077
  python3 -c 'import secrets; print(secrets.token_urlsafe(48))' > "$ETC/ssh-gateway.token.new"
  umask 022
  chown forkmesh-ssh-auth:forkmesh-ssh-gateway "$ETC/ssh-gateway.token.new"
  chmod 0600 "$ETC/ssh-gateway.token.new"
  mv "$ETC/ssh-gateway.token.new" "$ETC/ssh-gateway.token"
  echo "generated a new gateway token (not printed)"
fi
chown forkmesh-ssh-auth:forkmesh-ssh-gateway "$ETC/ssh-gateway.token"
chmod 0600 "$ETC/ssh-gateway.token"
python3 - "$ETC" "$ORIGIN" "$CONNECT" <<'PY'
import json, os, sys
etc, origin, connect = sys.argv[1:4]
gateway = {
    "schemaVersion": 1,
    "authorizationSocket": "/run/forkmesh-ssh-auth/authorize.sock",
    "authorizationBrokerUser": "forkmesh-ssh-auth",
    "gatewayExecutable": "/opt/forkmesh-ssh/ssh-gateway-entrypoint",
    "httpGit": {"origin": origin, "connectAddress": connect},
    "limits": {
        "capacityDirectory": "/run/forkmesh-ssh-gateway",
        # A 1-CPU, 1 GB host: a few concurrent sessions, one slot kept for
        # pushes. Each session is one small Python process; the Git work
        # runs in the instance's own node.
        "maxConcurrentSessions": 8,
        "reservedReceiveSessions": 2,
        "maxConcurrentProcesses": 8,
        "reservedReceiveProcesses": 2,
        "uploadPackDeadlineSeconds": 1800,
        "receivePackDeadlineSeconds": 900,
        "terminationGraceSeconds": 3,
        "receiveMaxInputBytes": 268435456,
        "defaultRepositoryMaxBytes": 17179869184,
        "storageScanDeadlineSeconds": 10,
        "cleanupDeadlineSeconds": 5,
        "maxPackThreads": 1,
    },
}
broker = {
    "schemaVersion": 1,
    "type": "forkmesh.ssh-authorization-broker",
    "apiOrigin": origin,
    "apiConnectAddress": connect,
    "gatewayTokenFile": etc + "/ssh-gateway.token",
    "socketPath": "/run/forkmesh-ssh-auth/authorize.sock",
    "socketGroup": "forkmesh-ssh-gateway",
    "lookupUser": "forkmesh-ssh-lookup",
    "gitUser": "git",
}
for name, value in (("ssh-gateway.json", gateway),
                    ("ssh-authorization-broker.json", broker)):
    path = os.path.join(etc, name)
    with open(path + ".new", "w", encoding="utf-8") as handle:
        handle.write(json.dumps(value, indent=2) + "\n")
    os.chmod(path + ".new", 0o644)
    os.replace(path + ".new", path)
PY

say "runtime directory and broker service"
cat > "$TMPFILES" <<'CONF'
d /run/forkmesh-ssh-gateway 0700 git git -
CONF
systemd-tmpfiles --create "$TMPFILES"
sed -e "s#/opt/forkmesh-mirror/#$OPT/#g" \
    -e "s#/etc/forkmesh/ssh-authorization-broker.json#$ETC/ssh-authorization-broker.json#" \
    "$SRC/server/packaging/systemd/forkmesh-ssh-authorization-broker.service" > "$BROKER_UNIT.new"
mv "$BROKER_UNIT.new" "$BROKER_UNIT"
systemctl daemon-reload
systemctl enable forkmesh-ssh-auth.service >/dev/null 2>&1
systemctl restart forkmesh-ssh-auth.service
for _ in 1 2 3 4 5 6 7 8 9 10; do
  [ -S /run/forkmesh-ssh-auth/authorize.sock ] && break
  sleep 1
done
[ -S /run/forkmesh-ssh-auth/authorize.sock ] || die "the broker did not start: journalctl -u forkmesh-ssh-auth"
runuser -u git -- "$OPT/ssh-gateway-entrypoint" check >/dev/null || die "gateway check failed: runuser -u git -- $OPT/ssh-gateway-entrypoint check"

say "publishing SSH addresses in $APP_ENV"
token="$(cat "$ETC/ssh-gateway.token")"
set_app_env "SSH_GATEWAY_HOST=$HOST" "SSH_GATEWAY_PORT=$PORT" \
  "SSH_GATEWAY_TOKEN=$token" "SSH_GATEWAY_REPOSITORIES=*"
token=""
systemctl restart "$APP_UNIT"

say "sshd: Match User git drop-in"
install -d -m 0755 /etc/ssh/sshd_config.d
grep -qE '^Include /etc/ssh/sshd_config.d/\*\.conf' /etc/ssh/sshd_config \
  || die "sshd_config does not include sshd_config.d; add the drop-in by hand"
install -m 0644 -o root -g root "$SRC/server/packaging/ssh/60-forkmesh-git-relay.conf" "$DROPIN.new"
mv "$DROPIN.new" "$DROPIN"
if ! sshd -t; then
  rm -f "$DROPIN"
  die "sshd -t rejected the drop-in; removed it, the running sshd is untouched"
fi
systemctl reload ssh 2>/dev/null || systemctl reload sshd

say "done"
check || true
cat <<EOF

SSH relay installed. Next:
  * DNS: a DNS-only (not proxied) record $HOST -> this server's address.
  * Users add a public key at $ORIGIN/dashboard/settings/ssh-keys, then:
      ssh -T git@$HOST                       # prints a greeting
      git clone git@$HOST:<owner>/<repository>.git
  * Remove with: bash $SRC/tools/install_ssh_relay.sh --uninstall
EOF
