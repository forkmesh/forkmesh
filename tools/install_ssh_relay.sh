#!/usr/bin/env bash
# Install (or remove) the Git SSH relay for a self-hosted ForkMesh instance.
#
# On the instance itself (the SSH name then points at the instance):
#   sudo bash tools/install_ssh_relay.sh --host ssh.example.org [--port 22]
#
# On a separate front host, so the instance's address stays private behind
# its CDN proxy (www/docs/operations/git-ssh-gateway.md, "Keep the origin
# private"):
#   instance$ sudo bash tools/install_ssh_relay.sh --control-plane-only --host ssh.example.org
#   instance$ sudo cat /etc/forkmesh-ssh/ssh-gateway.token |
#               ssh root@front 'bash /opt/forkmesh-src/tools/install_ssh_relay.sh \
#                 --remote-front --api-origin https://git.example.org \
#                 --host ssh.example.org --token-file -'
#
# Either host:
#   sudo bash tools/install_ssh_relay.sh --check
#   sudo bash tools/install_ssh_relay.sh --uninstall
#   bash tools/install_ssh_relay.sh [mode flags] --print-config   # changes nothing
#
# Token rotation without downtime (on the instance):
#   sudo bash tools/install_ssh_relay.sh --rotate-token     # both tokens valid
#   (remote front: pipe ssh-gateway.token.next to it with --token-file -)
#   sudo bash tools/install_ssh_relay.sh --finish-rotation  # old token retired
#
# Relay mode accepts `git@host:owner/repo` clones and pushes with the
# account's registered SSH keys, authorizes each operation through the
# control plane, and relays it to the instance's HTTPS Git routes; it needs
# no access to repository storage. A remote front reaches the instance only
# through its public HTTPS name (normal DNS, normal TLS verification) and
# keeps only the gateway token and its own SSH host key.
#
# What it changes, each step idempotent:
#   * accounts git (forced-command only), forkmesh-ssh-auth (holds the token),
#     forkmesh-ssh-lookup (AuthorizedKeysCommand), group forkmesh-ssh-gateway;
#   * /opt/forkmesh-ssh (the gateway and broker, root-owned);
#   * /etc/forkmesh-ssh (configuration; the gateway token, generated on the
#     instance once and never printed);
#   * forkmesh-ssh-auth.service (the authorization broker);
#   * on the instance: SSH_GATEWAY_* in app.env, then restarts the app;
#   * /etc/ssh/sshd_config.d/60-forkmesh-git-relay.conf: one `Match User git`
#     block, validated with `sshd -t` before a reload. Root's and every other
#     account's SSH settings are left exactly as they are.
#   * on a remote front only (a host that exists for this): 59-forkmesh-
#     front-limits.conf (MaxStartups and per-source limits for unauthenticated
#     connections) and a fail2ban jail for failed `git` logins.
# Backups go to /var/backups/forkmesh-ssh-relay/<timestamp>/ first.
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
APP_ENV="${FORKMESH_APP_ENV:-/etc/forkmesh-instance/app.env}"
APP_UNIT="forkmesh-instance.service"
OPT=/opt/forkmesh-ssh
ETC=/etc/forkmesh-ssh
DROPIN=/etc/ssh/sshd_config.d/60-forkmesh-git-relay.conf
LIMITS_DROPIN=/etc/ssh/sshd_config.d/59-forkmesh-front-limits.conf
JAIL=/etc/fail2ban/jail.d/forkmesh-git-ssh.local
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
ROLE=""
TOKEN_FILE=""
# A CDN proxy's request-body cap (Cloudflare's free plan: 100 MB); 0 = none.
MAX_REQUEST_MB=100
PRINT_CONFIG=0

