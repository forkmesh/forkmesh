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
INSTANCE=""
INSTANCES=""
FOUND=""
INSTANCE_OWNER=""
INSTANCE_NAME=""
INSTANCE_JOB_USER=""
INSTANCE_WORK=""
INSTANCE_LISTEN=""
OWNER=""
OWNERS_JSON=""
JOB_USER=""
JOB_HOME=""
JOB_COMMENT=""
WORK=""
LISTEN=""
PROBE=""
SIGN_KEY=""
PUBLIC_KEY=""
ETC=""
STATE=""
KEY=""
IDENTITY=""
CONFIG=""
UNIT=""
RUNNER_LABEL=""
MAC_LOG=""
PREREGISTERED="0"
PREREG_OWNER=""
JOB_USER_OVERRIDE=""
ENABLE_ACTIONS="0"

say() { printf '%s\n' "$*"; }
step() { printf '\n==> %s\n' "$*"; }
warn() { printf 'Warning: %s\n' "$*" >&2; }
die() { printf 'Error: %s\n' "$1" >&2; exit "${2:-1}"; }

usage() {
  cat <<'EOF'
ForkMesh runner installer: turns this Linux or macOS machine into a personal agent runner.

  curl -fsSL https://forkmesh.com/install-runner.sh | sudo bash -s -- \
    --token <one-time token> --name <runner name> [--app https://app.forkmesh.com]

Options:
  --token T        one-time setup token from the Runners page (30 minutes, single use)
  --name N         runner name the token was made for
  --app URL        ForkMesh app origin (default https://app.forkmesh.com)
  --providers P    agent CLIs to install: claude, codex, claude,codex or none (default claude)
  --binary PATH    use this forkmesh-mirror-node binary instead of the signed release
  --preregistered  install-instance.sh only: skip the token registration (the instance
                   registers the runner itself), lease Actions jobs, and print the
                   runner's public key as FM_RUNNER_PUBLIC_KEY=<key>; needs --owner and --binary
  --owner O        the one account the runner serves (--preregistered)
  --job-user U     job user name (--preregistered, default forkmesh-job)

What it installs: /usr/local/bin/forkmesh-mirror-node (run as a runner, it serves no
repositories), a forkmesh-runner@<account> service, an unprivileged job user of its own for
your ForkMesh account (fmjob-<account>) that agent jobs and their sign-ins live under,
firewall rules that keep jobs off private networks, and the chosen agent CLIs. The runner is
private to your account: nobody else can see it or run jobs on it. Several accounts can each
install a runner on the same machine; each gets its own service, folders, key and user, and
installing one leaves the others running. It deletes nothing.

On macOS the service is the com.forkmesh.runner.<account> launch daemon, the firewall is a pf
anchor, and forkmesh-mirror-node is built from source with Go (Homebrew installs Go,
OpenSSL 3 and Node.js when they are missing). It needs macOS 12 or newer and the
Xcode Command Line Tools.
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
    --preregistered) PREREGISTERED="1"; shift ;;
    --owner) PREREG_OWNER="${2:-}"; shift 2 ;;
    --owner=*) PREREG_OWNER="${1#*=}"; shift ;;
    --job-user) JOB_USER_OVERRIDE="${2:-}"; shift 2 ;;
    --job-user=*) JOB_USER_OVERRIDE="${1#*=}"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "Unknown option: $1 (see --help)" 2 ;;
  esac
done

