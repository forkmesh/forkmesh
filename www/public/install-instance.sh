#!/usr/bin/env bash
{
set -euo pipefail
umask 022

INSTANCE_INSTALLER_VERSION="2"
CONFIG_MODE=""
DETACHED="0"
CONFIG_FILE=""
INSTALL_SH_URL="${FORKMESH_INSTALL_SH_URL:-https://forkmesh.com/install.sh}"
OPT="/opt/forkmesh-instance"
SRC="$OPT/src"
VENV="$OPT/venv"
NEXT_SRC="$OPT/src.next"
PREV_SRC="$OPT/src.prev"
NEXT_VENV="$OPT/venv.next"
PREV_VENV="$OPT/venv.prev"
BUILD_SRC="$SRC"
BUILD_VENV="$VENV"
TARGET_COMMIT=""
SWAPPED="0"
VENV_SWAPPED="0"
APP_RESTARTED="0"
SOURCE_COMMITTED="0"
DAEMON_BUILD="$OPT/daemon-build.json"
RELEASE_SOURCE="https://app.forkmesh.com/forkmesh/forkmesh"
ETC="/etc/forkmesh-instance"
VAR="/var/lib/forkmesh-instance"
APP_ENV="$ETC/app.env"
INSTANCE_JSON="$ETC/instance.json"
APP_USER="forkmesh-app"
NODE_USER="forkmesh-node"
NODE_NAME="forkmesh"
DAEMON="/usr/local/bin/forkmesh-mirror-node"
DAEMON_CONFIG="/etc/forkmesh/mirror-node.json"
DAEMON_TOOLS="/usr/local/share/forkmesh/tools"
APP_UNIT="/etc/systemd/system/forkmesh-instance.service"
DAEMON_UNIT="/etc/systemd/system/forkmesh-mirror-node.service"
CLEANUP_DIR="/usr/local/lib/forkmesh-instance"
LOGIN_MARKER="$VAR/first-admin-login"
LOGIN_CLEANUP="$CLEANUP_DIR/first-login-cleanup.sh"
LOGIN_CLEANUP_UNIT="/etc/systemd/system/forkmesh-instance-first-login-cleanup.service"
LOGIN_CLEANUP_TIMER="/etc/systemd/system/forkmesh-instance-first-login-cleanup.timer"
FIRST_LOGIN_SECONDS=300
APP_PORT="8080"
GATEWAY_PORT="8790"
DNS_WAIT_SECONDS="${FORKMESH_DNS_WAIT_SECONDS:-1200}"
REPO_WAIT_SECONDS="${FORKMESH_REPO_WAIT_SECONDS:-900}"
SETUP_UNIT="forkmesh-instance-setup"
SETUP_RUN_DIR="/run/forkmesh-instance-setup"
SETUP_LOG="/var/log/forkmesh-instance-setup.log"
BUILD_STAMP="$OPT/build.stamp"
BUILD_INFO="$OPT/build-info.json"
UPDATE_LIB="/usr/local/lib/forkmesh-instance-update"
UPDATE_STATE="/var/lib/forkmesh-instance-update"
UPDATE_REQUEST_DIR="$VAR/update"
UPDATE_UNIT="/etc/systemd/system/forkmesh-instance-update.service"
UPDATE_PATH_UNIT="/etc/systemd/system/forkmesh-instance-update.path"
PACKAGES=(ca-certificates curl git openssl python3 python3-venv python3-pip nodejs npm caddy sqlite3 iproute2 age librsvg2-bin)
ACTIONS_PACKAGES=(git curl jq python3-venv nodejs npm build-essential unzip nftables)
RUNNER_INSTALLER="www/public/install-runner.sh"
RUNNER_ETC="/etc/forkmesh-runner"
RUNNER_STATE="/var/lib/forkmesh-runner"
RUNNER_JOB_USER="forkmesh-job"
RUNNER_NAME=""
CERT_WAIT_SECONDS="${FORKMESH_CERT_WAIT_SECONDS:-300}"
PROXY_WAIT_SECONDS="${FORKMESH_PROXY_WAIT_SECONDS:-60}"
CLOUDFLARE_RANGES="173.245.48.0/20 103.21.244.0/22 103.22.200.0/22 103.31.4.0/22 141.101.64.0/18 108.162.192.0/18 190.93.240.0/20 188.114.96.0/20 197.234.240.0/22 198.41.128.0/17 162.158.0.0/15 104.16.0.0/13 104.24.0.0/14 172.64.0.0/13 131.0.72.0/22 2400:cb00::/32 2606:4700::/32 2803:f800::/32 2405:b500::/32 2405:8100::/32 2a06:98c0::/29 2c0f:f248::/32"
PROXIED="0"
APP_CHANGED="0"
ADMIN_PATH_SOURCE=""
ADMIN_PATH_VALUE=""
DAEMON_BEFORE=""
EXCLUDED_PACKAGES="marketing,crm,competitor,ledger,x-banner-board,reddit-banner-board,forkmesh-sky-office,discord,sentry"
CURRENT_STEP=""

say() { printf '%s\n' "$*"; }
progress() { printf 'FMI {"step":"%s","state":"%s"}\n' "$1" "$2"; }
begin() {
  if [ -n "$CURRENT_STEP" ]; then progress "$CURRENT_STEP" done; fi
  CURRENT_STEP="$1"
  progress "$1" running
  printf '\n==> %s\n' "$2"
}
warn() { printf 'Warning: %s\n' "$*"; }
die() {
  printf 'Error: %s\n' "$1"
  [ -n "$CURRENT_STEP" ] && progress "$CURRENT_STEP" failed
  CURRENT_STEP=""
  exit "${2:-1}"
}
on_exit() {
  local code=$?
  if [ "$code" -ne 0 ] && [ -n "$CURRENT_STEP" ]; then
    progress "$CURRENT_STEP" failed
    printf 'Error: the step above stopped with status %s.\n' "$code"
  fi
  if [ "$code" -ne 0 ]; then rollback_source || true; fi
  if [ "$DETACHED" = "1" ]; then printf '%s\n' "$code" > "$SETUP_RUN_DIR/status" 2>/dev/null || true; fi
}
trap on_exit EXIT

usage() {
  cat <<'EOF'
ForkMesh instance installer: turns a fresh Debian 13 or Ubuntu 24.04 server into
your own ForkMesh (Git hosting, repositories, chat and agents) under your name.

Most people never run this by hand: the "Start your own instance" page at
https://forkmesh.com/start connects to the server and runs it for you.

  bash install-instance.sh --config settings.json
  bash install-instance.sh --update

Options:
  --config FILE    JSON settings (see docs/INSTANCES.md)
  --config-stdin   read the settings as one base64 line on standard input
  --update         pull the latest ForkMesh source, rebuild and restart
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --config) CONFIG_MODE="file"; CONFIG_FILE="${2:-}"; shift 2 ;;
    --config=*) CONFIG_MODE="file"; CONFIG_FILE="${1#*=}"; shift ;;
    --config-stdin) CONFIG_MODE="stdin"; shift ;;
    --update) CONFIG_MODE="update"; shift ;;
    --detached) DETACHED="1"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'Unknown option: %s (see --help)\n' "$1"; exit 2 ;;
  esac
done
[ -n "$CONFIG_MODE" ] || { usage; exit 2; }
PARSE_ONLY="${FORKMESH_INSTANCE_PARSE_ONLY:-0}"
[ "$PARSE_ONLY" = "1" ] || [ "$(id -u)" = "0" ] || { say "Run this as root (or through sudo)."; exit 12; }
export DEBIAN_FRONTEND=noninteractive
export HOME="${HOME:-/root}"
export PYTHONUNBUFFERED=1

RAW_CONFIG=""
case "$CONFIG_MODE" in
  stdin)
    IFS= read -r RAW_CONFIG || true
    RAW_CONFIG="$(printf '%s' "$RAW_CONFIG" | base64 -d 2>/dev/null)" ||
      { say "The settings could not be read."; exit 2; }
    ;;
  file)
    [ -r "$CONFIG_FILE" ] || { say "Cannot read $CONFIG_FILE"; exit 2; }
    RAW_CONFIG="$(cat "$CONFIG_FILE")"
    if [ "$DETACHED" = "1" ] && [ "$CONFIG_FILE" = "$SETUP_RUN_DIR/settings.json" ]; then rm -f "$CONFIG_FILE"; fi
    ;;
  update)
    [ -r "$INSTANCE_JSON" ] || { say "No instance is installed here ($INSTANCE_JSON is missing)."; exit 2; }
    RAW_CONFIG="$(cat "$INSTANCE_JSON")"
    ;;
esac

command -v python3 >/dev/null 2>&1 || {
  apt-get update -qq >/dev/null && apt-get install -y -qq python3 >/dev/null
}