die() { echo "install_ssh_relay: $*" >&2; exit 1; }
say() { echo "== $*"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --host) HOST="${2:-}"; shift 2 ;;
    --port) PORT="${2:-}"; shift 2 ;;
    --origin|--api-origin) ORIGIN="${2:-}"; shift 2 ;;
    --connect) CONNECT="${2:-}"; shift 2 ;;
    --remote-front) ROLE=front; shift ;;
    --control-plane-only) ROLE=control; shift ;;
    --token-file) TOKEN_FILE="${2:-}"; shift 2 ;;
    --max-request-mb) MAX_REQUEST_MB="${2:-}"; shift 2 ;;
    --print-config) PRINT_CONFIG=1; shift ;;
    --check) MODE=check; shift ;;
    --uninstall) MODE=uninstall; shift ;;
    --rotate-token) MODE=rotate; shift ;;
    --finish-rotation) MODE=finish-rotation; shift ;;
    *) die "unknown argument: $1" ;;
  esac
done

# The gateway and broker configuration files, written into directory $1. A
# remote front names only the public HTTPS origin: no connect address, so
# the gateway and the broker resolve it through DNS (to the CDN) and verify
# its certificate as usual.
write_configs() {
  python3 - "$1" "$ORIGIN" "$CONNECT" "$ROLE" "$MAX_REQUEST_MB" <<'PY'
import json, os, sys
etc, origin, connect, role, max_mb = sys.argv[1:6]
http_git = {"origin": origin}
if role == "front":
    if int(max_mb):
        http_git["maxRequestBytes"] = int(max_mb) * 1000 * 1000
    # One address cannot take every session slot with one registered key.
    http_git["maxSessionsPerClient"] = 4
else:
    http_git["connectAddress"] = connect
gateway = {
    "schemaVersion": 1,
    "authorizationSocket": "/run/forkmesh-ssh-auth/authorize.sock",
    "authorizationBrokerUser": "forkmesh-ssh-auth",
    "gatewayExecutable": "/opt/forkmesh-ssh/ssh-gateway-entrypoint",
    "httpGit": http_git,
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
    "gatewayTokenFile": "/etc/forkmesh-ssh/ssh-gateway.token",
    "socketPath": "/run/forkmesh-ssh-auth/authorize.sock",
    "socketGroup": "forkmesh-ssh-gateway",
    "lookupUser": "forkmesh-ssh-lookup",
    "gitUser": "git",
}
if role != "front":
    broker["apiConnectAddress"] = connect
for name, value in (("ssh-gateway.json", gateway),
                    ("ssh-authorization-broker.json", broker)):
    path = os.path.join(etc, name)
    with open(path + ".new", "w", encoding="utf-8") as handle:
        handle.write(json.dumps(value, indent=2) + "\n")
    os.chmod(path + ".new", 0o644)
    os.replace(path + ".new", path)
PY
}

valid_origin() {
  [[ "$ORIGIN" =~ ^https://[A-Za-z0-9.-]+(:[0-9]+)?/?$ ]] || die "invalid --api-origin '$ORIGIN'"
  ORIGIN="${ORIGIN%/}"
}

if [ "$PRINT_CONFIG" = 1 ]; then
  # What an install would write; touches nothing and needs no root.
  [ -n "$ROLE" ] || ROLE=local
  [ "$ROLE" != control ] || die "--control-plane-only writes no gateway configuration"
  if [ "$ROLE" = front ]; then
    [ -n "$ORIGIN" ] || die "--remote-front needs --api-origin https://<public name>"
  elif [ -z "$ORIGIN" ] && [ -r "$APP_ENV" ]; then
    ORIGIN="$(awk -F= '$1 == "PUBLIC_BASE_URL" {sub(/^[^=]*=/, ""); print; exit}' "$APP_ENV")"
  fi
  valid_origin
  [[ "$MAX_REQUEST_MB" =~ ^[0-9]{1,5}$ ]] || die "invalid --max-request-mb"
  preview="$(mktemp -d)"
  write_configs "$preview"
  for name in ssh-gateway.json ssh-authorization-broker.json; do
    echo "# $ETC/$name"
    cat "$preview/$name"
  done
  rm -rf "$preview"
  exit 0
fi

[ "$(id -u)" = 0 ] || die "run as root"
# --check, --uninstall and rotation act on whatever this host was set up as.
if [ -z "$ROLE" ]; then
  if [ "$MODE" != install ] && [ -s "$ETC/role" ]; then
    ROLE="$(cat "$ETC/role")"
  else
    ROLE=local
  fi
fi
case "$ROLE" in local|front|control) ;; *) die "unknown role in $ETC/role" ;; esac
# A remote front has no instance: no app.env, no instance secrets.
if [ "$ROLE" != front ]; then
  [ -r "$APP_ENV" ] || die "no instance settings at $APP_ENV"