if [ "$PREREGISTERED" = "1" ]; then
  [ -z "$TOKEN" ] || die "--preregistered takes no --token." 2
  [[ "$PREREG_OWNER" =~ ^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$ ]] ||
    die "--preregistered needs --owner, the account the runner serves." 2
  [ -n "$BINARY" ] || die "--preregistered needs --binary." 2
  [ -n "$JOB_USER_OVERRIDE" ] || JOB_USER_OVERRIDE="forkmesh-job"
  [[ "$JOB_USER_OVERRIDE" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] && [ "$JOB_USER_OVERRIDE" != "root" ] ||
    die "--job-user must be a plain user name." 2
  ENABLE_ACTIONS="1"
else
  [ -z "$PREREG_OWNER$JOB_USER_OVERRIDE" ] || die "--owner and --job-user need --preregistered." 2
  [[ "$TOKEN" =~ ^fmrt_[0-9a-f]{64}$ ]] ||
    die "--token must be the one-time token shown on the Runners page." 2
fi
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

PLATFORM="$(uname -s)"
if [ "$TEST_MODE" = "1" ] && [ -n "${FORKMESH_RUNNER_PLATFORM:-}" ]; then
  PLATFORM="$FORKMESH_RUNNER_PLATFORM"
fi
SOURCE_URL="${FORKMESH_RUNNER_SOURCE_URL:-https://forkmesh.com/forkmesh/forkmesh}"
OPENSSL="openssl"

if [ "$PLATFORM" = "Darwin" ]; then
  ETC_ROOT="$PREFIX/private/etc/forkmesh-runner"
  STATE_ROOT="$PREFIX/private/var/db/forkmesh-runner"
  JOB_BASE="$PREFIX/private/var/db"
  UNIT_DIR="$PREFIX/Library/LaunchDaemons"
  PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
else
  ETC_ROOT="$PREFIX/etc/forkmesh-runner"
  STATE_ROOT="$PREFIX/var/lib/forkmesh-runner"
  JOB_BASE="$PREFIX/var/lib"
  UNIT_DIR="$PREFIX/etc/systemd/system"
fi
BIN="$PREFIX/usr/local/bin/forkmesh-mirror-node"
NFT="$ETC_ROOT/jobs.nft"
NFT_LOCAL="$ETC_ROOT/jobs.d"
NFT_BASE="$ETC_ROOT/jobs-base.nft"
PF_RULES="$ETC_ROOT/jobs.pf"
PF_ANCHOR="com.apple/forkmesh-runner"
FIREWALL_LABEL="com.forkmesh.runner-firewall"
STAGED_KEY="$STATE_ROOT/pending-$NAME.pem"

say "ForkMesh runner installer v$RUNNER_INSTALLER_VERSION: runner $NAME for $APP_URL"

live_path() { printf '%s' "${1#"$PREFIX"}"; }

require_system() {
  [ "$TEST_MODE" = "1" ] && return 0
  case "$PLATFORM" in
    Linux)
      [ "$(id -u)" = "0" ] || die "Run this as root (the command pipes into sudo bash)."
      command -v systemctl >/dev/null 2>&1 || die "Runners need systemd (systemctl was not found)."
      ;;
    Darwin)
      [ "$(id -u)" = "0" ] || die "Run this as root (the command pipes into sudo bash)."
      local major
      major="$(sw_vers -productVersion 2>/dev/null | cut -d. -f1)"
      if ! [[ "$major" =~ ^[0-9]+$ ]] || [ "$major" -lt 12 ]; then
        die "Runners need macOS 12 or newer."
      fi
      for tool in launchctl dscl pfctl; do
        command -v "$tool" >/dev/null 2>&1 || die "$tool was not found; this does not look like macOS."
      done
      ;;
    *) die "Runners need Linux with systemd, or macOS." ;;
  esac
}

login_user() {
  [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != "root" ] || return 1
  printf '%s' "$SUDO_USER"
}

as_login_user() {
  local who
  who="$(login_user)" || return 1
  sudo -u "$who" -H "$@"
}

brew_install() {
  local brew
  brew="$(command -v brew 2>/dev/null)" || return 1
  as_login_user "$brew" install "$@" </dev/null
}

find_openssl() {
  local candidate
  for candidate in "$(command -v openssl 2>/dev/null || true)" \
      /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl; do
    [ -n "$candidate" ] && [ -x "$candidate" ] || continue
    if "$candidate" version 2>/dev/null | grep -Eq '^OpenSSL (1\.1\.1|[3-9]\.)'; then
      OPENSSL="$candidate"
      return 0
    fi
  done
  return 1
}

find_go() {
  local candidate
  for candidate in "$(command -v go 2>/dev/null || true)" \
      /opt/homebrew/bin/go /usr/local/go/bin/go /usr/local/bin/go; do
    if [ -n "$candidate" ] && [ -x "$candidate" ]; then
      printf '%s' "$candidate"
      return 0
    fi
  done
  return 1
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

install_packages_macos() {
  step "Checking system tools"
  if ! xcode-select -p >/dev/null 2>&1 || ! command -v git >/dev/null 2>&1 ||
     ! python3 -c '' >/dev/null 2>&1; then
    die "Runners need the Xcode Command Line Tools (git and python3). Install them with: xcode-select --install"
  fi
  command -v curl >/dev/null 2>&1 || die "curl is required."
  if ! find_openssl; then
    say "Installing OpenSSL 3 with Homebrew (the LibreSSL in macOS cannot sign with Ed25519)"
    brew_install openssl@3 || true
    find_openssl || die "OpenSSL 3 is required. Install it with: brew install openssl@3"
  fi
}

install_packages() {
  [ "$TEST_MODE" = "1" ] && return 0
  if [ "$PLATFORM" = "Darwin" ]; then
    install_packages_macos
    return 0
  fi
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

free_macos_id() {
  local used id
  used=" $( { dscl . -list /Users UniqueID; dscl . -list /Groups PrimaryGroupID; } | awk '{print $2}' | tr '\n' ' ') "
  for id in $(seq 400 -1 300); do
    case "$used" in
      *" $id "*) ;;
      *) printf '%s' "$id"; return 0 ;;
    esac
  done
  return 1
}

create_macos_job_user() {
  local id gid
  id="$(free_macos_id)" || die "No free system user id between 300 and 400 for $JOB_USER."
  gid="$(dscl . -read "/Groups/$JOB_USER" PrimaryGroupID 2>/dev/null | awk '{print $2}' || true)"
  if [ -z "$gid" ]; then
    gid="$id"
    dscl . -create "/Groups/$JOB_USER"
    dscl . -create "/Groups/$JOB_USER" PrimaryGroupID "$gid"
    dscl . -create "/Groups/$JOB_USER" RealName "$JOB_COMMENT"
    dscl . -create "/Groups/$JOB_USER" Password '*'
  fi
  dscl . -create "/Users/$JOB_USER"
  dscl . -create "/Users/$JOB_USER" UniqueID "$id"
  dscl . -create "/Users/$JOB_USER" PrimaryGroupID "$gid"
  dscl . -create "/Users/$JOB_USER" UserShell /usr/bin/false
  dscl . -create "/Users/$JOB_USER" NFSHomeDirectory "$(live_path "$JOB_HOME")"
  dscl . -create "/Users/$JOB_USER" RealName "$JOB_COMMENT"
  dscl . -create "/Users/$JOB_USER" Password '*'
  dscl . -create "/Users/$JOB_USER" IsHidden 1
  dscacheutil -flushcache >/dev/null 2>&1 || true
  say "Created the job user $JOB_USER (uid $id)"
}