parse_config() {
  python3 -c '
import base64, binascii, json, re, shlex, sys
raw = sys.stdin.read()
try:
    c = json.loads(raw)
except ValueError:
    sys.exit("The settings are not valid JSON.")
if not isinstance(c, dict):
    sys.exit("The settings are not a JSON object.")
update = sys.argv[1] == "update"
name = re.compile(r"^[a-z](?:[a-z0-9-]{0,37}[a-z0-9])?$")
checks = {
    "projectName": r"^[A-Za-z0-9][A-Za-z0-9 ._&+-]{0,39}$",
    "domain": r"^(?=.{4,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$",
    "publicAddress": r"^[0-9a-fA-F:.]{3,45}$",
    "adminUsername": name.pattern,
    "adminEmail": r"^[A-Za-z0-9.!#$%&*+/=?^_`{|}~-]{1,64}@(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}$",
    "ownerType": r"^(org|user)$",
    "ownerName": name.pattern,
    "mailProvider": r"^(none|mailtrap|sendgrid|mailgun)$",
    "sourceUrl": r"^https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?/[A-Za-z0-9._/-]+$",
    "sourceRef": r"^[A-Za-z0-9][A-Za-z0-9._/-]{0,120}$",
}
optional = {
    "mailApiKey": r"^[A-Za-z0-9._:+/=-]{8,400}$",
    "mailFrom": checks["adminEmail"],
    "mailgunDomain": checks["domain"],
    "mailgunRegion": r"^(us|eu)$",
    "launchId": r"^[0-9a-f]{32}$",
    "platformUrl": checks["sourceUrl"],
    "adminPath": r"^/[A-Za-z0-9_-]{8,64}$",
}
reserved_paths = {
    "admin", "api", "assets", "changelog", "dashboard", "developers", "ecosystem",
    "features", "forgot-password", "forkmesh", "forkmesh-mirror", "hackathon",
    "instance-theme", "leaderboards", "microjobs", "mirror-payouts", "nodeinfo",
    "pitch-deck", "product-analysis", "referral", "referrals", "reset-password",
    "security", "security-report", "simulate-sentry-error", "transparency",
    "verify-pending", "well-known", "client-error", "cron-runner", "desktop-error",
}
values = {}
c.setdefault("mailProvider", "none")
c.setdefault("ownerType", "org")
c.setdefault("sourceUrl", "https://app.forkmesh.com/forkmesh/forkmesh")
c.setdefault("sourceRef", "main")
for key, pattern in checks.items():
    value = str(c.get(key) or "").strip()
    if key == "projectName":
        value = " ".join(value.split())
    if key in ("domain", "adminUsername", "ownerName", "adminEmail"):
        value = value.lower()
    if key == "ownerName" and c.get("ownerType") == "user":
        value = str(c.get("adminUsername") or "").strip().lower()
    if not re.fullmatch(pattern, value):
        sys.exit("Setting %s is missing or not valid." % key)
    values[key] = value
for key, pattern in optional.items():
    value = str(c.get(key) or "").strip()
    if value and not re.fullmatch(pattern, value):
        sys.exit("Setting %s is not valid." % key)
    values[key] = value
if update:
    values["adminPath"] = ""
if values["adminPath"][1:].lower() in reserved_paths:
    sys.exit("Setting adminPath is not valid: it is a built-in address.")
if "forkmesh" in (values["adminUsername"], values["ownerName"]):
    sys.exit("The name forkmesh is kept for the instance git node.")
if values["ownerType"] == "org" and values["ownerName"] == values["adminUsername"]:
    sys.exit("The organization name must differ from the admin username.")
password = c.get("adminPassword")
if not update and (not isinstance(password, str) or not 8 <= len(password) <= 256):
    sys.exit("Setting adminPassword is missing or too short.")
owners = c.get("owners") or []
if not isinstance(owners, list) or len(owners) > 20:
    sys.exit("Setting owners must be a list of up to 20 owners.")
taken = {values["adminUsername"], values["ownerName"], "forkmesh"}
emails = {values["adminEmail"]}
names = []
for item in owners:
    if not isinstance(item, dict):
        sys.exit("Each entry in owners needs a username, email and password.")
    who = str(item.get("username") or "").strip().lower()
    mail = str(item.get("email") or "").strip().lower()
    secret = item.get("password")
    if not name.fullmatch(who) or who in taken:
        sys.exit("Owner username %s is not valid, or is used twice." % who[:40])
    if not re.fullmatch(checks["adminEmail"], mail) or mail in emails:
        sys.exit("Owner %s needs an email address of their own." % who)
    if not update and (not isinstance(secret, str) or not 8 <= len(secret) <= 256):
        sys.exit("Owner %s needs a password of at least 8 characters." % who)
    taken.add(who)
    emails.add(mail)
    names.append(who)
if names and values["ownerType"] != "org":
    sys.exit("Setting owners needs ownerType org.")
values["owners"] = ",".join(names)
if values["mailProvider"] != "none" and not update and not values["mailApiKey"]:
    sys.exit("Setting mailApiKey is missing.")
if values["mailProvider"] == "mailgun" and not values["mailgunDomain"]:
    sys.exit("Setting mailgunDomain is missing.")
values["domainProvided"] = "1" if c.get("domainProvided", True) else "0"
actions = c.get("actions", True)
if not isinstance(actions, bool):
    sys.exit("Setting actions must be true or false.")
values["actions"] = "1" if actions else "0"
powered_by = c.get("poweredBy", True)
if not isinstance(powered_by, bool):
    sys.exit("Setting poweredBy must be true or false.")
values["poweredBy"] = "1" if powered_by else "0"
single = c.get("singleOrganization", False)
if not isinstance(single, bool):
    sys.exit("Setting singleOrganization must be true or false.")
if single and values["ownerType"] != "org":
    sys.exit("Setting singleOrganization needs ownerType org.")
values["singleOrganization"] = "1" if single else "0"
values["logoType"] = values["logoData"] = ""
logo = str(c.get("logo") or "").strip()
if logo and not update:
    match = re.fullmatch(r"data:image/(?:png|jpeg|webp|svg\+xml);base64,([A-Za-z0-9+/]+={0,2})", logo)
    try:
        image = base64.b64decode(match.group(1), validate=True) if match else b""
    except (binascii.Error, ValueError):
        image = b""
    kind = ""
    if image.startswith(b"\x89PNG\r\n\x1a\n"):
        kind = "png"
    elif image.startswith(b"\xff\xd8\xff"):
        kind = "jpg"
    elif image[:4] == b"RIFF" and image[8:12] == b"WEBP":
        kind = "webp"
    elif re.search(rb"<svg[\s>]", image[:4096]) and not re.search(
            rb"<script|<foreignobject|<iframe|<embed|<object|\son[a-z]+\s*=|javascript:|<!entity", image, re.I):
        kind = "svg"
    if not kind or len(image) > 200 * 1024:
        sys.exit("Setting logo must be a PNG, JPEG, WebP or SVG image of up to 200 KB, as a data: URL.")
    values["logoType"] = kind
    values["logoData"] = base64.b64encode(image).decode("ascii")
plugins = [p for p in (c.get("plugins") or []) if isinstance(p, str) and re.fullmatch(r"[a-z][a-z0-9-]{0,40}", p)]
values["plugins"] = ",".join(plugins)
theme = c.get("theme") or ""
if isinstance(theme, dict):
    if not set(theme) <= {"dark", "light"} or not all(
            isinstance(tokens, dict) and all(
                isinstance(name, str) and re.fullmatch(r"[a-z][a-z-]{0,40}", name)
                and isinstance(colour, str) and re.fullmatch(r"#[0-9a-fA-F]{6}", colour)
                for name, colour in tokens.items())
            for tokens in theme.values()):
        sys.exit("Setting theme must map dark and light to token names and #rrggbb colours.")
    theme = json.dumps(theme, separators=(",", ":"), sort_keys=True)
    if len(theme) > 8192:
        sys.exit("Setting theme is larger than 8 KB.")
elif not isinstance(theme, str) or (theme and not re.fullmatch(r"[a-z][a-z0-9-]{0,40}", theme)):
    sys.exit("Setting theme must be the name of a theme preset or a set of colours.")
values["theme"] = theme
legal = c.get("legalPages") or {}
if update:
    legal = {}
if not isinstance(legal, dict) or not set(legal) <= {"terms", "privacy", "security"}:
    sys.exit("Setting legalPages may only name terms, privacy and security.")
for page, entry in legal.items():
    if entry in (None, False):
        continue
    if isinstance(entry, str):
        entry = {"url": entry}
    if not isinstance(entry, dict) or not set(entry) <= {"markdown", "url", "contact"} or \
            bool(entry.get("markdown")) == bool(entry.get("url")):
        sys.exit("Setting legalPages.%s must give markdown or url, not both." % page)
    if entry.get("url") and not (isinstance(entry["url"], str) and re.fullmatch(r"https?://[^\s<>\"]{1,1990}", entry["url"])):
        sys.exit("Setting legalPages.%s.url must be an http or https address." % page)
    if entry.get("markdown") and not (isinstance(entry["markdown"], str) and len(entry["markdown"].encode("utf-8")) <= 32768):
        sys.exit("Setting legalPages.%s.markdown must be text of up to 32 KB." % page)
    if entry.get("contact") and (page != "security" or not isinstance(entry["contact"], str)):
        sys.exit("Setting legalPages: only security takes a contact.")
    legal[page] = entry
legal = {page: entry for page, entry in legal.items() if entry not in (None, False)}
values["legalPages"] = json.dumps(legal, separators=(",", ":"), sort_keys=True) if legal else ""
directory = c.get("directory") or {}
if update:
    directory = {}
if not isinstance(directory, dict) or not set(directory) <= {"listed", "world"} or \
        not all(isinstance(value, bool) for value in directory.values()):
    sys.exit("Setting directory may only set listed and world, each true or false.")
values["directory"] = json.dumps(directory, separators=(",", ":"), sort_keys=True) if directory else ""
for key, value in values.items():
    print("CFG_%s=%s" % (key, shlex.quote(value)))
' "$CONFIG_MODE"
}

CONFIG_SHELL="$(printf '%s' "$RAW_CONFIG" | parse_config 2>&1)" || {
  say "Error: $CONFIG_SHELL"
  progress system failed
  exit 2
}
eval "$CONFIG_SHELL"
ADMIN_PASSWORD=""
OWNERS_JSON="[]"
if [ "$CONFIG_MODE" != "update" ]; then
  ADMIN_PASSWORD="$(printf '%s' "$RAW_CONFIG" | python3 -c 'import json,sys; print(json.loads(sys.stdin.read()).get("adminPassword") or "")')"
  OWNERS_JSON="$(printf '%s' "$RAW_CONFIG" | python3 -c 'import json,sys; print(json.dumps(json.loads(sys.stdin.read()).get("owners") or []))')"
fi
if [ "$CFG_actions" = "1" ]; then
  PACKAGES+=("${ACTIONS_PACKAGES[@]}")
fi
RUNNER_UNIT="forkmesh-runner@$CFG_ownerName.service"
DOMAIN="$CFG_domain"
ORIGIN="https://$DOMAIN"
DIRECTORY_URL="https://app.forkmesh.com"
DIRECTORY_HOST="app.forkmesh.com"
REPO_URL="$ORIGIN/$CFG_ownerName/forkmesh"
if [ "${CFG_singleOrganization:-0}" = "1" ]; then REPO_URL="$ORIGIN/forkmesh"; fi
NEW_INSTANCE=0
if [ "$CONFIG_MODE" != "update" ] && { [ ! -e "$INSTANCE_JSON" ] || [ -e "$CLEANUP_DIR/new-instance" ]; }; then
  NEW_INSTANCE=1
fi
if [ "$PARSE_ONLY" = "1" ]; then
  printf '%s\n' "$CONFIG_SHELL" | grep -v -e '^CFG_mailApiKey=' -e '^CFG_logoData=' -e '^CFG_adminPath='
  printf 'ADMIN_PATH_LENGTH=%s\n' "${#CFG_adminPath}"
  printf 'LOGO_DATA_LENGTH=%s\n' "${#CFG_logoData}"
  printf 'ADMIN_PASSWORD_LENGTH=%s\n' "${#ADMIN_PASSWORD}"
  printf 'MAIL_KEY_LENGTH=%s\n' "${#CFG_mailApiKey}"
  printf 'REPO_URL=%s\n' "$REPO_URL"
  exit 0
fi

can_detach() {
  [ "$DETACHED" = "0" ] && [ "${FORKMESH_INSTANCE_FOREGROUND:-0}" != "1" ] &&
    [ -d /run/systemd/system ] && command -v systemd-run >/dev/null 2>&1 &&
    [ -f "${BASH_SOURCE[0]}" ]
}

setup_running() { systemctl is-active --quiet "$SETUP_UNIT.service"; }

start_detached() {
  install -d -m 0700 "$SETUP_RUN_DIR"
  install -m 0700 "${BASH_SOURCE[0]}" "$SETUP_RUN_DIR/install-instance.sh"
  local args=(--detached --update)
  if [ "$CONFIG_MODE" != "update" ]; then
    (umask 077 && printf '%s' "$RAW_CONFIG" > "$SETUP_RUN_DIR/settings.json")
    args=(--detached --config "$SETUP_RUN_DIR/settings.json")
  fi
  rm -f "$SETUP_RUN_DIR/status"
  if [ -s "$SETUP_LOG" ]; then mv -f "$SETUP_LOG" "$SETUP_LOG.1"; fi
  (umask 077 && : > "$SETUP_LOG")
  local env_args=() name
  for name in FORKMESH_INSTALL_SH_URL FORKMESH_INSTANCE_SOURCE_DIR FORKMESH_INSTANCE_DAEMON \
      FORKMESH_INSTANCE_TLS FORKMESH_DNS_WAIT_SECONDS FORKMESH_REPO_WAIT_SECONDS FORKMESH_CERT_WAIT_SECONDS; do
    if [ -n "${!name:-}" ]; then env_args+=("--setenv=$name=${!name}"); fi
  done
  systemctl reset-failed "$SETUP_UNIT.service" >/dev/null 2>&1 || true
  if ! systemd-run --quiet --unit "$SETUP_UNIT" --service-type=exec --setenv=HOME=/root \
      "${env_args[@]}" -p "StandardOutput=append:$SETUP_LOG" -p "StandardError=append:$SETUP_LOG" \
      /bin/bash "$SETUP_RUN_DIR/install-instance.sh" "${args[@]}"; then
    rm -f "$SETUP_RUN_DIR/settings.json"
    say "Could not start the setup as a background service; running it in this session instead."
    return 1
  fi
}

follow_setup() {
  local pid code i
  pid="$(systemctl show -p MainPID --value "$SETUP_UNIT.service" 2>/dev/null || true)"
  if [[ "$pid" =~ ^[1-9][0-9]*$ ]]; then
    tail -n +1 -s 0.5 -F --pid="$pid" "$SETUP_LOG" 2>/dev/null || true
  else
    cat "$SETUP_LOG" 2>/dev/null || true
  fi
  for i in $(seq 1 20); do
    if [ -s "$SETUP_RUN_DIR/status" ] || ! setup_running; then break; fi
    sleep 0.5
  done
  code="$(cat "$SETUP_RUN_DIR/status" 2>/dev/null || true)"
  rm -f "$SETUP_RUN_DIR/settings.json"
  if ! [[ "$code" =~ ^[0-9]+$ ]]; then
    say "Error: the setup on this server stopped without finishing. See: journalctl -u $SETUP_UNIT"
    exit 1
  fi
  exit "$code"
}

if can_detach; then
  install -d -m 0700 "$SETUP_RUN_DIR"
  exec 8>"$SETUP_RUN_DIR/start.lock"
  flock -w 60 8 || true
  if setup_running; then
    exec 8>&-
    RAW_CONFIG=""
    ADMIN_PASSWORD=""
    OWNERS_JSON="[]"
    say "A setup started earlier is still running on this server; following it from its first line."
    say "The details just sent were not used. When it ends, start again to change them; finished steps are skipped."
    follow_setup
  fi
  if start_detached; then
    exec 8>&-
    RAW_CONFIG=""
    ADMIN_PASSWORD=""
    OWNERS_JSON="[]"
    say "The setup runs on this server as the $SETUP_UNIT service, so it keeps going if this connection drops."
    say "Start again with the same details to follow it; its log is $SETUP_LOG."
    follow_setup
  fi
  exec 8>&-
fi
RAW_CONFIG=""

files_digest() { { cat "$@" 2>/dev/null || true; } | sha256sum | cut -c1-64; }
lines() {
  local line
  while IFS= read -r line || [ -n "$line" ]; do
    line="${line//$'\r'/ }"
    if [ -n "${line//[[:space:]]/}" ]; then printf '%s\n' "${line:0:400}"; fi
  done
}

