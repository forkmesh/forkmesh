#!/usr/bin/env bash
{
set -euo pipefail
umask 022

RUNNER_INSTALLER_VERSION="1"
TOKEN=""
NAME=""
APP_URL="https://app.forkmesh.com"
PROVIDERS="claude"
BINARY=""
INSTALL_SH_URL="${FORKMESH_INSTALL_SH_URL:-https://forkmesh.com/install.sh}"
PREFIX="${FORKMESH_RUNNER_PREFIX:-}"
TEST_MODE="${FORKMESH_RUNNER_TEST:-0}"
JOB_USER="forkmesh-job"

say() { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
warn() { printf 'Warning: %s\n' "$*" >&2; }
die() { printf 'Error: %s\n' "$1" >&2; exit "${2:-1}"; }

usage() {
  cat <<'EOF'
ForkMesh runner installer: turns this Linux machine into a personal agent runner.

  curl -fsSL https://forkmesh.com/install-runner.sh | sudo bash -s -- \
    --token <one-time token> --name <runner name> [--app https://app.forkmesh.com]

Options:
  --token T        one-time setup token from the Runners page (30 minutes, single use)
  --name N         runner name the token was made for
  --app URL        ForkMesh app origin (default https://app.forkmesh.com)
  --providers P    agent CLIs to install: claude, codex, claude,codex or none (default claude)
  --binary PATH    use this forkmesh-mirror-node binary instead of the signed release

What it installs: /usr/local/bin/forkmesh-mirror-node (run as a runner, it serves no
repositories), the forkmesh-runner service, the unprivileged forkmesh-job user that
agent jobs run as, firewall rules that keep jobs off private networks, and the chosen
agent CLIs. It keeps any existing ForkMesh data on this machine and deletes nothing.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --token) TOKEN="${2:-}"; shift 2 ;;
    --token=*) TOKEN="${1#*=}"; shift ;;
    --name) NAME="${2:-}"; shift 2 ;;
    --name=*) NAME="${1#*=}"; shift ;;
    --app) APP_URL="${2:-}"; shift 2 ;;
    --app=*) APP_URL="${1#*=}"; shift ;;
    --providers) PROVIDERS="${2:-}"; shift 2 ;;
    --providers=*) PROVIDERS="${1#*=}"; shift ;;
    --binary) BINARY="${2:-}"; shift 2 ;;
    --binary=*) BINARY="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown option: $1 (see --help)" 2 ;;
  esac
done

[[ "$TOKEN" =~ ^fmrt_[0-9a-f]{64}$ ]] ||
  die "--token must be the one-time token shown on the Runners page." 2
[[ "$NAME" =~ ^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$ ]] ||
  die "--name must be lowercase letters, digits and dashes, starting with a letter." 2