job_user_for() {
  local account="$1" hash
  if [ "${#account}" -le 26 ]; then
    printf 'fmjob-%s' "$account"
    return 0
  fi
  hash="$(printf '%s' "$account" | "$OPENSSL" dgst -sha256 -r | cut -c1-6)"
  printf 'fmjob-%s-%s' "${account:0:19}" "$hash"
}

use_instance() {
  INSTANCE="$1"
  if [ -z "$INSTANCE" ]; then
    ETC="$ETC_ROOT"
    STATE="$STATE_ROOT"
    UNIT="forkmesh-runner.service"
    RUNNER_LABEL="com.forkmesh.runner"
    MAC_LOG="/private/var/log/forkmesh-runner.log"
  else
    ETC="$ETC_ROOT/accounts/$INSTANCE"
    STATE="$STATE_ROOT/accounts/$INSTANCE"
    UNIT="forkmesh-runner@$INSTANCE.service"
    RUNNER_LABEL="com.forkmesh.runner.$INSTANCE"
    MAC_LOG="/private/var/log/forkmesh-runner-$INSTANCE.log"
  fi
  KEY="$STATE/identity/ed25519.pem"
  IDENTITY="$STATE/gateway/config.json"
  CONFIG="$ETC/runner.json"
}

scan_instances() {
  python3 - "$PREFIX" "$(live_path "$ETC_ROOT")" <<'PY'
import json, os, re, sys
prefix, etc = sys.argv[1], sys.argv[2]
NAME = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
USER = re.compile(r"^[a-z_][a-z0-9_-]{0,31}$")
def load(path):
    try:
        with open(prefix + path) as handle:
            return json.load(handle)
    except Exception:
        return None
places = [("", etc + "/runner.json")]
try:
    accounts = sorted(os.listdir(prefix + etc + "/accounts"))
except OSError:
    accounts = []
places += [(a, etc + "/accounts/" + a + "/runner.json") for a in accounts if NAME.match(a)]
for instance, path in places:
    config = load(path)
    if not isinstance(config, dict) or not isinstance(config.get("runner"), dict):
        continue
    runner = config["runner"]
    owners = runner.get("allowedOwners") or []
    owner = owners[0] if len(owners) == 1 and isinstance(owners[0], str) else ""
    identity = load(str(config.get("gatewayConfig") or ""))
    node = identity.get("node") if isinstance(identity, dict) else None
    name = str(node.get("name") or "") if isinstance(node, dict) else ""
    user = str(runner.get("jobUser") or "")
    work = str(runner.get("workRoot") or "")
    print("\x1f".join((
        instance,
        owner if NAME.match(owner) else "",
        name if NAME.match(name) else "",
        user if USER.match(user) and user != "root" else "",
        work if work.startswith("/") and ".." not in work else "",
        str(config.get("listen") or ""),
        str(config.get("gatewayProbe") or ""))))
PY
}

instance_where() {
  local field="$1" value="$2" id owner name user work listen probe
  while IFS=$'\x1f' read -r id owner name user work listen probe; do
    case "$field" in
      name) [ "$name" = "$value" ] || continue ;;
      owner) [ "$owner" = "$value" ] || continue ;;
    esac
    FOUND=1
    INSTANCE_OWNER="$owner"
    INSTANCE_NAME="$name"
    INSTANCE_JOB_USER="$user"
    INSTANCE_WORK="$work"
    INSTANCE_LISTEN="$listen"
    use_instance "$id"
    return 0
  done <<<"$INSTANCES"
  return 1
}

find_instance() {
  INSTANCES="$(scan_instances)"
  if instance_where name "$NAME"; then
    say "Updating runner $NAME of ${INSTANCE_OWNER:-an unknown account} on this machine"
  fi
}

settle_instance() {
  if [ -z "$FOUND" ] && instance_where owner "$OWNER"; then
    say "$OWNER's runner ${INSTANCE_NAME:-} on this machine becomes $NAME"
  fi
  if [ -z "$FOUND" ]; then
    use_instance "$OWNER"
    say "Adding $NAME next to the other runners on this machine, as $OWNER's own instance"
  fi
  install -d -m 0700 "$ETC"
  install -d -m 0711 "$STATE"
  install -d -m 0700 "$STATE/identity" "$STATE/gateway"
  install -d -m 1777 "$STATE/tmp"
  if [ "$SIGN_KEY" = "$STAGED_KEY" ]; then
    if [ -s "$KEY" ]; then
      local aside="$KEY.${INSTANCE_NAME:-previous}"
      if [ -e "$aside" ]; then
        aside="$aside.$(date +%s)"
      fi
      mv "$KEY" "$aside"
      say "Kept the key of runner ${INSTANCE_NAME:-previous} at $aside; $NAME has its own key"
    fi
    mv "$STAGED_KEY" "$KEY"
    chmod 0600 "$KEY"
  fi
}