apt_lines() {
  local line total="?" got=0 set_up=0 waited=0
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      *"newly installed"*)
        total="${line#*, }"; total="${total%% newly*}"
        say "$line" ;;
      "Need to get "*) say "$line" ;;
      Get:*)
        got=$((got + 1))
        if [ $((got % 50)) -eq 0 ]; then say "Downloaded $got packages"; fi ;;
      "Setting up "*)
        set_up=$((set_up + 1))
        if [ $((set_up % 25)) -eq 0 ] || [ "$set_up" = "$total" ]; then say "Set up $set_up of $total packages"; fi ;;
      *"Waiting for cache lock"*)
        if [ "$waited" = "0" ]; then waited=1; say "Another program is installing packages; waiting for it to finish."; fi ;;
      E:*|W:*|Err:*) say "${line:0:400}" ;;
    esac
  done
}

pip_lines() {
  local line kept=0 added
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in
      Collecting*|ERROR*|error:*) say "${line:0:400}" ;;
      "Requirement already satisfied"*) kept=$((kept + 1)) ;;
      "Successfully installed "*)
        added="${line#Successfully installed }"
        set -- $added
        say "Installed $# Python packages." ;;
    esac
  done
  if [ "$kept" -gt 0 ]; then say "$kept Python packages were already installed."; fi
}

missing_packages() {
  local name
  for name in "${PACKAGES[@]}"; do
    dpkg-query -W -f='${Status}' "$name" 2>/dev/null | grep -q ' installed$' || printf '%s ' "$name"
  done
}

check_system() {
  begin system "Checking this server"
  . /etc/os-release
  local major="${VERSION_ID%%.*}"
  case "${ID:-}" in
    debian) [ "${major:-0}" -ge 13 ] 2>/dev/null || die "ForkMesh needs Debian 13 (trixie) or newer; this is Debian ${VERSION_ID:-?}." 20 ;;
    ubuntu) [ "${major:-0}" -ge 24 ] 2>/dev/null || die "ForkMesh needs Ubuntu 24.04 or newer; this is Ubuntu ${VERSION_ID:-?}." 20 ;;
    *) die "ForkMesh instances run on Debian 13 or Ubuntu 24.04; this server runs ${PRETTY_NAME:-an unknown system}." 20 ;;
  esac
  case "$(uname -m)" in
    x86_64|aarch64) ;;
    *) die "This server's processor ($(uname -m)) is not supported; use x86_64 or arm64." 20 ;;
  esac
  local mem_kb disk_kb
  mem_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
  [ "${mem_kb:-0}" -ge 900000 ] || die "ForkMesh needs at least 1 GB of memory." 20
  disk_kb="$(df -Pk / | awk 'NR==2 {print $4}')"
  [ "${disk_kb:-0}" -ge 6000000 ] || die "ForkMesh needs at least 6 GB of free disk space." 20
  if command -v ss >/dev/null 2>&1; then
    local busy
    busy="$(ss -ltnpH '( sport = :80 or sport = :443 )' 2>/dev/null | grep -v caddy || true)"
    [ -z "$busy" ] || die "Another program already uses port 80 or 443 on this server. Stop it and try again." 21
  fi
  say "$PRETTY_NAME, $(uname -m), $((mem_kb / 1024)) MB memory, $((disk_kb / 1024 / 1024)) GB free."
}

capture_first_login_baseline() {
  [ "$NEW_INSTANCE" = "1" ] || return 0
  install -d -m 0700 "$CLEANUP_DIR"
  : > "$CLEANUP_DIR/new-instance"
  if [ ! -e "$CLEANUP_DIR/caddyfile.before" ] && [ ! -e "$CLEANUP_DIR/caddyfile.absent" ]; then
    if [ -e /etc/caddy/Caddyfile ]; then
      cp -p /etc/caddy/Caddyfile "$CLEANUP_DIR/caddyfile.before"
    else
      : > "$CLEANUP_DIR/caddyfile.absent"
    fi
  fi
  id -u "$APP_USER" >/dev/null 2>&1 || : > "$CLEANUP_DIR/created-app-user"
  id -u "$NODE_USER" >/dev/null 2>&1 || : > "$CLEANUP_DIR/created-node-user"
  id -u "$RUNNER_JOB_USER" >/dev/null 2>&1 || : > "$CLEANUP_DIR/created-job-user"
  [ -e "$RUNNER_ETC" ] || [ -e "$RUNNER_STATE" ] || : > "$CLEANUP_DIR/created-runner-root"
}

ensure_swap() {
  local mem_kb swap_kb
  mem_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
  swap_kb="$(awk '/^SwapTotal:/ {print $2}' /proc/meminfo)"
  if [ "${mem_kb:-0}" -lt 3000000 ] && [ "${swap_kb:-0}" -lt 1000000 ] && [ ! -e /swapfile-forkmesh ]; then
    say "Adding 2 GB of swap so the build fits in memory"
    fallocate -l 2G /swapfile-forkmesh 2>/dev/null || dd if=/dev/zero of=/swapfile-forkmesh bs=1M count=2048 status=none
    chmod 0600 /swapfile-forkmesh
    mkswap /swapfile-forkmesh >/dev/null
    swapon /swapfile-forkmesh
    grep -q '^/swapfile-forkmesh ' /etc/fstab || echo '/swapfile-forkmesh none swap sw 0 0' >> /etc/fstab
    [ "$NEW_INSTANCE" = "1" ] && : > "$CLEANUP_DIR/created-swap"
  fi
}

install_packages() {
  begin packages "Installing system packages"
  capture_first_login_baseline
  ensure_swap
  local missing
  missing="$(missing_packages)"
  if [ -z "$missing" ]; then
    say "The system packages are already installed."
  else
    say "Installing ${missing% }."
    stdbuf -oL -eL apt-get update -q -o DPkg::Lock::Timeout=900 2>&1 | apt_lines
    stdbuf -oL -eL apt-get install -y -q --no-install-recommends -o Dpkg::Use-Pty=0 \
      -o DPkg::Lock::Timeout=900 "${PACKAGES[@]}" 2>&1 | apt_lines
    missing="$(missing_packages)"
    [ -z "$missing" ] || die "These packages did not install: ${missing% }."
  fi
  say "Installed git, Python $(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])'), Node $(node --version 2>/dev/null || echo '?') and Caddy $(caddy version 2>/dev/null | awk '{print $1}')."
  if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q '^Status: active'; then
    if ! ufw status | grep -Eq '(^|[[:space:]])80/tcp([[:space:]]|$)'; then
      if ufw allow 80/tcp >/dev/null && [ "$NEW_INSTANCE" = "1" ]; then
        : > "$CLEANUP_DIR/ufw-80"
      fi
    fi
    if ! ufw status | grep -Eq '(^|[[:space:]])443/tcp([[:space:]]|$)'; then
      if ufw allow 443/tcp >/dev/null && [ "$NEW_INSTANCE" = "1" ]; then
        : > "$CLEANUP_DIR/ufw-443"
      fi
    fi
    say "Opened ports 80 and 443 in ufw."
  fi
  id -u "$APP_USER" >/dev/null 2>&1 ||
    useradd --system --home-dir "$VAR" --shell /usr/sbin/nologin "$APP_USER"
  id -u "$NODE_USER" >/dev/null 2>&1 ||
    useradd --system --home-dir /var/lib/forkmesh --shell /usr/sbin/nologin "$NODE_USER"
  install -d -m 0755 "$OPT"
  install -d -m 0750 -o root -g "$APP_USER" "$ETC"
  install -d -m 0750 -o "$APP_USER" -g "$APP_USER" "$VAR" "$VAR/data"
}

source_git() {
  local url="$1"
  shift
  case "$url" in
    "https://$DOMAIN/"*) git -c "http.curloptResolve=$DOMAIN:443:127.0.0.1" "$@" ;;
    *) git "$@" ;;
  esac
}

follow_source_url() {
  local current
  current="$(git -C "$SRC" remote get-url origin 2>/dev/null || true)"
  if [ "$current" != "$CFG_sourceUrl_effective" ]; then
    if [ -n "$current" ]; then
      git -C "$SRC" remote set-url origin "$CFG_sourceUrl_effective"
    else
      git -C "$SRC" remote add origin "$CFG_sourceUrl_effective"
    fi
    say "The source now follows $CFG_sourceUrl_effective (was ${current:-none})."
  fi
}

fetch_source() {
  begin source "Downloading the ForkMesh source"
  local local_source="${FORKMESH_INSTANCE_SOURCE_DIR:-}"
  if [ -n "$local_source" ]; then
    [ -d "$local_source/.git" ] || die "FORKMESH_INSTANCE_SOURCE_DIR is not a git checkout."
    CFG_sourceUrl_effective="$local_source"
  else
    CFG_sourceUrl_effective="$CFG_sourceUrl"
  fi
  if [ -d "$SRC/.git" ]; then
    follow_source_url
    say "Fetching $CFG_sourceRef from $CFG_sourceUrl_effective"
    source_git "$CFG_sourceUrl_effective" -C "$SRC" fetch --depth 1 origin "$CFG_sourceRef" 2>&1 | lines
    TARGET_COMMIT="$(git -C "$SRC" rev-parse FETCH_HEAD)"
    if [ "$(git -C "$SRC" rev-parse HEAD)" = "$TARGET_COMMIT" ]; then
      say "The source is already at the latest $CFG_sourceRef."
    else
      say "New source: $(git -C "$SRC" log -1 --format='%h (%cs) %s' "$TARGET_COMMIT" | cut -c1-200)"
    fi
  else
    local stage="$OPT/src.partial"
    [ ! -e "$stage" ] || mv "$stage" "$OPT/src.partial.$(date +%s)"
    say "Cloning $CFG_sourceRef from $CFG_sourceUrl_effective"
    git clone --quiet --depth 1 --branch "$CFG_sourceRef" "$CFG_sourceUrl_effective" "$stage" ||
      die "Could not download the ForkMesh source from $CFG_sourceUrl_effective."
    mv "$stage" "$SRC"
    TARGET_COMMIT="$(git -C "$SRC" rev-parse HEAD)"
  fi
  say "ForkMesh source at $(git -C "$SRC" rev-parse --short "$TARGET_COMMIT") ($(git -C "$SRC" log -1 --format=%cs "$TARGET_COMMIT"))."
}

checkout_target() {
  local tree="$1"
  if [ "$(git -C "$tree" rev-parse HEAD 2>/dev/null)" = "$TARGET_COMMIT" ] && git -C "$tree" diff --quiet; then
    return 0
  fi
  if ! git -C "$tree" diff --quiet; then
    local saved
    saved="$OPT/src-changes-$(date +%Y%m%d-%H%M%S).patch"
    git -C "$tree" diff > "$saved"
    say "Saved the changes in $tree (including files the last build rewrote) to $saved."
  fi
  git -C "$tree" checkout -f -q --detach "$TARGET_COMMIT"
}

requirements_digest() { files_digest "$1/app/host/requirements.txt"; }

stage_venv() {
  BUILD_VENV="$VENV"
  if [ "$(cat "$VENV/.forkmesh-requirements" 2>/dev/null || true)" = "$(requirements_digest "$BUILD_SRC")" ]; then
    return 0
  fi
  if [ -e "$NEXT_VENV" ] && [ ! -x "$NEXT_VENV/bin/python" ]; then mv "$NEXT_VENV" "$NEXT_VENV.broken-$(date +%s)"; fi
  if [ ! -e "$NEXT_VENV" ] && [ -x "$PREV_VENV/bin/python" ]; then mv "$PREV_VENV" "$NEXT_VENV"; fi
  if [ ! -e "$NEXT_VENV" ]; then
    [ ! -e "$NEXT_VENV.partial" ] || mv "$NEXT_VENV.partial" "$NEXT_VENV.partial.$(date +%s)"
    cp -a "$VENV" "$NEXT_VENV.partial"
    mv "$NEXT_VENV.partial" "$NEXT_VENV"
  fi
  BUILD_VENV="$NEXT_VENV"
  say "The Python packages changed; installing them beside the running ones."
}

stage_source() {
  if [ -e "$NEXT_SRC" ] && [ ! -d "$NEXT_SRC/.git" ]; then mv "$NEXT_SRC" "$NEXT_SRC.broken-$(date +%s)"; fi
  if [ ! -e "$NEXT_SRC" ] && [ -d "$PREV_SRC/.git" ]; then mv "$PREV_SRC" "$NEXT_SRC"; fi
  if [ ! -e "$NEXT_SRC" ]; then
    say "Copying the running source to build the new one beside it"
    [ ! -e "$NEXT_SRC.partial" ] || mv "$NEXT_SRC.partial" "$NEXT_SRC.partial.$(date +%s)"
    cp -a "$SRC" "$NEXT_SRC.partial"
    mv "$NEXT_SRC.partial" "$NEXT_SRC"
  fi
  git -C "$SRC" update-ref refs/forkmesh/build "$TARGET_COMMIT"
  git -C "$NEXT_SRC" fetch --quiet --depth 1 "$SRC" refs/forkmesh/build 2>&1 | lines
  git -C "$NEXT_SRC" remote set-url origin "$CFG_sourceUrl_effective" 2>/dev/null ||
    git -C "$NEXT_SRC" remote add origin "$CFG_sourceUrl_effective"
  checkout_target "$NEXT_SRC"
  BUILD_SRC="$NEXT_SRC"
  say "Building $(git -C "$BUILD_SRC" rev-parse --short HEAD) in $BUILD_SRC; the running version keeps serving until it is ready."
  stage_venv
}