fi
if [ "$ROLE" != control ] && [ "$MODE" = install ]; then
  for file in tools/ssh_gateway.py tools/ssh_authorization_broker.py \
      server/packaging/ssh/60-forkmesh-git-relay.conf \
      server/packaging/systemd/forkmesh-ssh-authorization-broker.service; do
    [ -s "$SRC/$file" ] || die "missing $SRC/$file (run from a ForkMesh source checkout)"
  done
fi

env_value() { awk -F= -v key="$1" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$APP_ENV"; }

# Rewrite app.env without the lines starting with $1 (a key prefix, or an
# exact "KEY="), then add the given KEY=VALUE pairs. Same owner, group and
# mode as before.
update_app_env() {
  python3 - "$APP_ENV" "$@" <<'PY'
import os, sys
path, drop, pairs = sys.argv[1], sys.argv[2], sys.argv[3:]
st = os.stat(path)
lines = [line for line in open(path, encoding="utf-8").read().splitlines()
         if not line.startswith(drop)]
lines += pairs
tmp = path + ".ssh-relay.new"
fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, st.st_mode & 0o7777)
with os.fdopen(fd, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines) + "\n")
os.chown(tmp, st.st_uid, st.st_gid)
os.replace(tmp, path)
PY
}

# Every SSH_GATEWAY_* line replaced by the given pairs.
set_app_env() {
  update_app_env SSH_GATEWAY_ "$@"
}

# Publish the SSH addresses with this host's token, keeping a rotation in
# progress and an address pin.
publish_app_env() {
  local token keep_next keep_allowed
  token="$(cat "$ETC/ssh-gateway.token")"
  keep_next="$(env_value SSH_GATEWAY_TOKEN_NEXT)"
  keep_allowed="$(env_value SSH_GATEWAY_ALLOWED_ADDRESSES)"
  set_app_env "SSH_GATEWAY_HOST=$HOST" "SSH_GATEWAY_PORT=$PORT" \
    "SSH_GATEWAY_TOKEN=$token" "SSH_GATEWAY_REPOSITORIES=*" \
    ${keep_next:+"SSH_GATEWAY_TOKEN_NEXT=$keep_next"} \
    ${keep_allowed:+"SSH_GATEWAY_ALLOWED_ADDRESSES=$keep_allowed"}
  systemctl restart "$APP_UNIT"
}

reload_sshd() {
  sshd -t || return 1
  systemctl reload ssh 2>/dev/null || systemctl reload sshd
}

new_token() {
  # $1: the file to create, 0600.
  umask 077
  python3 -c 'import secrets; print(secrets.token_urlsafe(48))' > "$1.new"
  umask 022
  chmod 0600 "$1.new"
  mv "$1.new" "$1"
}

own_token() {
  if getent passwd forkmesh-ssh-auth >/dev/null; then
    chown forkmesh-ssh-auth:forkmesh-ssh-gateway "$1"
  else
    chown root:root "$1"
  fi
  chmod 0600 "$1"
}