choose_job_user() {
  local id owner name user rest
  JOB_USER="$INSTANCE_JOB_USER"
  if [ -z "$JOB_USER" ]; then
    JOB_USER="$JOB_USER_OVERRIDE"
  fi
  if [ -z "$JOB_USER" ]; then
    JOB_USER="$(job_user_for "$OWNER")"
  fi
  [[ "$JOB_USER" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] || die "Could not derive a job user name for $OWNER."
  if [ -z "$INSTANCE" ]; then
    JOB_HOME="$JOB_BASE/$JOB_USER"
  else
    JOB_HOME="$STATE/home"
  fi
  WORK="$STATE/work"
  case "$INSTANCE_WORK" in
    "$(live_path "$STATE")"/*) WORK="$PREFIX$INSTANCE_WORK" ;;
  esac
  JOB_COMMENT="ForkMesh runner jobs for $OWNER"
  while IFS=$'\x1f' read -r id owner name user rest; do
    if [ "$id" != "$INSTANCE" ] && [ -n "$user" ] && [ "$user" = "$JOB_USER" ]; then
      die "The job user $JOB_USER already belongs to the runner of ${owner:-another account} on this machine."
    fi
  done <<<"$INSTANCES"
}

check_job_user_owner() {
  if [ -z "$INSTANCE_JOB_USER" ] && [ "$1" != "$JOB_COMMENT" ]; then
    die "A user named $JOB_USER already exists on this machine and is not the job user of $OWNER's runner. It was left untouched."
  fi
}

ensure_job_user() {
  step "Job user $JOB_USER for $OWNER"
  local comment
  if [ "$TEST_MODE" = "1" ]; then
    install -d -m 0700 "$JOB_HOME" "$JOB_HOME/.claude" "$JOB_HOME/.codex"
  elif [ "$PLATFORM" = "Darwin" ]; then
    if dscl . -read "/Users/$JOB_USER" UniqueID >/dev/null 2>&1; then
      comment="$(dscl . -read "/Users/$JOB_USER" RealName 2>/dev/null | sed '1s/^RealName://' | tr '\n' ' ' | sed 's/^ *//; s/ *$//')"
      check_job_user_owner "$comment"
    else
      create_macos_job_user
    fi
    install -d -o "$JOB_USER" -g "$JOB_USER" -m 0700 "$JOB_HOME" "$JOB_HOME/.claude" "$JOB_HOME/.codex"
  else
    if id "$JOB_USER" >/dev/null 2>&1; then
      comment="$(getent passwd "$JOB_USER" | cut -d: -f5)"
      check_job_user_owner "$comment"
    else
      useradd --system --comment "$JOB_COMMENT" --home-dir "$(live_path "$JOB_HOME")" --create-home \
        --shell /usr/sbin/nologin "$JOB_USER"
    fi
    install -d -o "$JOB_USER" -g "$JOB_USER" -m 0700 "$JOB_HOME" "$JOB_HOME/.claude" "$JOB_HOME/.codex"
  fi
  install -d -m 0711 "$WORK"
  say "Agent jobs for $OWNER run as $JOB_USER (home $(live_path "$JOB_HOME"), work $(live_path "$WORK"))"
}

pick_listen() {
  local ports
  ports="$(printf '%s\n' "$INSTANCES" | python3 -c '
import socket, sys
mine, listen = sys.argv[1], sys.argv[2]
def port(value):
    try:
        return int(str(value).rsplit(":", 1)[1])
    except (IndexError, ValueError):
        return 0
used = set()
for line in sys.stdin:
    parts = line.rstrip("\n").split("\x1f")
    if len(parts) < 7 or parts[0] == mine:
        continue
    used.update(port(value) for value in parts[5:7])
def free(number):
    sock = socket.socket()
    try:
        sock.bind(("127.0.0.1", number))
        return True
    except OSError:
        return False
    finally:
        sock.close()
own = port(listen)
if own and own not in used and own + 1 not in used:
    print(own, own + 1)
    sys.exit(0)
for number in range(8795, 8995, 2):
    if number not in used and number + 1 not in used and free(number) and free(number + 1):
        print(number, number + 1)
        sys.exit(0)
sys.exit(1)' "$INSTANCE" "$INSTANCE_LISTEN")" ||
    die "No free loopback port pair between 8795 and 8995 for the runner."
  LISTEN="127.0.0.1:${ports% *}"
  PROBE="127.0.0.1:${ports#* }"
}

job_users() {
  local id owner name user rest
  while IFS=$'\x1f' read -r id owner name user rest; do
    if [ -z "$user" ]; then
      continue
    fi
    if [ "$TEST_MODE" != "1" ] && ! id "$user" >/dev/null 2>&1; then
      warn "The job user $user of ${owner:-a runner} no longer exists; the job firewall leaves it out."
      continue
    fi
    printf '%s\n' "$user"
  done <<<"$INSTANCES"
}

ensure_base_dirs() {
  install -d -m 0700 "$ETC_ROOT" "$ETC_ROOT/accounts"
  install -d -m 0711 "$STATE_ROOT" "$STATE_ROOT/accounts"
  local dir
  for dir in "$(dirname "$BIN")" "$UNIT_DIR"; do
    [ -d "$dir" ] || install -d -m 0755 "$dir"
  done
}

ensure_binary() {
  step "Installing forkmesh-mirror-node"
  if [ -n "$BINARY" ] && [ "$BINARY" -ef "$BIN" ]; then
    say "Using the installed $BIN"
    return 0
  fi
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
  if [ "$PLATFORM" = "Darwin" ]; then
    build_binary_from_source
    return 0
  fi
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

build_binary_from_source() {
  local go stage src
  if ! go="$(find_go)"; then
    say "Installing Go with Homebrew to build forkmesh-mirror-node"
    brew_install go || true
    go="$(find_go)" ||
      die "Go is required to build forkmesh-mirror-node on macOS. Install it with: brew install go"
  fi
  stage="$(mktemp -d /private/tmp/forkmesh-runner-build.XXXXXX)"
  say "Building forkmesh-mirror-node from $SOURCE_URL (the signed release has no macOS build of it)"
  src="$stage/src"
  if ! { git clone --quiet --depth 1 --filter=blob:none --sparse "$SOURCE_URL" "$src" &&
         git -C "$src" sparse-checkout set server; } </dev/null; then
    src="$stage/full"
    git clone --quiet --depth 1 "$SOURCE_URL" "$src" </dev/null ||
      die "Could not clone $SOURCE_URL."
  fi
  [ -f "$src/server/go.mod" ] || die "$SOURCE_URL has no server/go.mod."
  if ! (cd "$src/server" && HOME="$stage" GOPATH="$stage/go" GOCACHE="$stage/cache" CGO_ENABLED=0 \
          "$go" build -trimpath -o "$stage/forkmesh-mirror-node" ./cmd/forkmesh-mirror-node) </dev/null; then
    warn "The go.mod toolchain could not be used; building with the installed Go."
    (cd "$src/server" && HOME="$stage" GOPATH="$stage/go" GOCACHE="$stage/cache" CGO_ENABLED=0 \
       GOTOOLCHAIN=local "$go" build -trimpath -o "$stage/forkmesh-mirror-node" ./cmd/forkmesh-mirror-node) </dev/null ||
      die "Could not build forkmesh-mirror-node."
  fi
  install -m 0755 "$stage/forkmesh-mirror-node" "$BIN"
  say "Installed $BIN (macOS clears the build folder $stage when it restarts)"
}

b64url() { base64 | tr '+/' '-_' | tr -d '=\n'; }

ensure_identity() {
  step "Runner identity"
  if [ -n "$FOUND" ] && [ -s "$KEY" ]; then
    SIGN_KEY="$KEY"
    say "Reusing the runner key at $KEY"
  else
    local old_umask
    old_umask="$(umask)"
    umask 077
    "$OPENSSL" genpkey -algorithm ed25519 -out "$STAGED_KEY.new" 2>/dev/null ||
      die "OpenSSL 1.1.1 or newer is required (Ed25519)."
    mv "$STAGED_KEY.new" "$STAGED_KEY"
    umask "$old_umask"
    SIGN_KEY="$STAGED_KEY"
    say "Made a new runner key for $NAME"
  fi
  chmod 0600 "$SIGN_KEY"
  PUBLIC_KEY="$("$OPENSSL" pkey -in "$SIGN_KEY" -pubout -outform DER | tail -c 32 | b64url)"
  [[ "$PUBLIC_KEY" =~ ^[A-Za-z0-9_-]{43}$ ]] || die "Could not read the runner's public key."
}

use_preregistration() {
  step "Runner $NAME for $PREREG_OWNER (registered by the instance)"
  OWNER="$PREREG_OWNER"
  if [ -n "$INSTANCE_OWNER" ] && [ "$INSTANCE_OWNER" != "$OWNER" ]; then
    die "Runner $NAME on this machine belongs to $INSTANCE_OWNER, not $OWNER."
  fi
  OWNERS_JSON="[\"$OWNER\"]"
  say "Skipping the token registration; the instance registers $NAME."
}

register_runner() {
  if [ "$PREREGISTERED" = "1" ]; then
    use_preregistration
    return 0
  fi
  step "Registering $NAME with ForkMesh"
  local ts digest message_file signature body answer
  ts="$(python3 -c 'import time; print(int(time.time() * 1000))')"
  digest="$(printf '%s' "$TOKEN" | "$OPENSSL" dgst -sha256 -r | cut -d' ' -f1)"
  message_file="$(mktemp)"
  printf 'forkmesh-runner-register-token-v1\n%s\n%s\n%s\n%s' "$NAME" "$PUBLIC_KEY" "$digest" "$ts" >"$message_file"
  signature="$("$OPENSSL" pkeyutl -sign -rawin -inkey "$SIGN_KEY" -in "$message_file" | b64url)"
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
  OWNER="$(printf '%s' "$answer" | python3 -c 'import json,sys
d=json.load(sys.stdin)["runner"]
owners=[o for o in d.get("allowedOwners") or [] if isinstance(o,str)]
owner=d.get("registeredBy") or (owners[0] if len(owners)==1 else "")
print(owner if owners==[owner] else "")')"
  [[ "$OWNER" =~ ^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$ ]] ||
    die "ForkMesh did not answer with the one account this runner belongs to."
  if [ -n "$INSTANCE_OWNER" ] && [ "$INSTANCE_OWNER" != "$OWNER" ]; then
    die "Runner $NAME on this machine belongs to $INSTANCE_OWNER, but ForkMesh registered it for $OWNER."
  fi
  OWNERS_JSON="[\"$OWNER\"]"
  say "Registered $NAME as a private runner of $OWNER."
}

write_config() {
  step "Writing the runner configuration"
  FM_NAME="$NAME" FM_KEY="$PUBLIC_KEY" FM_IDENTITY="$IDENTITY" FM_CONFIG="$CONFIG" \
  FM_KEYFILE="$KEY" FM_APP="$APP_URL" FM_OWNERS="$OWNERS_JSON" FM_WORK="$WORK" \
  FM_JOB_USER="$JOB_USER" FM_PREFIX="$PREFIX" FM_LISTEN="$LISTEN" FM_PROBE="$PROBE" \
  FM_ACTIONS="$ENABLE_ACTIONS" python3 - <<'PY'
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
config = {
    "schemaVersion": 1,
    "role": "runner",
    "listen": os.environ["FM_LISTEN"],
    "gatewayProbe": os.environ["FM_PROBE"],
    "gatewayConfig": live(os.environ["FM_IDENTITY"]),
    "identityKey": live(os.environ["FM_KEYFILE"]),
    "runner": {
        "appUrl": os.environ["FM_APP"],
        "maxConcurrentRuns": 1,
        "workRoot": live(os.environ["FM_WORK"]),
        "allowedOwners": json.loads(os.environ["FM_OWNERS"]),
        "jobUser": os.environ["FM_JOB_USER"],
    },
}
if os.environ["FM_ACTIONS"] == "1":
    config["enableActions"] = True
write(os.environ["FM_CONFIG"], config)
PY
  say "Wrote $CONFIG"
}

job_uid_of() {
  if [ "$TEST_MODE" = "1" ]; then
    id -u
  else
    id -u "$1"
  fi
}

write_firewall_macos() {
  local user uid
  : >"$PF_RULES.new"
  chmod 0600 "$PF_RULES.new"
  for user in $(job_users); do
    uid="$(job_uid_of "$user")"
    cat >>"$PF_RULES.new" <<EOF
block return out quick proto { tcp, udp } from any to { 169.254.0.0/16, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 100.64.0.0/10, 127.0.0.0/8, 0.0.0.0/8, 224.0.0.0/4, 240.0.0.0/4 } user $uid
block return out quick inet6 proto { tcp, udp } from any to { ::1, ::ffff:0:0/96, fc00::/7, fe80::/10, ff00::/8 } user $uid
EOF
  done
  mv "$PF_RULES.new" "$PF_RULES"
}

write_units_macos() {
  step "Installing the $RUNNER_LABEL launch daemon"
  local live_state
  live_state="$(live_path "$STATE")"
  cat >"$UNIT_DIR/$RUNNER_LABEL.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$RUNNER_LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>/usr/local/bin/forkmesh-mirror-node</string>
		<string>--config</string>
		<string>$(live_path "$CONFIG")</string>
	</array>
	<key>UserName</key>
	<string>root</string>
	<key>GroupName</key>
	<string>wheel</string>
	<key>Umask</key>
	<integer>63</integer>
	<key>EnvironmentVariables</key>
	<dict>
		<key>TZ</key>
		<string>UTC</string>
		<key>HOME</key>
		<string>$live_state</string>
		<key>TMPDIR</key>
		<string>$live_state/tmp</string>
		<key>PATH</key>
		<string>/usr/local/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin</string>
	</dict>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<dict>
		<key>SuccessfulExit</key>
		<false/>
	</dict>
	<key>ThrottleInterval</key>
	<integer>5</integer>
	<key>ExitTimeOut</key>
	<integer>15</integer>
	<key>SoftResourceLimits</key>
	<dict>
		<key>NumberOfFiles</key>
		<integer>8192</integer>
	</dict>
	<key>StandardOutPath</key>
	<string>$MAC_LOG</string>
	<key>StandardErrorPath</key>
	<string>$MAC_LOG</string>
</dict>
</plist>
EOF
  write_firewall_macos
  cat >"$UNIT_DIR/$FIREWALL_LABEL.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$FIREWALL_LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>/bin/sh</string>
		<string>-c</string>
		<string>/sbin/pfctl -s info 2&gt;/dev/null | grep -q 'Status: Enabled' || /sbin/pfctl -E; /sbin/pfctl -a $PF_ANCHOR -f $(live_path "$PF_RULES")</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>StartInterval</key>
	<integer>300</integer>
</dict>
</plist>
EOF
  chmod 0644 "$UNIT_DIR/$RUNNER_LABEL.plist" "$UNIT_DIR/$FIREWALL_LABEL.plist"
  if [ "$TEST_MODE" != "1" ]; then
    chown root:wheel "$UNIT_DIR/$RUNNER_LABEL.plist" "$UNIT_DIR/$FIREWALL_LABEL.plist"
  fi
}

write_firewall_linux() {
  local user
  {
    printf 'table inet forkmesh_runner\n'
    printf 'delete table inet forkmesh_runner\n'
    printf 'table inet forkmesh_runner {\n'
    printf '\tchain output {\n'
    printf '\t\ttype filter hook output priority filter; policy accept;\n'
    for user in $(job_users); do
      printf '\t\tmeta skuid "%s" ip daddr 127.0.0.53 meta l4proto { tcp, udp } th dport 53 accept\n' "$user"
      printf '\t\tmeta skuid "%s" ip daddr { 169.254.0.0/16, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 100.64.0.0/10, 127.0.0.0/8, 0.0.0.0/8, 224.0.0.0/4, 240.0.0.0/4 } counter reject\n' "$user"
      printf '\t\tmeta skuid "%s" ip6 daddr { ::1, ::ffff:0:0/96, fc00::/7, fe80::/10, ff00::/8 } counter reject\n' "$user"
    done
    printf '\t}\n'
    printf '}\n'
  } >"$NFT_BASE.new"
  {
    cat "$NFT_BASE.new"
    printf 'include "%s/*.nft"\n' "$(live_path "$NFT_LOCAL")"
  } >"$NFT.new"
  install -d -m 0700 "$NFT_LOCAL"
  chmod 0600 "$NFT_BASE.new" "$NFT.new"
  mv "$NFT_BASE.new" "$NFT_BASE"
  mv "$NFT.new" "$NFT"
}

write_legacy_unit() {
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
ReadWritePaths=/var/lib/forkmesh-runner $(live_path "$JOB_HOME")
InaccessiblePaths=-/var/lib/forkmesh-runner/accounts -/etc/forkmesh-runner/accounts
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
}

write_account_unit() {
  cat >"$UNIT_DIR/forkmesh-runner@.service" <<'EOF'
[Unit]
Description=ForkMesh runner of account %i (agent jobs, docs/RUNNERS.md)
Wants=network-online.target forkmesh-runner-firewall.service
After=network-online.target forkmesh-runner-firewall.service

[Service]
Type=simple
User=root
Group=root
UMask=0077
Environment=TZ=UTC
Environment=HOME=/var/lib/forkmesh-runner/accounts/%i
Environment=TMPDIR=/var/lib/forkmesh-runner/accounts/%i/tmp
Environment=PATH=/usr/local/bin:/usr/bin:/bin
ExecStartPre=/usr/local/bin/forkmesh-mirror-node --config /etc/forkmesh-runner/accounts/%i/runner.json --check
ExecStart=/usr/local/bin/forkmesh-mirror-node --config /etc/forkmesh-runner/accounts/%i/runner.json
Restart=on-failure
RestartSec=5s
TimeoutStopSec=15s
MemoryHigh=150M
MemoryMax=250M
NoNewPrivileges=true
PrivateDevices=true
PrivateTmp=false
BindPaths=/var/lib/forkmesh-runner/accounts/%i/tmp:/tmp /var/lib/forkmesh-runner/accounts/%i/tmp:/var/tmp
ProtectClock=true
ProtectControlGroups=true
ProtectHome=true
ProtectHostname=true
ProtectKernelLogs=true
ProtectKernelModules=true
ProtectKernelTunables=true
ProtectSystem=strict
ReadWritePaths=/var/lib/forkmesh-runner/accounts/%i
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
}

write_units() {
  INSTANCES="$(scan_instances)"
  if [ "$PLATFORM" = "Darwin" ]; then
    write_units_macos
    return 0
  fi
  step "Installing the $UNIT service"
  if [ -z "$INSTANCE" ]; then
    write_legacy_unit
  else
    write_account_unit
  fi
  write_firewall_linux
  cat >"$UNIT_DIR/forkmesh-runner-firewall.service" <<'EOF'
[Unit]
Description=ForkMesh runner: keep agent jobs off private networks
After=network-pre.target
Before=forkmesh-runner.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c '/usr/sbin/nft -c -f /etc/forkmesh-runner/jobs.nft && exec /usr/sbin/nft -f /etc/forkmesh-runner/jobs.nft; echo "forkmesh-runner: a rule file in /etc/forkmesh-runner/jobs.d does not parse; loading the built-in rules only" >&2; exec /usr/sbin/nft -f /etc/forkmesh-runner/jobs-base.nft'
ExecStop=/usr/sbin/nft delete table inet forkmesh_runner

[Install]
WantedBy=multi-user.target
EOF
}

npm_global_install() {
  if [ "$PLATFORM" = "Darwin" ] && login_user >/dev/null; then
    as_login_user npm install -g --no-fund --no-audit "$1" </dev/null && return 0
  fi
  npm install -g --no-fund --no-audit "$1" </dev/null
}

install_providers() {
  [ "$PROVIDERS" = "none" ] && return 0
  [ "$TEST_MODE" = "1" ] && return 0
  step "Agent CLIs ($PROVIDERS)"
  if ! command -v npm >/dev/null 2>&1; then
    say "Installing Node.js and npm"
    if [ "$PLATFORM" = "Darwin" ]; then
      brew_install node || warn "Could not install Node.js with Homebrew; install the agent CLIs yourself."
    else
      pm_install nodejs npm || warn "Could not install Node.js; install the agent CLIs yourself."
    fi
  fi
  command -v npm >/dev/null 2>&1 || return 0
  case ",$PROVIDERS," in
    *,claude,*)
      if command -v claude >/dev/null 2>&1; then say "Claude Code is installed"
      else npm_global_install @anthropic-ai/claude-code || warn "Could not install Claude Code."; fi ;;
  esac
  case ",$PROVIDERS," in
    *,codex,*)
      if command -v codex >/dev/null 2>&1; then say "Codex is installed"
      else npm_global_install @openai/codex || warn "Could not install Codex."; fi ;;
  esac
}