activate_source() {
  [ "$BUILD_SRC" != "$SRC" ] || return 0
  [ ! -e "$PREV_SRC" ] || mv "$PREV_SRC" "$PREV_SRC.$(date +%s)"
  [ ! -e "$PREV_VENV" ] || [ "$BUILD_VENV" = "$VENV" ] || mv "$PREV_VENV" "$PREV_VENV.$(date +%s)"
  if [ -f "$BUILD_INFO" ]; then cp -p "$BUILD_INFO" "$BUILD_INFO.prev"; else rm -f "$BUILD_INFO.prev"; fi
  if [ -f "$BUILD_STAMP" ]; then cp -p "$BUILD_STAMP" "$BUILD_STAMP.prev"; else rm -f "$BUILD_STAMP.prev"; fi
  SWAPPED="1"
  mv "$SRC" "$PREV_SRC"
  mv "$NEXT_SRC" "$SRC"
  BUILD_SRC="$SRC"
  if [ "$BUILD_VENV" != "$VENV" ]; then
    VENV_SWAPPED="1"
    mv "$VENV" "$PREV_VENV"
    mv "$NEXT_VENV" "$VENV"
    BUILD_VENV="$VENV"
  fi
  write_build_info
  build_stamp "$SRC" > "$BUILD_STAMP"
  say "Switched to the new build; the previous one is kept in $PREV_SRC until the next update."
}

rollback_source() {
  [ "$SWAPPED" = "1" ] && [ "$SOURCE_COMMITTED" != "1" ] && [ -d "$PREV_SRC/.git" ] || return 0
  SWAPPED="0"
  if [ -e "$SRC" ]; then
    [ ! -e "$NEXT_SRC" ] || mv "$NEXT_SRC" "$NEXT_SRC.failed-$(date +%s)"
    mv "$SRC" "$NEXT_SRC"
  fi
  mv "$PREV_SRC" "$SRC"
  if [ "$VENV_SWAPPED" = "1" ] && [ -x "$PREV_VENV/bin/python" ]; then
    VENV_SWAPPED="0"
    if [ -e "$VENV" ]; then
      [ ! -e "$NEXT_VENV" ] || mv "$NEXT_VENV" "$NEXT_VENV.failed-$(date +%s)"
      mv "$VENV" "$NEXT_VENV"
    fi
    mv "$PREV_VENV" "$VENV"
  fi
  if [ -f "$BUILD_INFO.prev" ]; then mv -f "$BUILD_INFO.prev" "$BUILD_INFO"; fi
  if [ -f "$BUILD_STAMP.prev" ]; then mv -f "$BUILD_STAMP.prev" "$BUILD_STAMP"; fi
  if [ "$APP_RESTARTED" = "1" ]; then systemctl restart forkmesh-instance.service >/dev/null 2>&1 || true; fi
  say "Kept the version that was running: $(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown). The failed build is in $NEXT_SRC."
}

logo_file() {
  local ext
  for ext in png jpg webp svg; do
    if [ -s "$ETC/logo.$ext" ]; then printf '%s\n' "$ETC/logo.$ext"; return 0; fi
  done
}

save_logo() {
  if [ -z "$CFG_logoType" ]; then return 0; fi
  local ext
  printf '%s' "$CFG_logoData" | base64 -d > "$ETC/logo.$CFG_logoType.new" ||
    die "Your logo could not be saved."
  chmod 0644 "$ETC/logo.$CFG_logoType.new"
  for ext in png jpg webp svg; do
    if [ "$ext" != "$CFG_logoType" ]; then rm -f "$ETC/logo.$ext"; fi
  done
  mv "$ETC/logo.$CFG_logoType.new" "$ETC/logo.$CFG_logoType"
  CFG_logoData=""
  say "Saved your logo."
}

build_stamp() {
  local tree="${1:-$SRC}"
  printf '%s\n' "$(git -C "$tree" rev-parse HEAD)" "$INSTANCE_INSTALLER_VERSION" "$EXCLUDED_PACKAGES" "$CFG_projectName" \
    "$(files_digest "$tree/app/host/requirements.txt")" "$(logo_file)" "$(files_digest "$(logo_file)")" "${CFG_poweredBy:-1}"
}

release_version() {
  sed -n 's/^project(ForkMesh VERSION \([0-9][0-9.]*\).*/\1/p' "$SRC/desktop/CMakeLists.txt" 2>/dev/null | head -n1 || true
}

write_build_info() {
  git -C "$SRC" log -1 --format='%H%n%ct%n%s' | python3 -c '
import json, sys
commit, stamp, subject = (sys.stdin.read().split("\n", 2) + ["", "", ""])[:3]
print(json.dumps({"version": sys.argv[1], "commit": commit.strip(),
                  "commitTime": int(stamp.strip() or 0), "subject": subject.strip()[:500],
                  "source": sys.argv[2], "ref": sys.argv[3], "builtAt": int(sys.argv[4])},
                 sort_keys=True))
' "$(release_version)" "$CFG_sourceUrl" "$CFG_sourceRef" "$(date +%s)" > "$BUILD_INFO.new"
  chmod 0644 "$BUILD_INFO.new"
  mv -f "$BUILD_INFO.new" "$BUILD_INFO"
  write_daemon_state
}

write_daemon_state() {
  [ -s "$BUILD_INFO" ] || return 0
  python3 -c '
import json, os, sys
path, record, tree, source, ref = sys.argv[1:6]
try:
    with open(path, encoding="utf-8") as handle:
        info = json.load(handle)
except (OSError, ValueError):
    sys.exit(0)
try:
    with open(record, encoding="utf-8") as handle:
        built = json.load(handle)
except (OSError, ValueError):
    built = {}
if not isinstance(built, dict):
    built = {}
built_tree = str(built.get("serverTree") or "")
origin = str(built.get("from") or "")
if origin == "release":
    state = "release"
elif not built_tree or not tree:
    state = "unknown"
elif built_tree == tree:
    state = "current"
else:
    state = "stale"
info["daemon"] = {"state": state, "builtTree": built_tree, "sourceTree": tree,
                  "builtCommit": str(built.get("commit") or "")}
info["source"], info["ref"] = source, ref
with open(path + ".new", "w", encoding="utf-8") as handle:
    json.dump(info, handle, sort_keys=True)
os.chmod(path + ".new", 0o644)
os.replace(path + ".new", path)
' "$BUILD_INFO" "$DAEMON_BUILD" "$(git -C "$SRC" rev-parse HEAD:server 2>/dev/null || true)" \
    "$CFG_sourceUrl" "$CFG_sourceRef"
}

build_app() {
  begin build "Building the app (this is the longest step)"
  save_logo
  if [ -x "$VENV/bin/python" ] && [ -d "$SRC/app/dist" ] && [ -s "$BUILD_STAMP" ] &&
      [ "$(git -C "$SRC" rev-parse HEAD)" = "$TARGET_COMMIT" ] &&
      [ "$(cat "$BUILD_STAMP")" = "$(build_stamp "$SRC")" ]; then
    say "The app is already built from $(git -C "$SRC" rev-parse --short HEAD) for $CFG_projectName."
    if [ -s "$BUILD_INFO" ]; then write_daemon_state; else write_build_info; fi
    return 0
  fi
  APP_CHANGED="1"
  if [ -x "$VENV/bin/python" ] && [ -d "$SRC/app/dist" ]; then
    stage_source
  else
    rm -f "$BUILD_STAMP"
    checkout_target "$SRC"
  fi
  if [ ! -x "$BUILD_VENV/bin/python" ]; then
    python3 -m venv "$BUILD_VENV"
  fi
  if [ "$(cat "$BUILD_VENV/.forkmesh-requirements" 2>/dev/null || true)" != "$(requirements_digest "$BUILD_SRC")" ]; then
    say "Installing the Python packages"
    "$BUILD_VENV/bin/python" -m pip install --disable-pip-version-check --progress-bar off -r "$BUILD_SRC/app/host/requirements.txt" 2>&1 |
      pip_lines || die "The Python packages did not install. The lines above show why."
    requirements_digest "$BUILD_SRC" > "$BUILD_VENV/.forkmesh-requirements"
  fi
  say "Python packages are ready."
  (
    cd "$BUILD_SRC/app/host"
    FORKMESH_EXCLUDED_PLUGINS="$EXCLUDED_PACKAGES" "$BUILD_VENV/bin/python" -m forkmesh_host build 2>&1 | lines
    cd "$BUILD_SRC/app"
    "$BUILD_VENV/bin/python" tools/build_site_assets.py instance-world 2>&1 | lines
  )
  local brand_args=(--name "$CFG_projectName" --dist "$BUILD_SRC/app/dist") logo
  logo="$(logo_file)"
  if [ -n "$logo" ]; then brand_args+=(--logo "$logo"); fi
  if [ "$CFG_poweredBy" = "0" ]; then brand_args+=(--no-powered-by); fi
  "$BUILD_VENV/bin/python" "$BUILD_SRC/app/tools/apply_instance_brand.py" "${brand_args[@]}"
  chmod -R a+rX "$BUILD_SRC" "$BUILD_VENV"
  if [ "$BUILD_SRC" = "$SRC" ]; then
    write_build_info
    build_stamp "$SRC" > "$BUILD_STAMP"
  fi
}

platform_source() {
  if [ -n "${CFG_platformUrl:-}" ]; then
    printf '%s\n' "${CFG_platformUrl%.git}"
  else
    case "$CFG_sourceUrl" in
      "https://$DOMAIN/"*) printf '%s\n' "$RELEASE_SOURCE" ;;
      *) printf '%s\n' "${CFG_sourceUrl%.git}" ;;
    esac
  fi
}

upstream_page() {
  local url
  url="$(platform_source)"
  url="${url%/}"
  printf '%s\n' "$url" | sed -E 's#^https://app\.#https://#'
}

resolve_runner_name() {
  local kept
  kept="$(env_value ACTIONS_INSTANCE_RUNNER)"
  if [[ "$kept" =~ ^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$ ]]; then
    RUNNER_NAME="$kept"
  elif [[ "$CFG_ownerName-runner" =~ ^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$ ]]; then
    RUNNER_NAME="$CFG_ownerName-runner"
  else
    RUNNER_NAME="instance-runner"
  fi
}

env_value() {
  [ -r "$APP_ENV" ] || return 0
  awk -F= -v key="$1" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$APP_ENV"
}

new_admin_path() {
  python3 -c '
import secrets
alphabet = "abcdefghijklmnopqrstuvwxyz0123456789"
chars = []
while len(chars) < 20:
    for byte in secrets.token_bytes(32):
        if byte < 252 and len(chars) < 20:
            chars.append(alphabet[byte % 36])
print("/admin-" + "".join(chars))
'
}

choose_admin_path() {
  local existing
  existing="$(env_value ADMIN_PATH)"
  ADMIN_PATH_VALUE=""
  ADMIN_PATH_SOURCE=""
  if [ -n "$existing" ]; then
    ADMIN_PATH_VALUE="$existing"
    ADMIN_PATH_SOURCE="kept"
    if [ -n "${CFG_adminPath:-}" ] && [ "$CFG_adminPath" != "$existing" ]; then
      ADMIN_PATH_SOURCE="kept-other"
    fi
  elif [ "$CONFIG_MODE" = "update" ]; then
    return 0
  elif [ -n "${CFG_adminPath:-}" ]; then
    ADMIN_PATH_VALUE="$CFG_adminPath"
    ADMIN_PATH_SOURCE="settings"
  elif [ "$NEW_INSTANCE" = "1" ]; then
    ADMIN_PATH_VALUE="$(new_admin_path)"
    ADMIN_PATH_SOURCE="generated"
  fi
}

setup_log_private() {
  [ -f "$SETUP_LOG" ] || return 1
  local owner mode
  owner="$(stat -c %u "$SETUP_LOG" 2>/dev/null || echo x)"
  mode="$(stat -c %a "$SETUP_LOG" 2>/dev/null || echo 777)"
  [ "$owner" = "0" ] && [ $(( 8#$mode & 8#077 )) -eq 0 ]
}

say_admin_path() {
  case "$ADMIN_PATH_SOURCE" in
    generated)
      if [ "$DETACHED" = "1" ] && ! setup_log_private; then
        say "Admin console: a private address was created; it is ADMIN_PATH in $APP_ENV."
      else
        say "Admin console: $ORIGIN$ADMIN_PATH_VALUE"
        say "Save this address now: it is the only way into the admin console (also ADMIN_PATH in $APP_ENV)."
      fi ;;
    settings) say "Admin console: the private address from your settings (adminPath); save it." ;;
    kept-other) say "Admin console: this server already had a private address, so it was kept (ADMIN_PATH in $APP_ENV)." ;;
  esac
}