# Can this host reach the control plane's authorize endpoint with its token,
# through whatever proxy is on the way? A 403 means the token was accepted
# (the probe's request id is deliberately invalid, so nothing is looked up);
# a 401 means the token, or this host's address, was refused; anything else
# is the path (a proxy rule, DNS, TLS).
probe_control_plane() {
  python3 - "$ETC/ssh-authorization-broker.json" <<'PY'
import json, socket, sys, urllib.error, urllib.parse, urllib.request
config = json.load(open(sys.argv[1], encoding="utf-8"))
origin = config["apiOrigin"]
connect = config.get("apiConnectAddress")
if connect:
    # Take the broker's own path: the local front end, same certificate check.
    host = urllib.parse.urlsplit(origin).hostname
    address = connect.rpartition(":")[0].strip("[]")
    resolve = socket.getaddrinfo
    socket.getaddrinfo = lambda name, port, *rest: resolve(
        address if name == host else name, connect.rpartition(":")[2]
        if name == host else port, *rest)
token = open(config["gatewayTokenFile"], encoding="ascii").read().strip()
request = urllib.request.Request(
    origin + "/api/ssh/authorize", method="POST",
    data=b'{"action":"lookup","requestId":"probe"}',
    headers={"authorization": "Bearer " + token,
             "content-type": "application/json",
             "user-agent": "ForkMesh-ssh-authorization-broker/1.0"})
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
try:
    status = opener.open(request, timeout=10).status
except urllib.error.HTTPError as error:
    status = error.code
except OSError as error:
    print("cannot reach %s: %s" % (origin, error.__class__.__name__))
    sys.exit(1)
if status == 403:
    print("%s: token accepted" % origin)
    sys.exit(0)
if status == 401:
    print("%s: token refused; compare SSH_GATEWAY_TOKEN(_NEXT) and "
          "SSH_GATEWAY_ALLOWED_ADDRESSES on the instance" % origin)
else:
    print("%s: unexpected HTTP %d (a proxy rule may be blocking the relay)"
          % (origin, status))
sys.exit(1)
PY
}

check() {
  local ok=0
  if [ "$ROLE" = control ]; then
    say "app settings (the gateway runs on a remote front)"
    for key in SSH_GATEWAY_HOST SSH_GATEWAY_PORT SSH_GATEWAY_REPOSITORIES SSH_GATEWAY_ALLOWED_ADDRESSES; do
      echo "$key=$(env_value "$key")"
    done
    [ -n "$(env_value SSH_GATEWAY_TOKEN)" ] && echo "SSH_GATEWAY_TOKEN=<set>" || { echo "SSH_GATEWAY_TOKEN missing"; ok=1; }
    [ -z "$(env_value SSH_GATEWAY_TOKEN_NEXT)" ] || echo "SSH_GATEWAY_TOKEN_NEXT=<set> (rotation in progress)"
    return "$ok"
  fi
  say "gateway configuration"
  runuser -u git -- /opt/forkmesh-ssh/ssh-gateway-entrypoint check || ok=1
  say "broker"
  systemctl is-active --quiet forkmesh-ssh-auth.service && echo active || { echo inactive; ok=1; }
  say "sshd"
  sshd -t && echo "sshd -t: ok" || ok=1
  sshd -T -C user=git,host=check,addr=192.0.2.1 | grep -E '^(authorizedkeyscommand|permittty|disableforwarding|authenticationmethods) ' || ok=1
  echo "root (unchanged by this drop-in):"
  sshd -T -C user=root,host=check,addr=192.0.2.1 | grep -E '^(permitrootlogin|passwordauthentication|pubkeyauthentication) '
  say "control plane"
  probe_control_plane || ok=1
  if [ "$ROLE" = front ]; then
    say "abuse controls"
    sshd -T | grep -E '^(maxstartups|persourcemaxstartups|persourcenetblocksize|logingracetime) ' || true
    if command -v fail2ban-client >/dev/null; then
      fail2ban-client status forkmesh-git-ssh 2>/dev/null | grep -E 'Currently (failed|banned)' \
        || echo "the fail2ban jail forkmesh-git-ssh is not running"
    else
      echo "fail2ban is not installed (apt install fail2ban): failed logins are not banned"
    fi
    return "$ok"
  fi
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
  [ "$ROLE" = front ] || cp -a "$APP_ENV" "$backup/app.env"
  if [ "$ROLE" = control ]; then
    say "unpublishing SSH addresses"
    set_app_env
    systemctl restart "$APP_UNIT"
    rm -f "$ETC/role"
    say "removed; backup in $backup. Uninstall the remote front as well."
    return 0
  fi
  [ -f "$DROPIN" ] && cp -a "$DROPIN" "$backup/"
  [ -f "$LIMITS_DROPIN" ] && cp -a "$LIMITS_DROPIN" "$backup/"
  say "removing the sshd drop-ins"
  rm -f "$DROPIN" "$LIMITS_DROPIN"
  reload_sshd || die "sshd -t failed after removing $DROPIN; restore from $backup"
  if [ -f "$JAIL" ]; then
    rm -f "$JAIL"
    if command -v fail2ban-client >/dev/null; then
      fail2ban-client reload >/dev/null 2>&1 || true
    fi
  fi
  say "stopping the broker"
  systemctl disable --now forkmesh-ssh-auth.service 2>/dev/null || true
  rm -f "$BROKER_UNIT" "$TMPFILES"
  systemctl daemon-reload
  if [ "$ROLE" = front ]; then
    echo "on the instance, run --uninstall as well to stop publishing SSH addresses"
  else
    say "unpublishing SSH addresses"
    set_app_env
    systemctl restart "$APP_UNIT"
  fi
  rm -f "$ETC/role"
  # Accounts and files stay (locked, unreachable without the drop-in) so a
  # reinstall reuses the same token; delete $ETC to rotate it.
  usermod -L git 2>/dev/null || true
  say "removed; backup in $backup"
}