bootstrap_daemon() {
  local label="$1" tick
  launchctl bootout "system/$label" >/dev/null 2>&1 || true
  launchctl enable "system/$label" >/dev/null 2>&1 || true
  for tick in 1 2 3 4 5; do
    launchctl bootstrap system "$UNIT_DIR/$label.plist" 2>/dev/null && return 0
    sleep 1
  done
  return 1
}

start_macos() {
  step "Starting the runner"
  local tick rules="" state=""
  if bootstrap_daemon "$FIREWALL_LABEL"; then
    for tick in 1 2 3 4 5; do
      rules="$(pfctl -a "$PF_ANCHOR" -s rules 2>/dev/null || true)"
      case "$rules" in *user*) break ;; esac
      sleep 1
    done
  fi
  case "$rules" in
    *user*) say "The job firewall is loaded (pf anchor $PF_ANCHOR)" ;;
    *) warn "The job firewall did not load; agent jobs can reach this machine's private network." ;;
  esac
  bootstrap_daemon "$RUNNER_LABEL" || die "launchctl could not load $RUNNER_LABEL. Log: $MAC_LOG"
  for tick in 1 2 3 4 5 6; do
    sleep 2
    state="$(launchctl print "system/$RUNNER_LABEL" 2>/dev/null || true)"
    case "$state" in
      *"state = running"*) say "The runner is running. Log: $MAC_LOG"; return 0 ;;
    esac
  done
  die "$RUNNER_LABEL did not start. Log: $MAC_LOG"
}