write_settings() {
  begin configure "Saving your settings"
  resolve_runner_name
  local data_key admin_pass seed public_key mail_key env_before login_deadline bot_seed
  env_before="$(files_digest "$APP_ENV")"
  data_key="$(env_value DATA_KEY)"
  admin_pass="$(env_value ADMIN_PASS)"
  seed="$(env_value MIRROR_ROUTER_SIGNING_SEED)"
  public_key="$(env_value MIRROR_ROUTER_PUBLIC_KEY)"
  login_deadline="$(env_value INSTANCE_LOGIN_DEADLINE)"
  bot_seed="$(env_value ACTIONS_BOT_SEED)"
  choose_admin_path
  [[ "$bot_seed" =~ ^[0-9a-f]{64}$ ]] || bot_seed=""
  if [ -z "$bot_seed" ] && [ "$CFG_actions" = "1" ]; then
    bot_seed="$(openssl rand -hex 32)"
  fi
  [ -n "$data_key" ] || data_key="$(openssl rand -base64 48 | tr '+/' '-_' | tr -d '=\n')"
  [ -n "$admin_pass" ] || admin_pass="$(openssl rand -base64 24 | tr '+/' '-_' | tr -d '=\n')"
  if [ -z "$seed" ] || [ -z "$public_key" ]; then
    read -r seed public_key < <("$VENV/bin/python" -c '
import base64, secrets
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization
seed = secrets.token_bytes(32)
public = Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(
    serialization.Encoding.Raw, serialization.PublicFormat.Raw)
encode = lambda b: base64.urlsafe_b64encode(b).decode().rstrip("=")
print(encode(seed), encode(public))
')
  fi
  mail_key="$CFG_mailApiKey"
  if [ -z "$mail_key" ]; then
    for name in MAILTRAP_API_TOKEN SENDGRID_API_KEY MAILGUN_API_KEY; do
      [ -n "$mail_key" ] || mail_key="$(env_value "$name")"
    done
  fi
  local mail_key_name=""
  case "$CFG_mailProvider" in
    mailtrap) mail_key_name="MAILTRAP_API_TOKEN" ;;
    sendgrid) mail_key_name="SENDGRID_API_KEY" ;;
    mailgun) mail_key_name="MAILGUN_API_KEY" ;;
  esac
  local tmp="$APP_ENV.new"
  : > "$tmp"
  chmod 0640 "$tmp"
  chown root:"$APP_USER" "$tmp"
  {
    echo "FORKMESH_PLATFORM=vps"
    echo "INSTANCE_NAME=$CFG_projectName"
    echo "INSTANCE_PLUGINS=$CFG_plugins"
    echo "INSTANCE_FLAGSHIP_NODE=$NODE_NAME"
    echo "INSTANCE_OWNER=$CFG_ownerName"
    echo "INSTANCE_UPSTREAM=$(upstream_page)"
    echo "INSTANCE_POWERED_BY=$CFG_poweredBy"
    echo "INSTANCE_LOGIN_MARKER=$LOGIN_MARKER"
    echo "INSTANCE_LOGIN_DEADLINE=$login_deadline"
    echo "INSTANCE_UPDATE_REQUEST=$UPDATE_REQUEST_DIR/request"
    echo "INSTANCE_UPDATE_STATUS=$UPDATE_STATE/status.json"
    echo "BUILD_INFO_FILE=$BUILD_INFO"
    echo "BUILD_REV=$(git -C "$SRC" rev-parse HEAD)"
    echo "APP_VERSION=$(release_version)"
    if [ -s "$SRC/app/dist/instance-logo.png" ] && [ -n "$(logo_file)" ]; then echo "INSTANCE_LOGO=/instance-logo.png"
    elif [ -n "$(logo_file)" ]; then echo "INSTANCE_LOGO=/instance-logo.$(logo_file | sed 's/.*\.//')"; fi
    echo "PUBLIC_BASE_URL=$ORIGIN"
    echo "API_ORIGIN=$ORIGIN"
    echo "APP_ORIGIN=$ORIGIN"
    echo "WWW_ORIGIN=$ORIGIN"
    echo "WORLD_ORIGIN=$ORIGIN"
    echo "INSTANCE_DIRECTORY_URL=$DIRECTORY_URL"
    echo "CORS_ALLOWED_ORIGINS=$ORIGIN"
    echo "NODE_NAME=$NODE_NAME"
    echo "HOSTED_GIT_CORE_NODES=$NODE_NAME"
    echo "HOSTED_IMPORT_HOSTS=github.com,gitlab.com,codeberg.org,app.forkmesh.com"
    echo "HOSTED_REPO_MAX_BYTES=1073741824"
    if [ "$CFG_actions" = "1" ]; then
      echo "RUNNER_POOL_TARGET_NODES=$NODE_NAME"
      echo "ACTIONS_INSTANCE_RUNNER=$RUNNER_NAME"
    else
      echo "RUNNER_POOL_TARGET_NODES="
    fi
    if [ -n "$bot_seed" ]; then echo "ACTIONS_BOT_SEED=$bot_seed"; fi
    echo "MAIL_LOG_OWNER=$CFG_adminUsername"
    echo "ADMIN_ALERT_EMAILS=$CFG_adminEmail"
    echo "SECURITY_EMAIL=$CFG_adminEmail"
    echo "MAIL_TRANSPORT=http"
    echo "MAIL_PROVIDER=$CFG_mailProvider"
    echo "MAIL_SENDER=${CFG_mailFrom:-no-reply@$DOMAIN}"
    echo "MAIL_SENDER_NAME=$CFG_projectName"
    if [ -n "$mail_key_name" ] && [ -n "$mail_key" ]; then echo "$mail_key_name=$mail_key"; fi
    if [ "$CFG_mailProvider" = "mailgun" ]; then
      echo "MAILGUN_DOMAIN=$CFG_mailgunDomain"
      echo "MAILGUN_REGION=${CFG_mailgunRegion:-us}"
    fi
    echo "DATA_KEY=$data_key"
    echo "ADMIN_PASS=$admin_pass"
    if [ -n "$ADMIN_PATH_VALUE" ]; then echo "ADMIN_PATH=$ADMIN_PATH_VALUE"; fi
    echo "MIRROR_ROUTER_SIGNING_SEED=$seed"
    echo "MIRROR_ROUTER_PUBLIC_KEY=$public_key"
    if [ -r "$APP_ENV" ]; then
      grep -E '^(TURNSTILE_SITE_KEY|TURNSTILE_SECRET_KEY|HOSTED_GIT_CONTRIBUTOR_PUSH|SSH_GATEWAY_HOST|SSH_GATEWAY_PORT|SSH_GATEWAY_TOKEN|SSH_GATEWAY_REPOSITORIES|SSH_GATEWAY_NODE_HOSTS|SSH_GATEWAY_TOKEN_NEXT|SSH_GATEWAY_ALLOWED_ADDRESSES)=' "$APP_ENV" || true
    fi
    if [ -r "$APP_ENV" ] && grep -qE '^TURNSTILE_HOSTNAMES=.' "$APP_ENV"; then
      grep -E '^TURNSTILE_HOSTNAMES=' "$APP_ENV"
    else
      echo "TURNSTILE_HOSTNAMES=$DOMAIN"
    fi
  } >> "$tmp"
  mv "$tmp" "$APP_ENV"
  if [ "$(files_digest "$APP_ENV")" != "$env_before" ]; then APP_CHANGED="1"; fi
  ROUTER_PUBLIC_KEY="$public_key"
  python3 -c '
import json, sys
keys = ["projectName", "domain", "domainProvided", "publicAddress", "adminUsername", "adminEmail",
        "ownerType", "ownerName", "mailProvider", "mailFrom", "mailgunDomain", "mailgunRegion",
        "sourceUrl", "sourceRef", "plugins", "actions", "poweredBy", "platformUrl"]
values = dict(zip(keys, sys.argv[1:]))
if not values["platformUrl"]:
    del values["platformUrl"]
values["poweredBy"] = values["poweredBy"] == "1"
values["domainProvided"] = values["domainProvided"] == "1"
values["actions"] = values["actions"] == "1"
values["plugins"] = [p for p in values["plugins"].split(",") if p]
values["installerVersion"] = sys.argv[-1]
print(json.dumps(values, indent=2, sort_keys=True))
' "$CFG_projectName" "$CFG_domain" "$CFG_domainProvided" "$CFG_publicAddress" "$CFG_adminUsername" \
    "$CFG_adminEmail" "$CFG_ownerType" "$CFG_ownerName" "$CFG_mailProvider" "$CFG_mailFrom" \
    "$CFG_mailgunDomain" "$CFG_mailgunRegion" "$CFG_sourceUrl" "$CFG_sourceRef" "$CFG_plugins" \
    "$CFG_actions" "$CFG_poweredBy" "${CFG_platformUrl:-}" "$INSTANCE_INSTALLER_VERSION" > "$INSTANCE_JSON.new"
  chmod 0640 "$INSTANCE_JSON.new"
  mv "$INSTANCE_JSON.new" "$INSTANCE_JSON"
  say "Settings saved to $APP_ENV (only root and the app can read it)."
  if [ "$CFG_mailProvider" = "none" ]; then
    say "No email provider: the instance runs without sending email. Add one later in $APP_ENV."
  else
    say "Email goes out through $CFG_mailProvider as ${CFG_mailFrom:-no-reply@$DOMAIN}."
  fi
}

host_cli() {
  runuser -u "$APP_USER" -- env HOME="$VAR" "$VENV/bin/python" -m forkmesh_host "$@" \
    --env-file "$APP_ENV" --db "sqlite:$VAR/app.db" --data-dir "$VAR/data"
}

migrate_database() {
  begin database "Creating the database"
  say "Applying the database migrations"
  local result
  if ! result="$(cd "$SRC/app/host" && host_cli migrate 2>&1)"; then
    printf '%s\n' "$result" | tail -n 15 | lines
    die "The database migrations failed. The lines above show why."
  fi
  printf '%s\n' "$result" | tail -n 2 | lines
  case "$result" in
    *"migrations: 0 applied"*) ;;
    *) APP_CHANGED="1" ;;
  esac
}

write_app_unit() {
  cat > "$APP_UNIT" <<EOF
[Unit]
Description=ForkMesh instance $CFG_projectName ($DOMAIN)
Wants=network-online.target
After=network-online.target

[Service]
Type=simple
User=$APP_USER
Group=$APP_USER
WorkingDirectory=$SRC/app/host
Environment=HOME=$VAR
Environment=FORKMESH_PUBLIC_HOSTS=$DOMAIN
Environment=FORKMESH_HOST_REPO_GATEWAY=http://127.0.0.1:$GATEWAY_PORT
Environment=FORKMESH_EGRESS_PRODUCTION_ALLOW=$DIRECTORY_HOST
Environment=MALLOC_ARENA_MAX=2
Environment=PYTHONUNBUFFERED=1
ExecStart=$VENV/bin/python -m forkmesh_host serve --no-build --host 127.0.0.1 --port $APP_PORT --env-file $APP_ENV --db sqlite:$VAR/app.db --data-dir $VAR/data
Restart=on-failure
RestartSec=3s
TimeoutStopSec=20s
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=$VAR
UMask=0027

[Install]
WantedBy=multi-user.target
EOF
}

write_updater() {
  [ -f "$SRC/tools/instance-update.sh" ] || return 0
  install -d -m 0755 "$UPDATE_LIB"
  install -m 0700 "$SRC/tools/instance-update.sh" "$UPDATE_LIB/instance-update.sh.new"
  mv -f "$UPDATE_LIB/instance-update.sh.new" "$UPDATE_LIB/instance-update.sh"
  install -d -m 2750 -o root -g "$APP_USER" "$UPDATE_STATE"
  install -d -m 0750 -o "$APP_USER" -g "$APP_USER" "$UPDATE_REQUEST_DIR"
  cat > "$UPDATE_UNIT" <<EOF
[Unit]
Description=Update ForkMesh when an admin asks from the admin console

[Service]
Type=oneshot
ExecStart=/bin/bash $UPDATE_LIB/instance-update.sh
TimeoutStartSec=90min
EOF
  cat > "$UPDATE_PATH_UNIT" <<EOF
[Unit]
Description=Watch for update requests from the ForkMesh admin console

[Path]
PathExists=$UPDATE_REQUEST_DIR/request
Unit=forkmesh-instance-update.service

[Install]
WantedBy=paths.target
EOF
  systemctl daemon-reload
  systemctl enable forkmesh-instance-update.path >/dev/null 2>&1 || true
  systemctl start forkmesh-instance-update.path >/dev/null 2>&1 || true
}