# Rotation: the instance accepts SSH_GATEWAY_TOKEN and SSH_GATEWAY_TOKEN_NEXT
# alike, so the gateway host can switch at any moment in between.
finish_rotation() {
  [ -s "$ETC/ssh-gateway.token.next" ] || die "no rotation in progress"
  local next
  next="$(cat "$ETC/ssh-gateway.token.next")"
  say "promoting the next token; the previous one stops working"
  # Drops SSH_GATEWAY_TOKEN= and SSH_GATEWAY_TOKEN_NEXT= alike.
  update_app_env SSH_GATEWAY_TOKEN "SSH_GATEWAY_TOKEN=$next"
  next=""
  mv "$ETC/ssh-gateway.token.next" "$ETC/ssh-gateway.token"
  own_token "$ETC/ssh-gateway.token"
  systemctl restart "$APP_UNIT"
  say "rotation finished"
}

rotate_token() {
  [ "$ROLE" != front ] || die "rotate on the instance; on the front, rerun --remote-front with --token-file -"
  [ -s "$ETC/ssh-gateway.token" ] || die "no gateway token in $ETC; install first"
  [ -s "$ETC/ssh-gateway.token.next" ] || new_token "$ETC/ssh-gateway.token.next"
  own_token "$ETC/ssh-gateway.token.next"
  say "accepting the next token as well"
  local next
  next="$(cat "$ETC/ssh-gateway.token.next")"
  update_app_env SSH_GATEWAY_TOKEN_NEXT= "SSH_GATEWAY_TOKEN_NEXT=$next"
  next=""
  systemctl restart "$APP_UNIT"
  if [ "$ROLE" = local ]; then
    say "moving this host's broker to the next token"
    cp -p "$ETC/ssh-gateway.token.next" "$ETC/ssh-gateway.token.switch"
    mv "$ETC/ssh-gateway.token.switch" "$ETC/ssh-gateway.token"
    systemctl restart forkmesh-ssh-auth.service
    finish_rotation
    return 0
  fi
  cat <<EOF
The instance now accepts both tokens. Move the remote front to the new one:
  sudo cat $ETC/ssh-gateway.token.next |
    ssh root@<front> 'bash <checkout>/tools/install_ssh_relay.sh --remote-front \\
      --api-origin <origin> --host <ssh name> --token-file -'
then retire the old token here:
  sudo bash $SRC/tools/install_ssh_relay.sh --finish-rotation
EOF
}

[ "$MODE" = check ] && { check; exit $?; }
[ "$MODE" = uninstall ] && { uninstall; exit 0; }
[ "$MODE" = rotate ] && { rotate_token; exit 0; }
[ "$MODE" = finish-rotation ] && { finish_rotation; exit 0; }