start_firewall_linux() {
  if ! command -v nft >/dev/null 2>&1; then
    warn "nftables is missing, so agent jobs are not kept off this machine's private network."
    return 0
  fi
  systemctl enable forkmesh-runner-firewall.service >/dev/null 2>&1 || true
  if systemctl is-active --quiet forkmesh-runner-firewall.service; then
    if nft -c -f "$(live_path "$NFT")" >/dev/null 2>&1; then
      nft -f "$(live_path "$NFT")" ||
        warn "The job firewall did not reload; agent jobs can reach this machine's private network."
    else
      warn "A rule file in $(live_path "$NFT_LOCAL") does not parse; the job firewall loads its built-in rules only."
      nft -f "$(live_path "$NFT_BASE")" ||
        warn "The job firewall did not reload; agent jobs can reach this machine's private network."
    fi
  else
    systemctl start forkmesh-runner-firewall.service ||
      warn "The job firewall did not load; agent jobs can reach this machine's private network."
  fi
}

check_and_start() {
  "$BIN" --config "$CONFIG" --check >/dev/null 2>&1 || {
    [ "$TEST_MODE" = "1" ] && return 0
    die "The runner configuration did not pass forkmesh-mirror-node --check. Run it by hand to see why."
  }
  [ "$TEST_MODE" = "1" ] && return 0
  if [ "$PLATFORM" = "Darwin" ]; then
    start_macos
    return 0
  fi
  step "Starting the runner"
  systemctl daemon-reload
  start_firewall_linux
  systemctl enable "$UNIT"
  systemctl restart "$UNIT"
  local tick
  for tick in 1 2 3 4 5 6; do
    sleep 2
    if systemctl is-active --quiet "$UNIT"; then
      say "The runner is running. Logs: journalctl -u $UNIT"
      return 0
    fi
  done
  die "$UNIT did not start. Logs: journalctl -u $UNIT"
}

require_system
install_packages
ensure_base_dirs
ensure_binary
find_instance
ensure_identity
register_runner
settle_instance
choose_job_user
ensure_job_user
pick_listen
write_config
write_units
install_providers
check_and_start

if [ "$PREREGISTERED" = "1" ]; then
  say ""
  say "Done. $NAME runs Actions jobs for $OWNER as $JOB_USER."
  printf 'FM_RUNNER_PUBLIC_KEY=%s\n' "$PUBLIC_KEY"
  exit 0
fi
say ""
say "Done. $NAME is registered as your private runner; its jobs run as $JOB_USER."
say "Other accounts' runners on this machine keep running next to it, each with its own user."
say "Next: open https://forkmesh.com/dashboard/runners and press Connect Claude (or Codex)"
say "to sign the agent CLI in. The login stays on this machine."
exit 0
}