start_app() {
  begin services "Starting the services"
  local unit_before
  unit_before="$(files_digest "$APP_UNIT")"
  write_updater
  write_app_unit
  if [ "$APP_CHANGED" = "0" ] && [ "$(files_digest "$APP_UNIT")" = "$unit_before" ] &&
      systemctl is-active --quiet forkmesh-instance.service &&
      curl -fs -o /dev/null --max-time 3 "http://127.0.0.1:$APP_PORT/readyz"; then
    say "The app is already running this version with these settings."
    return 0
  fi
  systemctl daemon-reload
  systemctl enable forkmesh-instance.service >/dev/null 2>&1
  APP_RESTARTED="1"
  systemctl restart forkmesh-instance.service
  local i
  for i in $(seq 1 90); do
    if curl -fs -o /dev/null --max-time 3 "http://127.0.0.1:$APP_PORT/readyz"; then
      say "The app is answering on this server."
      return 0
    fi
    sleep 2
  done
  journalctl -u forkmesh-instance --no-pager -n 25 2>/dev/null || true
  die "The app did not start. The lines above show why."
}

write_caddyfile() {
  if [ -f /etc/caddy/Caddyfile ] && ! grep -q 'managed by install-instance.sh' /etc/caddy/Caddyfile; then
    cp /etc/caddy/Caddyfile "/etc/caddy/Caddyfile.before-forkmesh.$(date +%s)"
  fi
  cat > /etc/caddy/Caddyfile <<EOF
# managed by install-instance.sh for $DOMAIN
{
	email $CFG_adminEmail
}

$DOMAIN {
$( [ "${FORKMESH_INSTANCE_TLS:-}" = "internal" ] && printf '\ttls internal\n' )
	encode zstd gzip
	header {
		defer
		Strict-Transport-Security "max-age=31536000; includeSubDomains"
	}
	@gateway path /v1/* /health /forkmesh-mirror.json /.well-known/forkmesh-mirror.json
	@cloudflare remote_ip $CLOUDFLARE_RANGES
	handle @gateway {
		reverse_proxy 127.0.0.1:$GATEWAY_PORT
	}
	handle @cloudflare {
		reverse_proxy 127.0.0.1:$APP_PORT {
			header_up -X-Forwarded-For
			header_up -X-Forwarded-Host
		}
	}
	handle {
		reverse_proxy 127.0.0.1:$APP_PORT {
			header_up Cf-Connecting-Ip {remote_host}
			header_up -Cf-Ipcountry
			header_up -X-Forwarded-For
			header_up -X-Forwarded-Host
		}
	}
}
EOF
}

dns_record_type() {
  case "$CFG_publicAddress" in *:*) printf 'AAAA' ;; *) printf 'A' ;; esac
}

dns_answers() {
  local type url
  type="$(dns_record_type)"
  {
    for url in "https://cloudflare-dns.com/dns-query" "https://dns.google/resolve"; do
      curl -fsS --max-time 5 -H 'accept: application/dns-json' "$url?name=$DOMAIN&type=$type" 2>/dev/null |
        python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
except ValueError:
    sys.exit(0)
for answer in (data.get("Answer") or []) if isinstance(data, dict) else []:
    if isinstance(answer, dict) and answer.get("type") in (1, 28):
        print(str(answer.get("data") or "").strip())
' 2>/dev/null || true
    done
    getent ahosts "$DOMAIN" 2>/dev/null | awk '{print $1}' || true
  } | tr 'A-F' 'a-f' | grep -Ex '[0-9a-f:.]{3,45}' | grep -Ev '^(127\.|::1$|::ffff:127\.)' | sort -u || true
}

server_addresses() {
  printf '%s\n' "$CFG_publicAddress"
  ip -o addr show scope global 2>/dev/null | awk '{sub(/\/.*/, "", $4); print $4}' || true
  curl -fsS --max-time 5 https://cloudflare.com/cdn-cgi/trace 2>/dev/null | sed -n 's/^ip=//p' || true
}

resolves_here() {
  local answers
  answers="$(dns_answers)"
  [ -n "$answers" ] || return 1
  server_addresses | tr 'A-F' 'a-f' | grep -Fxq -f <(printf '%s\n' "$answers")
}

behind_cloudflare() {
  curl -fsS --max-time 5 "http://$DOMAIN/cdn-cgi/trace" 2>/dev/null | tr -d '\r' | grep -Fxq "h=$DOMAIN"
}

dns_ready() {
  if resolves_here; then
    PROXIED="0"
    return 0
  fi
  if behind_cloudflare; then
    PROXIED="1"
    return 0
  fi
  return 1
}

dns_wait_message() {
  local answers
  answers="$(dns_answers | paste -sd ' ' -)"
  if [ -n "$answers" ]; then
    say "$DOMAIN points at $answers, not at this server. Change its record at your DNS provider (DNS only, no proxy):"
  else
    say "Waiting for DNS. Add this record at your DNS provider:"
  fi
  say "    $DOMAIN  $(dns_record_type)  $CFG_publicAddress"
}

https_live() {
  curl -fs -o /dev/null --max-time 5 --resolve "$DOMAIN:443:127.0.0.1" "$ORIGIN/api/version"
}

caddy_news() {
  journalctl -u caddy --no-pager -o cat --cursor-file "$1" --since "@$2" 2>/dev/null | python3 -c '
import json, sys
quiet = {"acquiring lock", "lock acquired", "releasing lock", "lock released"}
for raw in sys.stdin:
    try:
        entry = json.loads(raw)
    except ValueError:
        continue
    if not isinstance(entry, dict):
        continue
    logger = str(entry.get("logger") or "")
    level = str(entry.get("level") or "")
    text = str(entry.get("msg") or "")
    wanted = logger.startswith(("tls.obtain", "tls.issuance")) or (
        level in ("warn", "error") and logger.startswith(("tls", "http")))
    if not wanted or text in quiet:
        continue
    if entry.get("error"):
        text += ": " + str(entry["error"])
    print("Caddy: " + text[:300], flush=True)
' || true
}

check_https() {
  local text issuer until answer
  text="$(openssl s_client -connect 127.0.0.1:443 -servername "$DOMAIN" </dev/null 2>/dev/null |
    openssl x509 -noout -issuer -enddate 2>/dev/null || true)"
  issuer="$(printf '%s\n' "$text" | sed -n 's/^issuer=.*O *= *\([^,]*\).*/\1/p' | head -n 1)"
  until="$(printf '%s\n' "$text" | sed -n 's/^notAfter=//p')"
  if [ -n "$until" ]; then say "HTTPS certificate from ${issuer:-an unknown issuer}, valid until $until; Caddy renews it."; fi
  answer="$(curl -s -o /dev/null --max-time 5 -w '%{http_code} %{redirect_url}' \
    --resolve "$DOMAIN:80:127.0.0.1" "http://$DOMAIN/" 2>/dev/null || true)"
  case "$answer" in
    30[1278]" https://$DOMAIN/"*) say "http://$DOMAIN redirects to https://$DOMAIN." ;;
    *) warn "http://$DOMAIN did not redirect to HTTPS (answered: ${answer:-nothing})." ;;
  esac
  check_cloudflare
}

check_cloudflare() {
  if [ "$CFG_domainProvided" != "1" ] || { [ "$PROXIED" != "1" ] && ! behind_cloudflare; }; then
    return 0
  fi
  say "$DOMAIN is behind Cloudflare's proxy. Checking that Cloudflare reaches this server."
  local waited=0 answer
  while :; do
    answer="$(curl -s -o /dev/null --max-time 10 -w '%{http_code} %header{x-forkmesh-worker} %{redirect_url}' \
      "$ORIGIN/api/version" 2>/dev/null || true)"
    case "$answer" in
      "200 app "*) say "$ORIGIN answers through Cloudflare."; return 0 ;;
    esac
    if [ "$waited" -ge "$PROXY_WAIT_SECONDS" ]; then break; fi
    sleep 5
    waited=$((waited + 5))
  done
  case "$answer" in
    30[1278]*" https://$DOMAIN/"*)
      die "Cloudflare sends $ORIGIN back to itself in a loop: its SSL/TLS encryption mode is Flexible. In the Cloudflare dashboard, SSL/TLS > Overview, choose Full (strict), then run the setup again." 32 ;;
    52[0-9]*)
      die "Cloudflare could not reach this server over HTTPS (Cloudflare error ${answer%% *}). Check that the Cloudflare record for $DOMAIN points at $CFG_publicAddress and that port 443 is open to the internet, including any firewall at your hosting provider, then run the setup again." 32 ;;
    *)
      die "Through Cloudflare, $ORIGIN does not reach this server (answered: ${answer:-nothing}). Check that the Cloudflare record for $DOMAIN points at $CFG_publicAddress, then run the setup again." 32 ;;
  esac
}

setup_https() {
  begin https "Setting up HTTPS for $DOMAIN"
  local caddy_before live_before="0"
  caddy_before="$(files_digest /etc/caddy/Caddyfile)"
  if systemctl is-active --quiet caddy && https_live; then live_before="1"; fi
  write_caddyfile
  if [ "$(files_digest /etc/caddy/Caddyfile)" = "$caddy_before" ] && [ "$live_before" = "1" ]; then
    say "$ORIGIN is already live over HTTPS."
    check_https
    return 0
  fi
  if [ "$live_before" = "1" ]; then
    say "$ORIGIN already has an HTTPS certificate, so its DNS is set up."
  elif [ "$CFG_domainProvided" = "1" ] && ! dns_ready; then
    dns_wait_message
    local waited=0
    while ! dns_ready; do
      if [ "$waited" -ge "$DNS_WAIT_SECONDS" ]; then
        die "$DOMAIN does not point at this server ($CFG_publicAddress) yet. Add the $(dns_record_type) record and run the setup again." 30
      fi
      sleep 15
      waited=$((waited + 15))
      if [ $((waited % 60)) -eq 0 ]; then say "Still waiting for $DOMAIN to point at $CFG_publicAddress ($((waited / 60)) min)."; fi
    done
    [ "$PROXIED" = "1" ] || say "$DOMAIN now points at this server."
  fi
  if [ "$PROXIED" = "1" ]; then
    say "$DOMAIN is behind Cloudflare's proxy, so it shows Cloudflare's addresses. Caddy gets the certificate through it."
  fi
  local started cursor
  started="$(date +%s)"
  cursor="$(mktemp)"
  systemctl enable caddy >/dev/null 2>&1
  systemctl restart caddy
  if [ "${FORKMESH_INSTANCE_TLS:-}" = "internal" ]; then
    sleep 2
    caddy trust >/dev/null 2>&1 || warn "could not trust Caddy's internal certificate authority"
  fi
  say "Caddy is getting an HTTPS certificate for $DOMAIN (Let's Encrypt, or ZeroSSL if that fails)."
  local waited=0
  while [ "$waited" -lt "$CERT_WAIT_SECONDS" ]; do
    caddy_news "$cursor" "$started"
    if https_live; then
      rm -f "$cursor"
      say "$ORIGIN is live over HTTPS."
      check_https
      return 0
    fi
    sleep 3
    waited=$((waited + 3))
    if [ $((waited % 60)) -eq 0 ]; then say "Still waiting for the certificate ($((waited / 60)) min)."; fi
  done
  caddy_news "$cursor" "$started"
  rm -f "$cursor"
  journalctl -u caddy --no-pager -n 15 2>/dev/null | cut -c1-300 || true
  die "Caddy could not get a certificate for $DOMAIN. Check that ports 80 and 443 are open to the internet." 31
}

record_daemon_build() {
  python3 -c '
import json, sys
print(json.dumps({"from": sys.argv[1], "serverTree": sys.argv[2], "commit": sys.argv[3],
                  "installedAt": int(sys.argv[4])}, sort_keys=True))
' "$1" "$(git -C "$SRC" rev-parse HEAD:server 2>/dev/null || true)" "$(git -C "$SRC" rev-parse HEAD)" "$(date +%s)" > "$DAEMON_BUILD.new"
  chmod 0644 "$DAEMON_BUILD.new"
  mv -f "$DAEMON_BUILD.new" "$DAEMON_BUILD"
}

install_daemon_binary() {
  if [ -n "${FORKMESH_INSTANCE_DAEMON:-}" ]; then
    install -m 0755 "$FORKMESH_INSTANCE_DAEMON" "$DAEMON.new"
    mv -f "$DAEMON.new" "$DAEMON"
    record_daemon_build supplied
    return 0
  fi
  if [ -x "$DAEMON" ] && [ "$CONFIG_MODE" != "update" ]; then
    say "The signed ForkMesh git node program is already installed."
    return 0
  fi
  if [ -x "$DAEMON" ] && [ "${CFG_sourceUrl%.git}" != "$RELEASE_SOURCE" ]; then
    say "Kept the installed git node program: this instance follows its own source, which the signed release is not built from."
    return 0
  fi
  local stage
  stage="$(mktemp -d /tmp/forkmesh-instance-stage.XXXXXX)"
  curl -fsSL --retry 3 "$INSTALL_SH_URL" -o "$stage/install.sh" ||
    die "Could not download $INSTALL_SH_URL."
  say "Fetching the signed ForkMesh release (verified the same way as a node install)"
  if ! HOME=/root FORKMESH_NO_LAUNCH=1 FORKMESH_NO_INSTALL_DEPS=1 FORKMESH_NO_SOURCE_FALLBACK=1 \
      FORKMESH_BIN_DIR="$stage/bin" bash "$stage/install.sh" </dev/null >"$stage/install.log" 2>&1; then
    tail -n 15 "$stage/install.log"
    die "Could not fetch the signed ForkMesh release."
  fi
  [ -x "$stage/bin/forkmesh-mirror-node" ] ||
    die "The signed release has no forkmesh-mirror-node for this server."
  install -m 0755 "$stage/bin/forkmesh-mirror-node" "$DAEMON"
  record_daemon_build release
  rm -f "$stage/install.sh" "$stage/install.log" "$stage/bin/forkmesh-mirror-node" "$stage/bin/forkmesh"
  rmdir "$stage/bin" "$stage" 2>/dev/null || true
}