[ -n "$HOST" ] || die "--host is required (a DNS-only name for SSH, e.g. ssh.example.org)"
[[ "$PORT" =~ ^[0-9]{1,5}$ ]] && [ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || die "invalid --port"
[[ "$HOST" =~ ^[A-Za-z0-9.-]+$ ]] || die "invalid --host"

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
backup="$BACKUP_ROOT/$stamp"

if [ "$ROLE" = control ]; then
  # The gateway runs on a remote front: this host only issues the token and
  # publishes the SSH addresses. Nothing here listens for SSH Git.
  [ -z "$TOKEN_FILE" ] || die "--token-file belongs on the front host"
  install -d -m 0700 "$BACKUP_ROOT" "$backup"
  cp -a "$APP_ENV" "$backup/app.env"
  install -d -m 0755 -o root -g root "$ETC"
  if [ ! -s "$ETC/ssh-gateway.token" ]; then
    new_token "$ETC/ssh-gateway.token"
    echo "generated a new gateway token (not printed)"
  fi
  own_token "$ETC/ssh-gateway.token"
  echo control > "$ETC/role"
  say "publishing SSH addresses in $APP_ENV"
  publish_app_env
  cat <<EOF

Control plane ready; backup in $backup. Next, on the front host (with a
source checkout, e.g. /opt/forkmesh-src), pipe the token over SSH so it is
never written anywhere else:
  sudo cat $ETC/ssh-gateway.token |
    ssh root@<front> 'bash /opt/forkmesh-src/tools/install_ssh_relay.sh \\
      --remote-front --api-origin $(env_value PUBLIC_BASE_URL) --host $HOST --token-file -'
Then a DNS-only record $HOST -> the front's address (never this server's).
Optional: pin the front with SSH_GATEWAY_ALLOWED_ADDRESSES=<front address>
in $APP_ENV, then restart $APP_UNIT.
EOF
  exit 0
fi

incoming=""
if [ "$ROLE" = front ]; then
  [ -n "$ORIGIN" ] || die "--remote-front needs --api-origin https://<public name>"
  if [ -z "$TOKEN_FILE" ] && [ ! -s "$ETC/ssh-gateway.token" ]; then
    die "--token-file is required on a new front (the instance's $ETC/ssh-gateway.token, or - for stdin)"
  fi
  if [ -n "$TOKEN_FILE" ]; then
    if [ "$TOKEN_FILE" = - ]; then
      incoming="$(head -c 4096 | tr -d '[:space:]')"
    else
      [ -r "$TOKEN_FILE" ] || die "cannot read $TOKEN_FILE"
      incoming="$(head -c 4096 "$TOKEN_FILE" | tr -d '[:space:]')"
    fi
    [[ "$incoming" =~ ^[A-Za-z0-9_-]{32,512}$ ]] || die "the token is not 32-512 base64url characters"
  fi
else
  [ -z "$TOKEN_FILE" ] || die "--token-file is for --remote-front (here the token is generated)"
  [ -n "$ORIGIN" ] || ORIGIN="$(env_value PUBLIC_BASE_URL)"
fi
valid_origin
[[ "$MAX_REQUEST_MB" =~ ^[0-9]{1,5}$ ]] || die "invalid --max-request-mb"
command -v sshd >/dev/null || die "sshd is not installed"
command -v python3 >/dev/null || die "python3 is not installed"
PYTHON3="$(command -v python3)"
[ "$PYTHON3" = /usr/bin/python3 ] || die "expected /usr/bin/python3"

install -d -m 0700 "$BACKUP_ROOT" "$backup"
[ "$ROLE" = front ] || cp -a "$APP_ENV" "$backup/app.env"
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
# SSH_CONNECTION's first field (the client address) is passed on for the
# per-client session cap; nothing else of the client's environment is.
cat > "$OPT/ssh-gateway-entrypoint.new" <<'SH'
#!/bin/sh
original_command=${SSH_ORIGINAL_COMMAND-}
git_protocol=${GIT_PROTOCOL-}
client_address=${SSH_CONNECTION%% *}
exec /usr/bin/env -i \
  PATH=/usr/bin:/bin \
  LANG=C \
  LC_ALL=C \
  SSH_ORIGINAL_COMMAND="$original_command" \
  GIT_PROTOCOL="$git_protocol" \
  SSH_CLIENT_ADDRESS="$client_address" \
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
if [ -n "$incoming" ]; then
  umask 077
  printf '%s\n' "$incoming" > "$ETC/ssh-gateway.token.new"
  umask 022
  chmod 0600 "$ETC/ssh-gateway.token.new"
  mv "$ETC/ssh-gateway.token.new" "$ETC/ssh-gateway.token"
  incoming=""
  echo "installed the gateway token from the instance (not printed)"
