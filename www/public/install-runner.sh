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
JOB_USER=""
JOB_HOME=""
WORK=""
OWNER=""
EXISTING_NAME=""
EXISTING_OWNER=""
EXISTING_JOB_USER=""
EXISTING_WORK=""

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

What it installs: /usr/local/bin/forkmesh-mirror-node (run as a runner, it serves no
repositories), the forkmesh-runner service, an unprivileged job user of its own for your
ForkMesh account (fmjob-<account>) that agent jobs and their sign-ins live under, firewall
rules that keep jobs off private networks, and the chosen agent CLIs. The runner is private
to your account: nobody else can see it or run jobs on it. It keeps any existing ForkMesh
data on this machine and deletes nothing.

On macOS the service is the com.forkmesh.runner launch daemon, the firewall is a pf
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

PLATFORM="$(uname -s)"
if [ "$TEST_MODE" = "1" ] && [ -n "${FORKMESH_RUNNER_PLATFORM:-}" ]; then
  PLATFORM="$FORKMESH_RUNNER_PLATFORM"
fi
SOURCE_URL="${FORKMESH_RUNNER_SOURCE_URL:-https://forkmesh.com/forkmesh/forkmesh}"
OPENSSL="openssl"

if [ "$PLATFORM" = "Darwin" ]; then
  ETC="$PREFIX/private/etc/forkmesh-runner"
  STATE="$PREFIX/private/var/db/forkmesh-runner"
  JOB_BASE="$PREFIX/private/var/db"
  UNIT_DIR="$PREFIX/Library/LaunchDaemons"
  PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
else
  ETC="$PREFIX/etc/forkmesh-runner"
  STATE="$PREFIX/var/lib/forkmesh-runner"
  JOB_BASE="$PREFIX/var/lib"
  UNIT_DIR="$PREFIX/etc/systemd/system"
fi
BIN="$PREFIX/usr/local/bin/forkmesh-mirror-node"
KEY="$STATE/identity/ed25519.pem"
IDENTITY="$STATE/gateway/config.json"
CONFIG="$ETC/runner.json"
NFT="$ETC/jobs.nft"
PF_RULES="$ETC/jobs.pf"
PF_ANCHOR="com.apple/forkmesh-runner"
RUNNER_LABEL="com.forkmesh.runner"
FIREWALL_LABEL="com.forkmesh.runner-firewall"
MAC_LOG="/private/var/log/forkmesh-runner.log"

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
    dscl . -create "/Groups/$JOB_USER" RealName "ForkMesh runner jobs for $OWNER"
    dscl . -create "/Groups/$JOB_USER" Password '*'
  fi
  dscl . -create "/Users/$JOB_USER"
  dscl . -create "/Users/$JOB_USER" UniqueID "$id"
  dscl . -create "/Users/$JOB_USER" PrimaryGroupID "$gid"
  dscl . -create "/Users/$JOB_USER" UserShell /usr/bin/false
  dscl . -create "/Users/$JOB_USER" NFSHomeDirectory "$(live_path "$JOB_HOME")"
  dscl . -create "/Users/$JOB_USER" RealName "ForkMesh runner jobs for $OWNER"
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