APP_URL="${APP_URL%/}"
if [[ ! "$APP_URL" =~ ^https://[A-Za-z0-9.-]+(:[0-9]{1,5})?$ ]]; then
  if [ "$TEST_MODE" = "1" ] && [[ "$APP_URL" =~ ^http://127\.0\.0\.1:[0-9]{1,5}$ ]]; then
    :
  else
    die "--app must be an https:// origin." 2
  fi
fi
case "$PROVIDERS" in
  claude|codex|claude,codex|codex,claude|none) ;;
  *) die "--providers must be claude, codex, claude,codex or none." 2 ;;
esac
if [ -n "$BINARY" ] && [ ! -f "$BINARY" ]; then
  die "--binary $BINARY does not exist." 2
fi

ETC="$PREFIX/etc/forkmesh-runner"
STATE="$PREFIX/var/lib/forkmesh-runner"
JOB_HOME="$PREFIX/var/lib/$JOB_USER"
BIN="$PREFIX/usr/local/bin/forkmesh-mirror-node"
UNIT_DIR="$PREFIX/etc/systemd/system"
KEY="$STATE/identity/ed25519.pem"
IDENTITY="$STATE/gateway/config.json"
CONFIG="$ETC/runner.json"
NFT="$ETC/jobs.nft"

say "ForkMesh runner installer v$RUNNER_INSTALLER_VERSION: runner $NAME for $APP_URL"

require_system() {
  [ "$TEST_MODE" = "1" ] && return 0
  [ "$(uname -s)" = "Linux" ] || die "Runners need Linux with systemd."
  [ "$(id -u)" = "0" ] || die "Run this as root (the command pipes into sudo bash)."
  command -v systemctl >/dev/null 2>&1 || die "Runners need systemd (systemctl was not found)."
}

pm_install() {
  if command -v apt-get >/dev/null 2>&1; then
    DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 update -qq </dev/null || true
    DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=600 install -y -qq --no-install-recommends "$@" </dev/null
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y -q "$@" </dev/null
  elif command -v yum >/dev/null 2>&1; then
    yum install -y -q "$@" </dev/null
  elif command -v zypper >/dev/null 2>&1; then
    zypper --non-interactive install "$@" </dev/null
  else
    return 1
  fi
}

install_packages() {
  [ "$TEST_MODE" = "1" ] && return 0
  step "Checking system packages"
  local missing=()
  command -v curl >/dev/null 2>&1 || missing+=(curl)
  command -v openssl >/dev/null 2>&1 || missing+=(openssl)
  command -v python3 >/dev/null 2>&1 || missing+=(python3)
  command -v git >/dev/null 2>&1 || missing+=(git)
  command -v nft >/dev/null 2>&1 || missing+=(nftables)
  [ -f /etc/ssl/certs/ca-certificates.crt ] || [ -d /etc/pki/tls/certs ] || missing+=(ca-certificates)
  if [ "${#missing[@]}" -gt 0 ]; then
    say "Installing: ${missing[*]}"
    pm_install "${missing[@]}" || warn "Could not install ${missing[*]} with the package manager."
  fi
  for tool in curl openssl python3 git; do
    command -v "$tool" >/dev/null 2>&1 || die "$tool is required and could not be installed."
  done
}

ensure_job_user() {
  if [ "$TEST_MODE" != "1" ]; then
    if ! id "$JOB_USER" >/dev/null 2>&1; then
      useradd --system --home-dir "$JOB_HOME" --create-home --shell /usr/sbin/nologin "$JOB_USER"
    fi
    install -d -o "$JOB_USER" -g "$JOB_USER" -m 0700 "$JOB_HOME" "$JOB_HOME/.claude" "$JOB_HOME/.codex"
  else
    install -d -m 0700 "$JOB_HOME" "$JOB_HOME/.claude" "$JOB_HOME/.codex"
  fi
}

ensure_dirs() {
  install -d -m 0700 "$ETC"
  install -d -m 0711 "$STATE" "$STATE/work"
  install -d -m 0700 "$STATE/identity" "$STATE/gateway"
  install -d -m 1777 "$STATE/tmp"
  install -d -m 0755 "$(dirname "$BIN")" "$UNIT_DIR"
}

ensure_binary() {
  step "Installing forkmesh-mirror-node"
  if [ -n "$BINARY" ]; then
    install -m 0755 "$BINARY" "$BIN"
    say "Installed $BIN from $BINARY"
    return 0
  fi
  if [ -x "$BIN" ]; then
    say "Keeping the installed $BIN"
    return 0
  fi
  [ "$TEST_MODE" = "1" ] && die "No --binary given in test mode."
  local stage
  stage="$(mktemp -d /tmp/forkmesh-runner-stage.XXXXXX)"
  curl -fsSL --retry 3 "$INSTALL_SH_URL" -o "$stage/install.sh" ||
    die "Could not download $INSTALL_SH_URL."
  say "Fetching the signed ForkMesh release with install.sh (verified the same way as a node install)"
  if ! HOME=/root FORKMESH_NO_LAUNCH=1 FORKMESH_BIN_DIR="$stage/bin" bash "$stage/install.sh" </dev/null; then
    die "install.sh could not fetch the signed release. Nothing else was changed."
  fi
  [ -x "$stage/bin/forkmesh-mirror-node" ] ||
    die "The signed release has no forkmesh-mirror-node for this platform."
  install -m 0755 "$stage/bin/forkmesh-mirror-node" "$BIN"
  rm -f "$stage/install.sh" "$stage/bin/forkmesh-mirror-node" "$stage/bin/forkmesh"
  rmdir "$stage/bin" "$stage" 2>/dev/null || say "Left the download folder $stage (remove it when you like)."
  say "Installed $BIN"
}

b64url() { base64 | tr '+/' '-_' | tr -d '=\n'; }

ensure_identity() {
  step "Runner identity"
  if [ -s "$KEY" ]; then
    say "Reusing the runner key at $KEY"
  else
    local old_umask
    old_umask="$(umask)"
    umask 077
    openssl genpkey -algorithm ed25519 -out "$KEY.new" 2>/dev/null ||
      die "OpenSSL 1.1.1 or newer is required (Ed25519)."
    mv "$KEY.new" "$KEY"
    umask "$old_umask"
    say "Made a new runner key at $KEY"
  fi
  chmod 0600 "$KEY"
  PUBLIC_KEY="$(openssl pkey -in "$KEY" -pubout -outform DER | tail -c 32 | b64url)"
  [[ "$PUBLIC_KEY" =~ ^[A-Za-z0-9_-]{43}$ ]] || die "Could not read the runner's public key."
}

register_runner() {
  step "Registering $NAME with ForkMesh"
  local ts digest message_file signature body answer
  ts="$(python3 -c 'import time; print(int(time.time() * 1000))')"
  digest="$(printf '%s' "$TOKEN" | openssl dgst -sha256 -r | cut -d' ' -f1)"
  message_file="$(mktemp)"
  printf 'forkmesh-runner-register-token-v1\n%s\n%s\n%s\n%s' "$NAME" "$PUBLIC_KEY" "$digest" "$ts" >"$message_file"
  signature="$(openssl pkeyutl -sign -rawin -inkey "$KEY" -in "$message_file" | b64url)"
  rm -f "$message_file"
  body="$(FM_TOKEN="$TOKEN" FM_NAME="$NAME" FM_KEY="$PUBLIC_KEY" FM_SIG="$signature" FM_TS="$ts" python3 -c '
import json, os
print(json.dumps({"token": os.environ["FM_TOKEN"], "name": os.environ["FM_NAME"],
                  "publicKey": os.environ["FM_KEY"], "proof": os.environ["FM_SIG"],
                  "ts": int(os.environ["FM_TS"])}))')"
  answer="$(printf '%s' "$body" | curl -sS --max-time 30 -H 'content-type: application/json' \
    --data-binary @- -w '\n%{http_code}' "$APP_URL/api/runners/register-token")" ||
    die "Could not reach $APP_URL."
  body=""
  local status="${answer##*$'\n'}"
  answer="${answer%$'\n'*}"
  if [ "$status" != "200" ] && [ "$status" != "201" ]; then
    local message
    message="$(printf '%s' "$answer" | python3 -c 'import json,sys
try: d=json.load(sys.stdin)
except Exception: d={}
print(d.get("message") or d.get("error") or "")' 2>/dev/null || true)"
    die "ForkMesh refused the registration (HTTP $status): ${message:-no details}"
  fi
  OWNERS_JSON="$(printf '%s' "$answer" | python3 -c 'import json,sys
d=json.load(sys.stdin)
owners=[o for o in d["runner"]["allowedOwners"] if isinstance(o,str)]
print(json.dumps(owners))')"
  say "Registered $NAME for $(printf '%s' "$OWNERS_JSON" | tr -d '[]"')."
}

write_config() {
  step "Writing the runner configuration"
  FM_NAME="$NAME" FM_KEY="$PUBLIC_KEY" FM_IDENTITY="$IDENTITY" FM_CONFIG="$CONFIG" \
  FM_KEYFILE="$KEY" FM_APP="$APP_URL" FM_OWNERS="$OWNERS_JSON" FM_WORK="$STATE/work" \
  FM_JOB_USER="$JOB_USER" FM_TEST="$TEST_MODE" FM_PREFIX="$PREFIX" python3 - <<'PY'
import json, os
def write(path, doc):
    tmp = path + ".new"
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as handle:
        handle.write(json.dumps(doc, indent=2) + "\n")
    os.replace(tmp, path)
prefix = os.environ["FM_PREFIX"]
def live(path):
    return path[len(prefix):] if prefix and path.startswith(prefix) else path
write(os.environ["FM_IDENTITY"], {
    "schemaVersion": 1,
    "node": {"name": os.environ["FM_NAME"], "publicKey": os.environ["FM_KEY"]},
    "repositories": [],
})
runner = {
    "appUrl": os.environ["FM_APP"],
    "maxConcurrentRuns": 1,
    "workRoot": live(os.environ["FM_WORK"]),
    "allowedOwners": json.loads(os.environ["FM_OWNERS"]),
}
if os.environ["FM_TEST"] != "1":
    runner["jobUser"] = os.environ["FM_JOB_USER"]
write(os.environ["FM_CONFIG"], {
    "schemaVersion": 1,
    "role": "runner",
    "listen": "127.0.0.1:8795",
    "gatewayProbe": "127.0.0.1:8796",
    "gatewayConfig": live(os.environ["FM_IDENTITY"]),
    "identityKey": live(os.environ["FM_KEYFILE"]),
    "runner": runner,
})
PY
  say "Wrote $CONFIG"
}

write_units() {
  step "Installing the forkmesh-runner service"
  cat >"$UNIT_DIR/forkmesh-runner.service" <<EOF
[Unit]
Description=ForkMesh runner $NAME (agent jobs, docs/RUNNERS.md)
Wants=network-online.target forkmesh-runner-firewall.service
After=network-online.target forkmesh-runner-firewall.service

[Service]
Type=simple
User=root
Group=root
UMask=0077
Environment=TZ=UTC
Environment=HOME=/var/lib/forkmesh-runner
Environment=TMPDIR=/var/lib/forkmesh-runner/tmp
Environment=PATH=/usr/local/bin:/usr/bin:/bin
ExecStartPre=/usr/local/bin/forkmesh-mirror-node --config /etc/forkmesh-runner/runner.json --check
ExecStart=/usr/local/bin/forkmesh-mirror-node --config /etc/forkmesh-runner/runner.json
Restart=on-failure
RestartSec=5s
TimeoutStopSec=15s
MemoryHigh=150M
MemoryMax=250M
NoNewPrivileges=true
PrivateDevices=true
PrivateTmp=false
BindPaths=/var/lib/forkmesh-runner/tmp:/tmp /var/lib/forkmesh-runner/tmp:/var/tmp
ProtectClock=true
ProtectControlGroups=true
ProtectHome=true
ProtectHostname=true
ProtectKernelLogs=true
ProtectKernelModules=true
ProtectKernelTunables=true
ProtectSystem=strict
ReadWritePaths=/var/lib/forkmesh-runner /var/lib/$JOB_USER
IPAddressDeny=169.254.0.0/16
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
RestrictNamespaces=true
RestrictRealtime=true
RestrictSUIDSGID=true
LockPersonality=true
CapabilityBoundingSet=CAP_CHOWN CAP_DAC_OVERRIDE CAP_FOWNER CAP_KILL CAP_SETUID CAP_SETGID
AmbientCapabilities=CAP_SETUID CAP_SETGID
SystemCallArchitectures=native
TasksMax=128
LimitNOFILE=8192

[Install]
WantedBy=multi-user.target
EOF
  cat >"$NFT" <<EOF
table inet forkmesh_runner
delete table inet forkmesh_runner
table inet forkmesh_runner {
	chain output {
		type filter hook output priority filter; policy accept;
		meta skuid "$JOB_USER" ip daddr 127.0.0.53 meta l4proto { tcp, udp } th dport 53 accept
		meta skuid "$JOB_USER" ip daddr { 169.254.0.0/16, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 100.64.0.0/10, 127.0.0.0/8, 0.0.0.0/8, 224.0.0.0/4, 240.0.0.0/4 } counter reject
		meta skuid "$JOB_USER" ip6 daddr { ::1, ::ffff:0:0/96, fc00::/7, fe80::/10, ff00::/8 } counter reject
	}
}
EOF
  chmod 0600 "$NFT"
  cat >"$UNIT_DIR/forkmesh-runner-firewall.service" <<'EOF'
[Unit]
Description=ForkMesh runner: keep agent jobs off private networks
After=network-pre.target
Before=forkmesh-runner.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/sbin/nft -f /etc/forkmesh-runner/jobs.nft
ExecStop=/usr/sbin/nft delete table inet forkmesh_runner

[Install]
WantedBy=multi-user.target
EOF
}

install_providers() {
  [ "$PROVIDERS" = "none" ] && return 0
  [ "$TEST_MODE" = "1" ] && return 0
  step "Agent CLIs ($PROVIDERS)"
  if ! command -v npm >/dev/null 2>&1; then
    say "Installing Node.js and npm"
    pm_install nodejs npm || warn "Could not install Node.js; install the agent CLIs yourself."
  fi
  command -v npm >/dev/null 2>&1 || return 0
  case ",$PROVIDERS," in
    *,claude,*)
      if command -v claude >/dev/null 2>&1; then say "Claude Code is installed"
      else npm install -g --no-fund --no-audit @anthropic-ai/claude-code </dev/null || warn "Could not install Claude Code."; fi ;;
  esac
  case ",$PROVIDERS," in
    *,codex,*)
      if command -v codex >/dev/null 2>&1; then say "Codex is installed"
      else npm install -g --no-fund --no-audit @openai/codex </dev/null || warn "Could not install Codex."; fi ;;
  esac
}