elif [ ! -s "$ETC/ssh-gateway.token" ]; then
  new_token "$ETC/ssh-gateway.token"
  echo "generated a new gateway token (not printed)"
fi
own_token "$ETC/ssh-gateway.token"
echo "$ROLE" > "$ETC/role"
write_configs "$ETC"

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

if [ "$ROLE" = local ]; then
  say "publishing SSH addresses in $APP_ENV"
  publish_app_env
fi

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

if [ "$ROLE" = front ]; then
  # The front's SSH port is public, so it sees every scanner. These are
  # global sshd settings, which is fine on a host that exists for this.
  say "sshd: connection limits for a public front"
  cat > "$LIMITS_DROPIN" <<'CONF'
# ForkMesh SSH front (tools/install_ssh_relay.sh --remote-front).
# Unauthenticated connections: refuse some beyond 10, all beyond 60, and at
# most 4 at once from one address (or IPv6 /64).
MaxStartups 10:30:60
PerSourceMaxStartups 4
PerSourceNetBlockSize 32:64
LoginGraceTime 20
CONF
  chmod 0644 "$LIMITS_DROPIN"
  if ! sshd -t 2>/dev/null; then
    # An older OpenSSH without the per-source keywords (before 8.5).
    sed -i '/^PerSource/d' "$LIMITS_DROPIN"
    sshd -t || { rm -f "$LIMITS_DROPIN"; die "sshd -t rejected $LIMITS_DROPIN; removed it"; }
  fi
  say "fail2ban jail for failed git logins"
  install -d -m 0755 /etc/fail2ban/jail.d
  cat > "$JAIL" <<CONF
# ForkMesh SSH front (tools/install_ssh_relay.sh --remote-front): ban an
# address after repeated failed or abandoned SSH logins (unregistered keys,
# scanners). A registered key never fails here.
[forkmesh-git-ssh]
enabled  = true
filter   = sshd[mode=aggressive]
backend  = systemd
port     = $PORT
maxretry = 10
findtime = 10m
bantime  = 1h
CONF
  chmod 0644 "$JAIL"
  if command -v fail2ban-client >/dev/null; then
    fail2ban-client reload >/dev/null 2>&1 || systemctl restart fail2ban || true
  else
    echo "fail2ban is not installed; the jail is written and starts once it is (apt install fail2ban)"
  fi
fi
systemctl reload ssh 2>/dev/null || systemctl reload sshd

say "done"
check || true
if [ "$ROLE" = front ]; then
  cat <<EOF

SSH front installed. It reaches $ORIGIN only through public DNS and HTTPS.
  * DNS: a DNS-only (not proxied) record $HOST -> this front's address. The
    instance's own address appears nowhere in this host's configuration.
  * Pushes over $MAX_REQUEST_MB MB (0 = no cap) stop here with an explanation.
  * Firewall: allow $PORT/tcp in; outbound HTTPS only to the CDN.
  * Remove with: bash $SRC/tools/install_ssh_relay.sh --uninstall
EOF
else
  cat <<EOF

SSH relay installed. Next:
  * DNS: a DNS-only (not proxied) record $HOST -> this server's address.
    That publishes this server's address; to keep it private behind a CDN,
    run the gateway on a separate front host instead (--remote-front).
  * Users add a public key at $ORIGIN/dashboard/settings/ssh-keys, then:
      ssh -T git@$HOST                       # prints a greeting
      git clone git@$HOST:<owner>/<repository>.git
  * Remove with: bash $SRC/tools/install_ssh_relay.sh --uninstall
EOF
fi