read_existing() {
  local fields
  if [ -s "$IDENTITY" ]; then
    EXISTING_NAME="$(python3 -c 'import json,sys
try: print(json.load(open(sys.argv[1]))["node"]["name"])
except Exception: print("")' "$IDENTITY" 2>/dev/null || true)"
    [[ "$EXISTING_NAME" =~ ^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$ ]] || EXISTING_NAME=""
  fi
  [ -s "$CONFIG" ] || return 0
  fields="$(python3 -c 'import json,sys
try:
    r=json.load(open(sys.argv[1]))["runner"]
    owners=r.get("allowedOwners") or []
    print(owners[0] if len(owners)==1 else "", r.get("jobUser") or "", r.get("workRoot") or "", sep="\x1f")
except Exception: print("\x1f\x1f")' "$CONFIG" 2>/dev/null || true)"
  IFS=$'\x1f' read -r EXISTING_OWNER EXISTING_JOB_USER EXISTING_WORK <<<"$fields" || true
  [[ "$EXISTING_OWNER" =~ ^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$ ]] || EXISTING_OWNER=""
  [[ "$EXISTING_JOB_USER" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] && [ "$EXISTING_JOB_USER" != "root" ] || EXISTING_JOB_USER=""
  case "$EXISTING_WORK" in
    "$(live_path "$STATE")"/*) case "$EXISTING_WORK" in *..*) EXISTING_WORK="" ;; esac ;;
    *) EXISTING_WORK="" ;;
  esac
}

choose_job_user() {
  if [ -n "$EXISTING_OWNER" ] && [ "$EXISTING_OWNER" != "$OWNER" ]; then
    warn "This machine ran runner ${EXISTING_NAME:-?} for the ForkMesh account $EXISTING_OWNER. It now runs $NAME for $OWNER; $EXISTING_OWNER's job user and sign-ins stay where they are and $NAME never uses them."
  fi
  if [ "$EXISTING_OWNER" = "$OWNER" ] && [ "$EXISTING_NAME" = "$NAME" ]; then
    JOB_USER="$EXISTING_JOB_USER"
    [ -n "$EXISTING_WORK" ] && WORK="$PREFIX$EXISTING_WORK"
  fi
  [ -n "$JOB_USER" ] || JOB_USER="$(job_user_for "$OWNER")"
  [[ "$JOB_USER" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] || die "Could not derive a job user name for $OWNER."
  JOB_HOME="$JOB_BASE/$JOB_USER"
  [ -n "$WORK" ] || WORK="$STATE/jobs/$JOB_USER"
}

ensure_job_user() {
  step "Job user $JOB_USER for $OWNER"
  if [ "$TEST_MODE" != "1" ] && [ "$PLATFORM" = "Darwin" ]; then
    dscl . -read "/Users/$JOB_USER" UniqueID >/dev/null 2>&1 || create_macos_job_user
    install -d -o "$JOB_USER" -g "$JOB_USER" -m 0700 "$JOB_HOME" "$JOB_HOME/.claude" "$JOB_HOME/.codex"
  elif [ "$TEST_MODE" != "1" ]; then
    if ! id "$JOB_USER" >/dev/null 2>&1; then
      useradd --system --home-dir "$JOB_HOME" --create-home --shell /usr/sbin/nologin "$JOB_USER"
    fi
    install -d -o "$JOB_USER" -g "$JOB_USER" -m 0700 "$JOB_HOME" "$JOB_HOME/.claude" "$JOB_HOME/.codex"
  else
    install -d -m 0700 "$JOB_HOME" "$JOB_HOME/.claude" "$JOB_HOME/.codex"
  fi
  install -d -m 0711 "$WORK"
  say "Agent jobs for $OWNER run as $JOB_USER (home $(live_path "$JOB_HOME"), work $(live_path "$WORK"))"
}

ensure_dirs() {
  install -d -m 0700 "$ETC"
  install -d -m 0711 "$STATE" "$STATE/jobs"
  install -d -m 0700 "$STATE/identity" "$STATE/gateway"
  install -d -m 1777 "$STATE/tmp"
  local dir
  for dir in "$(dirname "$BIN")" "$UNIT_DIR"; do
    [ -d "$dir" ] || install -d -m 0755 "$dir"
  done
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
  if [ -s "$KEY" ] && [ -n "$EXISTING_NAME" ] && [ "$EXISTING_NAME" != "$NAME" ]; then
    local aside="$KEY.$EXISTING_NAME"
    [ -e "$aside" ] && aside="$aside.$(date +%s)"
    mv "$KEY" "$aside"
    say "Kept the key of runner $EXISTING_NAME at $aside; $NAME gets its own key"
  fi
  if [ -s "$KEY" ]; then
    say "Reusing the runner key at $KEY"
  else
    local old_umask
    old_umask="$(umask)"
    umask 077
    "$OPENSSL" genpkey -algorithm ed25519 -out "$KEY.new" 2>/dev/null ||
      die "OpenSSL 1.1.1 or newer is required (Ed25519)."
    mv "$KEY.new" "$KEY"
    umask "$old_umask"
    say "Made a new runner key at $KEY"
  fi
  chmod 0600 "$KEY"
  PUBLIC_KEY="$("$OPENSSL" pkey -in "$KEY" -pubout -outform DER | tail -c 32 | b64url)"
  [[ "$PUBLIC_KEY" =~ ^[A-Za-z0-9_-]{43}$ ]] || die "Could not read the runner's public key."
}

register_runner() {
  step "Registering $NAME with ForkMesh"
  local ts digest message_file signature body answer
  ts="$(python3 -c 'import time; print(int(time.time() * 1000))')"
  digest="$(printf '%s' "$TOKEN" | "$OPENSSL" dgst -sha256 -r | cut -d' ' -f1)"
  message_file="$(mktemp)"
  printf 'forkmesh-runner-register-token-v1\n%s\n%s\n%s\n%s' "$NAME" "$PUBLIC_KEY" "$digest" "$ts" >"$message_file"
  signature="$("$OPENSSL" pkeyutl -sign -rawin -inkey "$KEY" -in "$message_file" | b64url)"
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
  OWNERS_JSON="[\"$OWNER\"]"
  say "Registered $NAME as a private runner of $OWNER."
}

write_config() {
  step "Writing the runner configuration"
  FM_NAME="$NAME" FM_KEY="$PUBLIC_KEY" FM_IDENTITY="$IDENTITY" FM_CONFIG="$CONFIG" \
  FM_KEYFILE="$KEY" FM_APP="$APP_URL" FM_OWNERS="$OWNERS_JSON" FM_WORK="$WORK" \
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

write_units_macos() {
  step "Installing the $RUNNER_LABEL launch daemon"
  local job_uid live_state
  live_state="$(live_path "$STATE")"
  if [ "$TEST_MODE" = "1" ]; then
    job_uid="$(id -u)"
  else
    job_uid="$(id -u "$JOB_USER")"
  fi
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
  cat >"$PF_RULES" <<EOF
block return out quick proto { tcp, udp } from any to { 169.254.0.0/16, 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 100.64.0.0/10, 127.0.0.0/8, 0.0.0.0/8, 224.0.0.0/4, 240.0.0.0/4 } user $job_uid
block return out quick inet6 proto { tcp, udp } from any to { ::1, ::ffff:0:0/96, fc00::/7, fe80::/10, ff00::/8 } user $job_uid
EOF
  chmod 0600 "$PF_RULES"
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

write_units() {
  if [ "$PLATFORM" = "Darwin" ]; then
    write_units_macos
    return 0
  fi
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
read_existing
ensure_binary
ensure_identity
register_runner
choose_job_user
ensure_job_user
write_config
write_units
install_providers
check_and_start

say ""
say "Done. $NAME is registered as your private runner; its jobs run as $JOB_USER."
say "Next: open https://forkmesh.com/dashboard/runners and press Connect Claude (or Codex)"
say "to sign the agent CLI in. The login stays on this machine."
exit 0
}