daemon_rebuild_notice() {
  write_daemon_state
  local state
  state="$(python3 -c 'import json, sys; print((json.load(open(sys.argv[1])).get("daemon") or {}).get("state") or "")' "$BUILD_INFO" 2>/dev/null || true)"
  if [ "$state" = "stale" ]; then
    warn "server/ changed since the git node program was built; it keeps running the old build. Rebuild it from $(git -C "$SRC" rev-parse --short HEAD) and run the update with FORKMESH_INSTANCE_DAEMON (docs/INSTANCES.md)."
  fi
}

write_daemon_unit() {
  cat > "$DAEMON_UNIT" <<EOF
[Unit]
Description=ForkMesh git node for $DOMAIN
Wants=network-online.target
After=network-online.target forkmesh-instance.service

[Service]
Type=simple
User=$NODE_USER
Group=$NODE_USER
UMask=0077
Environment=TZ=UTC
Environment=HOME=/var/lib/forkmesh
Environment=XDG_DATA_HOME=/var/lib/forkmesh/.local/share
Environment=TMPDIR=/var/lib/forkmesh/tmp
Environment="FORKMESH_COMMIT_NAME=$CFG_projectName"
Environment=FORKMESH_COMMIT_EMAIL=noreply@$DOMAIN
ExecStartPre=$DAEMON --config $DAEMON_CONFIG --check
ExecStart=$DAEMON --config $DAEMON_CONFIG
Restart=on-failure
RestartSec=5s
TimeoutStopSec=15s
NoNewPrivileges=true
PrivateDevices=true
PrivateTmp=true
ProtectHome=true
ProtectSystem=strict
ReadWritePaths=/var/lib/forkmesh /etc/forkmesh
IPAddressDeny=169.254.0.0/16
LimitNOFILE=8192

[Install]
WantedBy=multi-user.target
EOF
}

daemon_digest() {
  local gateway=""
  if [ -s "$DAEMON_CONFIG" ]; then
    gateway="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1])).get("gatewayConfig") or "")' "$DAEMON_CONFIG" 2>/dev/null || true)"
  fi
  files_digest "$DAEMON" "$DAEMON_CONFIG" "$DAEMON_UNIT" ${gateway:+"$gateway"}
}

# An SSH relay added by tools/install_ssh_relay.sh keeps its gateway and
# broker in step with the app it relays to: an update installs the same
# release's copies and restarts the broker. Nothing here creates a relay.
refresh_ssh_relay() {
  [ -s /etc/forkmesh-ssh/ssh-gateway.json ] || return 0
  [ -d /opt/forkmesh-ssh ] || return 0
  local tool
  for tool in ssh_gateway.py ssh_authorization_broker.py; do
    [ -s "$SRC/tools/$tool" ] && install -m 0755 -o root -g root "$SRC/tools/$tool" "/opt/forkmesh-ssh/$tool"
  done
  systemctl try-restart forkmesh-ssh-auth.service >/dev/null 2>&1 || warn "the SSH relay broker did not restart (journalctl -u forkmesh-ssh-auth)"
}

setup_git_node() {
  DAEMON_BEFORE="$(daemon_digest)"
  install_daemon_binary
  daemon_rebuild_notice
  install -d -m 0755 "$DAEMON_TOOLS"
  local tool
  for tool in tools/mirror_gateway.py app/src/mirror_replica.py app/src/public_snapshots.py \
      app/src/portable_sessions.py tools/direct_https_bootstrap.py tools/node_operator_policy.py \
      tools/secret_gate.py; do
    [ -f "$SRC/$tool" ] && install -m 0644 "$SRC/$tool" "$DAEMON_TOOLS/"
  done
  install -d -m 0700 -o "$NODE_USER" -g "$NODE_USER" /etc/forkmesh /var/lib/forkmesh /var/lib/forkmesh/tmp
  local version
  version="$(git -C "$SRC" rev-parse --short HEAD)"
  local current_origin=""
  if [ -s "$DAEMON_CONFIG" ]; then
    current_origin="$(python3 -c '
import json, sys
cfg = json.load(open(sys.argv[1]))
gateway = json.load(open(cfg["gatewayConfig"]))
manifest = json.load(open(gateway["manifestPath"]))
print((manifest.get("endpoint") or {}).get("origin", ""))
' "$DAEMON_CONFIG" 2>/dev/null || true)"
  fi
  if [ -s "$DAEMON_CONFIG" ] && [ "$current_origin" != "$ORIGIN" ]; then
    say "The address changed from ${current_origin:-?} to $ORIGIN; setting the git node up again with the same key."
    systemctl stop forkmesh-mirror-node.service 2>/dev/null || true
    mv "$DAEMON_CONFIG" "$DAEMON_CONFIG.before-$(date +%s)"
  fi
  if [ ! -s "$DAEMON_CONFIG" ]; then
    runuser -u "$NODE_USER" -- "$DAEMON" --init \
      --config "$DAEMON_CONFIG.init" --state-dir /var/lib/forkmesh \
      --node "$NODE_NAME" --owner forkmesh --repository forkmesh \
      --upstream "$(platform_source)" \
      --catalog-url "$ORIGIN/api/repositories" \
      --public-origin "$ORIGIN" --mirror-url "$ORIGIN" \
      --gateway-script "$DAEMON_TOOLS/mirror_gateway.py" \
      --disable-cloudflared --version "$version" \
      --router-public-key "$ROUTER_PUBLIC_KEY" >/dev/null
    mv "$DAEMON_CONFIG.init" "$DAEMON_CONFIG"
  fi
  python3 - "$DAEMON_CONFIG" "$ORIGIN" "$version" "$ROUTER_PUBLIC_KEY" "$CFG_ownerName" "$(upstream_page)" <<'PY'
import json, os, re, sys
path, origin, version, router_key, owner, upstream_page = sys.argv[1:7]
cfg = json.load(open(path))
upstreams = (cfg.get("upstreams") or {}).get("forkmesh/forkmesh") or []
if upstreams and re.fullmatch(r"https://[A-Za-z0-9.-]+(?::[0-9]{1,5})?(?:/[A-Za-z0-9._~-]+){1,8}", upstream_page):
    cfg["mirrorOf"] = {"forkmesh/forkmesh": upstream_page}
    driven = [item for item in cfg.get("eventDrivenUpstreams") or [] if item in upstreams]
    if not driven:
        cfg["eventDrivenUpstreams"] = [upstreams[0]]
cfg.update({
    "role": "core",
    "mirrorUrl": origin,
    "catalogUrl": origin + "/api/repositories",
    "disableCloudflared": True,
    "disableAgentJobs": True,
    "enableActions": False,
    "disableAgentLogin": True,
    "version": version,
    "hosted": {
        "enabled": True,
        "root": "/var/lib/forkmesh/hosted",
        "maxRepositoryBytes": 1073741824,
        "importHosts": ["github.com", "gitlab.com", "codeberg.org", "app.forkmesh.com"],
    },
})
gateway = cfg.get("gatewayConfig")
if gateway and os.path.exists(gateway):
    doc = json.load(open(gateway))
    before = json.dumps(doc, sort_keys=True)
    doc["routerPublicKey"] = router_key
    doc["publicOrigin"] = origin
    dashboard = doc.get("dashboard")
    if isinstance(dashboard, dict) and dashboard.get("canonicalOrigin"):
        dashboard["canonicalOrigin"] = origin
    repositories = doc.get("repositories") or []
    base = next((entry for entry in repositories
                 if (entry.get("owner"), entry.get("name")) == ("forkmesh", "forkmesh")), None)
    if base is not None:
        kept = [entry for entry in repositories
                if entry.get("name") != "forkmesh" or entry.get("owner") == "forkmesh"
                or entry.get("gitDir") != base.get("gitDir")]
        if owner != "forkmesh":
            alias = dict(base)
            alias["owner"] = owner
            kept.append(alias)
        doc["repositories"] = kept
    if json.dumps(doc, sort_keys=True) != before:
        st = os.stat(gateway)
        with open(gateway + ".new", "w") as handle:
            handle.write(json.dumps(doc, indent=2) + "\n")
        os.chmod(gateway + ".new", st.st_mode & 0o7777)
        os.chown(gateway + ".new", st.st_uid, st.st_gid)
        os.replace(gateway + ".new", gateway)
with open(path + ".new", "w") as handle:
    handle.write(json.dumps(cfg, indent=2) + "\n")
cfg.pop("mirrorOf", None)
with open(path + ".nomirror", "w") as handle:
    handle.write(json.dumps(cfg, indent=2) + "\n")
cfg.pop("role", None)
cfg.pop("hosted", None)
with open(path + ".norole", "w") as handle:
    handle.write(json.dumps(cfg, indent=2) + "\n")
PY
  chown "$NODE_USER:$NODE_USER" "$DAEMON_CONFIG.new" "$DAEMON_CONFIG.nomirror" "$DAEMON_CONFIG.norole"
  if runuser -u "$NODE_USER" -- "$DAEMON" --config "$DAEMON_CONFIG.new" --check >/dev/null 2>&1; then
    mv "$DAEMON_CONFIG.new" "$DAEMON_CONFIG"
  elif runuser -u "$NODE_USER" -- "$DAEMON" --config "$DAEMON_CONFIG.nomirror" --check >/dev/null 2>&1; then
    say "This ForkMesh release does not label the read-only forkmesh mirror yet; the website still labels it and refuses writes."
    mv "$DAEMON_CONFIG.nomirror" "$DAEMON_CONFIG"
  else
    warn "this ForkMesh release does not know the core role yet; the git node runs without it"
    mv "$DAEMON_CONFIG.norole" "$DAEMON_CONFIG"
  fi
  rm -f "$DAEMON_CONFIG.new" "$DAEMON_CONFIG.nomirror" "$DAEMON_CONFIG.norole"
  refresh_ssh_relay
  NODE_PUBLIC_KEY="$(runuser -u "$NODE_USER" -- "$DAEMON" --config "$DAEMON_CONFIG" --public-key | tr -d '[:space:]')"
  [[ "$NODE_PUBLIC_KEY" =~ ^[A-Za-z0-9_-]{43}$ ]] || die "The git node has no identity key."
  chmod -R go-rwx /etc/forkmesh /var/lib/forkmesh
}

create_admin() {
  begin admin "Creating your admin account"
  setup_git_node
  local result
  result="$(
    cd "$SRC/app/host" && printf '%s\n%s\n%s\n%s' "$OWNERS_JSON" "${CFG_legalPages:-}" "${CFG_directory:-}" "$ADMIN_PASSWORD" | python3 -c '
import json, sys
owners = json.loads(sys.stdin.readline())
legal = json.loads(sys.stdin.readline().strip() or "{}")
directory = json.loads(sys.stdin.readline().strip() or "{}")
print(json.dumps({
    "adminUsername": sys.argv[1], "adminEmail": sys.argv[2],
    "adminPassword": sys.stdin.read(), "ownerType": sys.argv[3],
    "ownerName": sys.argv[4], "projectName": sys.argv[5],
    "nodePublicKey": sys.argv[6], "owners": owners,
    "theme": json.loads(sys.argv[7]) if sys.argv[7].startswith("{") else sys.argv[7],
    "singleOrganization": sys.argv[8] == "1", "legalPages": legal,
    "directory": directory}))
' "$CFG_adminUsername" "$CFG_adminEmail" "$CFG_ownerType" "$CFG_ownerName" "$CFG_projectName" "$NODE_PUBLIC_KEY" "$CFG_theme" "$CFG_singleOrganization" \
      | host_cli instance-bootstrap 2>&1 | tail -n 1
  )" || true
  ADMIN_PASSWORD=""
  OWNERS_JSON="[]"
  case "$result" in
    *'"ok": true'*) ;;
    *) die "Could not create the admin account: $(printf '%s' "$result" | cut -c1-300)" ;;
  esac
  say "Admin @$CFG_adminUsername is ready (it asks you to set up two-factor sign-in the first time)."
  if [ "$CFG_ownerType" = "org" ]; then
    say "Organization $CFG_ownerName is ready, owned by @$CFG_adminUsername$(printf '%s' "$CFG_owners" | sed -e 's/^\(.\)/,@\1/' -e 's/,\([^@]\)/,@\1/g' -e 's/,/, /g')."
  fi
}