check_and_start() {
  "$BIN" --config "$CONFIG" --check >/dev/null 2>&1 || {
    [ "$TEST_MODE" = "1" ] && return 0
    die "The runner configuration did not pass forkmesh-mirror-node --check. Run it by hand to see why."
  }
  [ "$TEST_MODE" = "1" ] && return 0
  step "Starting the runner"
  systemctl daemon-reload
  if command -v nft >/dev/null 2>&1; then
    systemctl enable --now forkmesh-runner-firewall.service ||
      warn "The job firewall did not load; agent jobs can reach this machine's private network."
  else
    warn "nftables is missing, so agent jobs are not kept off this machine's private network."
  fi
  systemctl enable forkmesh-runner.service
  systemctl restart forkmesh-runner.service
  local tick
  for tick in 1 2 3 4 5 6; do
    sleep 2
    if systemctl is-active --quiet forkmesh-runner.service; then
      say "The runner is running. Logs: journalctl -u forkmesh-runner.service"
      return 0
    fi
  done
  die "forkmesh-runner.service did not start. Logs: journalctl -u forkmesh-runner.service"
}

require_system
install_packages
ensure_dirs
ensure_job_user
ensure_binary
ensure_identity
register_runner
write_config
write_units
install_providers
check_and_start

say ""
say "Done. $NAME is registered as your personal runner."
say "Next: open https://forkmesh.com/dashboard/runners and press Connect Claude (or Codex)"
say "to sign the agent CLI in. The login stays on this machine."
exit 0
}