update_admin() {
  begin admin "Checking the git node"
  setup_git_node
}

start_git_node() {
  begin repo "Creating $CFG_ownerName/forkmesh"
  write_daemon_unit
  if [ "$(daemon_digest)" = "$DAEMON_BEFORE" ] && systemctl is-active --quiet forkmesh-mirror-node.service; then
    say "The git node is already running with these settings."
  else
    systemctl daemon-reload
    systemctl enable forkmesh-mirror-node.service >/dev/null 2>&1
    systemctl restart forkmesh-mirror-node.service
    say "The git node is copying the ForkMesh source from $(platform_source)."
  fi
  local waited=0
  while [ "$waited" -lt "$REPO_WAIT_SECONDS" ]; do
    if curl -fs -o /dev/null --max-time 10 --resolve "$DOMAIN:443:127.0.0.1" \
        "$REPO_URL.git/info/refs?service=git-upload-pack"; then
      say "$REPO_URL is ready."
      return 0
    fi
    sleep 10
    waited=$((waited + 10))
    if [ $((waited % 60)) -eq 0 ]; then say "Still copying ($((waited / 60)) min)."; fi
  done
  warn "$REPO_URL is still copying; it appears on its own when the copy finishes."
}

setup_actions_runner() {
  [ -n "$RUNNER_NAME" ] || resolve_runner_name
  if [ "$CFG_actions" != "1" ]; then
    if systemctl is-enabled --quiet "$RUNNER_UNIT" 2>/dev/null; then
      systemctl disable --now "$RUNNER_UNIT" >/dev/null 2>&1 || true
      say "Actions are off: stopped the runner $RUNNER_NAME."
    fi
    return 0
  fi
  say "Installing the Actions runner $RUNNER_NAME next to the git node"
  [ -f "$SRC/$RUNNER_INSTALLER" ] || { warn "this ForkMesh source has no $RUNNER_INSTALLER; Actions stay off"; return 0; }
  if [ "$NEW_INSTANCE" = "1" ]; then
    install -d -m 0700 "$CLEANUP_DIR"
    printf '%s\n' "$CFG_ownerName" > "$CLEANUP_DIR/created-runner"
  fi
  if [ -f "$SRC/tools/forkmesh-actions.sh" ]; then
    install -m 0755 "$SRC/tools/forkmesh-actions.sh" /usr/local/bin/forkmesh-actions
  fi
  local output runner_key result
  if ! output="$(HOME=/root bash "$SRC/$RUNNER_INSTALLER" --preregistered \
      --name "$RUNNER_NAME" --owner "$CFG_ownerName" --app "$ORIGIN" \
      --providers none --job-user "$RUNNER_JOB_USER" --binary "$DAEMON" </dev/null 2>&1)"; then
    printf '%s\n' "$output" | tail -n 15 | lines
    warn "the Actions runner did not install; workflows wait until it does (run --update to retry)"
    return 0
  fi
  runner_key="$(printf '%s\n' "$output" | sed -n 's/^FM_RUNNER_PUBLIC_KEY=//p' | tail -n 1)"
  if ! [[ "$runner_key" =~ ^[A-Za-z0-9_-]{43}$ ]]; then
    warn "the Actions runner has no public key; workflows wait until it does"
    return 0
  fi
  if ! result="$(cd "$SRC/app/host" && host_cli instance-runner --name "$RUNNER_NAME" \
      --public-key "$runner_key" --owner "$CFG_ownerName" 2>&1)"; then
    printf '%s\n' "$result" | tail -n 5 | lines
    warn "could not register the Actions runner $RUNNER_NAME with the app"
    return 0
  fi
  write_runner_identity
  systemctl restart "$RUNNER_UNIT" >/dev/null 2>&1 || true
  say "The Actions runner $RUNNER_NAME runs workflows for $CFG_ownerName as $RUNNER_JOB_USER."
}

write_runner_identity() {
  local dir="/etc/systemd/system/$RUNNER_UNIT.d"
  install -d -m 0755 "$dir"
  printf '[Service]\nEnvironment="FORKMESH_COMMIT_NAME=%s"\nEnvironment=FORKMESH_COMMIT_EMAIL=noreply@%s\n' \
    "$CFG_projectName" "$DOMAIN" > "$dir/identity.conf.new"
  chmod 0644 "$dir/identity.conf.new"
  mv "$dir/identity.conf.new" "$dir/identity.conf"
  systemctl daemon-reload >/dev/null 2>&1 || true
}

write_first_login_cleanup() {
  install -d -m 0700 "$CLEANUP_DIR"
  install -m 0700 "$SRC/tools/forkmesh-safe-rm.sh" "$CLEANUP_DIR/safe-rm.sh"
  cat > "$LOGIN_CLEANUP" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
CLEANUP_DIR="/usr/local/lib/forkmesh-instance"
MARKER="/var/lib/forkmesh-instance/first-admin-login"
APP_ENV="/etc/forkmesh-instance/app.env"
APP_USER="forkmesh-app"
NODE_USER="forkmesh-node"
source "$CLEANUP_DIR/safe-rm.sh"
if [ -e "$MARKER" ]; then
  sed -i 's/^INSTANCE_LOGIN_DEADLINE=.*/INSTANCE_LOGIN_DEADLINE=0/' "$APP_ENV"
  rm -f "$CLEANUP_DIR/caddyfile.before" "$CLEANUP_DIR/caddyfile.absent" \
    "$CLEANUP_DIR/new-instance" "$CLEANUP_DIR/created-app-user" \
    "$CLEANUP_DIR/created-node-user" "$CLEANUP_DIR/created-swap" \
    "$CLEANUP_DIR/ufw-80" "$CLEANUP_DIR/ufw-443" \
    "$CLEANUP_DIR/created-runner" "$CLEANUP_DIR/created-runner-root" \
    "$CLEANUP_DIR/created-job-user"
  systemctl disable --now forkmesh-instance-first-login-cleanup.timer >/dev/null 2>&1 || true
  systemctl daemon-reload
  systemctl restart forkmesh-instance.service
  exit 0
fi
systemctl disable --now forkmesh-instance.service forkmesh-mirror-node.service forkmesh-instance-update.path >/dev/null 2>&1 || true
RUNNER_OWNER=""
if [ -s "$CLEANUP_DIR/created-runner" ]; then
  RUNNER_OWNER="$(head -n 1 "$CLEANUP_DIR/created-runner")"
  [[ "$RUNNER_OWNER" =~ ^[a-z]([a-z0-9-]{0,61}[a-z0-9])?$ ]] || RUNNER_OWNER=""
fi
if [ -n "$RUNNER_OWNER" ]; then
  systemctl disable --now "forkmesh-runner@$RUNNER_OWNER.service" >/dev/null 2>&1 || true
  fm_safe_rm_tree /etc/forkmesh-runner/accounts "/etc/forkmesh-runner/accounts/$RUNNER_OWNER"
  fm_safe_rm_tree /var/lib/forkmesh-runner/accounts "/var/lib/forkmesh-runner/accounts/$RUNNER_OWNER"
  rm -f "/etc/systemd/system/forkmesh-runner@$RUNNER_OWNER.service.d/identity.conf"
  rmdir "/etc/systemd/system/forkmesh-runner@$RUNNER_OWNER.service.d" 2>/dev/null || true
fi
if [ -e "$CLEANUP_DIR/created-runner-root" ]; then
  systemctl disable --now forkmesh-runner-firewall.service >/dev/null 2>&1 || true
  nft delete table inet forkmesh_runner >/dev/null 2>&1 || true
  fm_safe_rm_tree /etc /etc/forkmesh-runner
  fm_safe_rm_tree /var/lib /var/lib/forkmesh-runner
  rm -f /etc/systemd/system/forkmesh-runner@.service \
    /etc/systemd/system/forkmesh-runner-firewall.service
fi
if [ -e "$CLEANUP_DIR/caddyfile.before" ]; then
  install -m 0644 "$CLEANUP_DIR/caddyfile.before" /etc/caddy/Caddyfile
elif [ -e "$CLEANUP_DIR/caddyfile.absent" ]; then
  rm -f /etc/caddy/Caddyfile
fi
fm_safe_rm_tree /opt /opt/forkmesh-instance
fm_safe_rm_tree /etc /etc/forkmesh-instance /etc/forkmesh
fm_safe_rm_tree /var/lib /var/lib/forkmesh-instance /var/lib/forkmesh /var/lib/forkmesh-instance-update
fm_safe_rm_tree /usr/local/share/forkmesh /usr/local/share/forkmesh/tools
fm_safe_rm_tree /usr/local/lib /usr/local/lib/forkmesh-instance-update
rm -f /usr/local/bin/forkmesh-mirror-node /usr/local/bin/forkmesh-actions \
  /etc/systemd/system/forkmesh-instance.service \
  /etc/systemd/system/forkmesh-mirror-node.service \
  /etc/systemd/system/forkmesh-instance-update.service \
  /etc/systemd/system/forkmesh-instance-update.path \
  /etc/systemd/system/forkmesh-instance-first-login-cleanup.service \
  /etc/systemd/system/forkmesh-instance-first-login-cleanup.timer
if [ -e "$CLEANUP_DIR/ufw-80" ]; then ufw delete allow 80/tcp >/dev/null 2>&1 || true; fi
if [ -e "$CLEANUP_DIR/ufw-443" ]; then ufw delete allow 443/tcp >/dev/null 2>&1 || true; fi
if [ -e "$CLEANUP_DIR/created-swap" ]; then
  swapoff /swapfile-forkmesh 2>/dev/null || true
  rm -f /swapfile-forkmesh
  sed -i '\|^/swapfile-forkmesh |d' /etc/fstab
fi
if [ -e "$CLEANUP_DIR/created-app-user" ]; then userdel "$APP_USER" 2>/dev/null || true; fi
if [ -e "$CLEANUP_DIR/created-node-user" ]; then userdel "$NODE_USER" 2>/dev/null || true; fi
if [ -e "$CLEANUP_DIR/created-job-user" ]; then userdel forkmesh-job 2>/dev/null || true; fi
rm -f "$CLEANUP_DIR/caddyfile.before" "$CLEANUP_DIR/caddyfile.absent" \
  "$CLEANUP_DIR/new-instance" "$CLEANUP_DIR/created-app-user" \
  "$CLEANUP_DIR/created-node-user" "$CLEANUP_DIR/created-swap" \
  "$CLEANUP_DIR/ufw-80" "$CLEANUP_DIR/ufw-443" \
  "$CLEANUP_DIR/created-runner" "$CLEANUP_DIR/created-runner-root" \
  "$CLEANUP_DIR/created-job-user"
systemctl daemon-reload
systemctl reload caddy >/dev/null 2>&1 || systemctl restart caddy >/dev/null 2>&1 || true
EOF
  chmod 0700 "$LOGIN_CLEANUP"
  cat > "$LOGIN_CLEANUP_UNIT" <<EOF
[Unit]
Description=Remove an unclaimed ForkMesh instance

[Service]
Type=oneshot
ExecStart=$LOGIN_CLEANUP
EOF
  cat > "$LOGIN_CLEANUP_TIMER" <<EOF
[Unit]
Description=Five-minute claim window for a new ForkMesh instance

[Timer]
OnActiveSec=${FIRST_LOGIN_SECONDS}s
AccuracySec=1s
Unit=forkmesh-instance-first-login-cleanup.service

[Install]
WantedBy=timers.target
EOF
}

arm_first_login_timeout() {
  [ "$NEW_INSTANCE" = "1" ] || return 0
  [ ! -e "$LOGIN_MARKER" ] || return 0
  [ -z "$(env_value INSTANCE_LOGIN_DEADLINE)" ] || return 0
  local deadline
  deadline=$(( $(date +%s) * 1000 + FIRST_LOGIN_SECONDS * 1000 ))
  sed -i "s/^INSTANCE_LOGIN_DEADLINE=.*/INSTANCE_LOGIN_DEADLINE=$deadline/" "$APP_ENV"
  write_first_login_cleanup
  systemctl daemon-reload
  systemctl enable --now forkmesh-instance-first-login-cleanup.timer >/dev/null
  systemctl restart forkmesh-instance.service
  say "This new instance will remove itself in five minutes unless @$CFG_adminUsername signs in."
}

finish() {
  arm_first_login_timeout
  begin done "Opening $CFG_projectName"
  progress done done
  CURRENT_STEP=""
  say ""
  say "$CFG_projectName is running at $ORIGIN"
  say "Sign in as @$CFG_adminUsername. Your repository: $REPO_URL"
  say_admin_path
  say "Update later with: bash install-instance.sh --update"
  printf 'FMI {"done":true,"url":"%s"}\n' "$REPO_URL"
}

check_system
install_packages
fetch_source
build_app
activate_source
write_settings
migrate_database
start_app
SOURCE_COMMITTED="1"
setup_https
if [ "$CONFIG_MODE" = "update" ]; then update_admin; else create_admin; fi
start_git_node
setup_actions_runner
finish
exit 0
}
