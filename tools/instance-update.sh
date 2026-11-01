#!/usr/bin/env bash
{
set -euo pipefail
umask 027

REQUEST="${FORKMESH_UPDATE_REQUEST:-/var/lib/forkmesh-instance/update/request}"
STATE="${FORKMESH_UPDATE_STATE:-/var/lib/forkmesh-instance-update}"
SRC="${FORKMESH_INSTANCE_SRC:-/opt/forkmesh-instance/src}"
INSTANCE_JSON="${FORKMESH_INSTANCE_JSON:-/etc/forkmesh-instance/instance.json}"
SETUP_UNIT="${FORKMESH_SETUP_UNIT:-forkmesh-instance-setup.service}"
STATUS="$STATE/status.json"
LOG="$STATE/update.log"
INSTALLER="$STATE/install-instance.sh"
STAGE="$STATE/.check"
VAR="${FORKMESH_INSTANCE_VAR:-/var/lib/forkmesh-instance}"
VENV="${FORKMESH_INSTANCE_VENV:-/opt/forkmesh-instance/venv}"
APP_ENV="${FORKMESH_INSTANCE_ENV:-/etc/forkmesh-instance/app.env}"
UPDATE_UNIT="${FORKMESH_UPDATE_UNIT:-forkmesh-instance-update.service}"
FOLLOW_FLAG="$(dirname "$REQUEST")/follow"
FOLLOW_TOOL="${FORKMESH_FOLLOW_TOOL:-$(dirname "$0")/instance_follow.py}"
FOLLOW_WORK="$STATE/follow"
FOLLOW_BARE="$STATE/follow.git"
FOLLOW_EVERY="${FORKMESH_FOLLOW_EVERY:-72000}"

now() { date +%s; }

status() {
  python3 -c '
import json, os, sys
path = sys.argv[1]
try:
    with open(path, encoding="utf-8") as handle:
        doc = json.load(handle)
except (OSError, ValueError):
    doc = {}
if not isinstance(doc, dict):
    doc = {}
for pair in sys.argv[2:]:
    key, _, value = pair.partition("=")
    if key.endswith(":int"):
        doc[key[:-4]] = int(value or 0)
    else:
        doc[key] = value[:500]
tmp = path + ".new"
with open(tmp, "w", encoding="utf-8") as handle:
    json.dump(doc, handle, sort_keys=True)
os.chmod(tmp, 0o640)
os.replace(tmp, path)
' "$STATUS" "$@"
}

setting() {
  python3 -c '
import json, sys
try:
    with open(sys.argv[1], encoding="utf-8") as handle:
        doc = json.load(handle)
except (OSError, ValueError):
    doc = {}
print(str(doc.get(sys.argv[2]) or sys.argv[3]) if isinstance(doc, dict) else sys.argv[3])
' "$INSTANCE_JSON" "$1" "$2"
}

read_request() {
  python3 -I -c '
import os, stat, sys
path = sys.argv[1]
try:
    folder = os.open(os.path.dirname(path), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
except OSError:
    sys.exit(0)
name = os.path.basename(path)
action = ""
try:
    fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=folder)
except OSError:
    fd = -1
if fd >= 0:
    with os.fdopen(fd, "rb") as handle:
        if stat.S_ISREG(os.fstat(handle.fileno()).st_mode):
            action = "".join(c for c in handle.read(32).decode("ascii", "ignore")
                             if "a" <= c <= "z")
try:
    os.unlink(name, dir_fd=folder)
except OSError:
    pass
os.close(folder)
sys.stdout.write(action)
' "$REQUEST"
}

commit_fields() {
  local prefix="$1" rev="$2" commit stamp subject version
  commit="$(git -C "$SRC" log -1 --format=%H "$rev" 2>/dev/null)" || return 1
  stamp="$(git -C "$SRC" log -1 --format=%ct "$rev")"
  subject="$(git -C "$SRC" log -1 --format=%s "$rev")"
  version="$(git -C "$SRC" show "$rev:desktop/CMakeLists.txt" 2>/dev/null |
    sed -n 's/^project(ForkMesh VERSION \([0-9][0-9.]*\).*/\1/p' | head -n1 || true)"
  printf '%s\n' "${prefix}Commit=$commit" "${prefix}Time:int=$stamp" \
    "${prefix}Subject=$subject" "${prefix}Version=$version"
}

source_url() {
  local url
  url="$(setting sourceUrl https://app.forkmesh.com/forkmesh/forkmesh)"
  [[ "$url" =~ ^https://[A-Za-z0-9.-]+(:[0-9]{1,5})?/[A-Za-z0-9._/-]+$ ]] ||
    [[ "$url" =~ ^file:///[A-Za-z0-9._/-]+$ ]] || return 1
  printf '%s' "$url"
}

fetch_latest() {
  local ref url domain
  local resolve=()
  ref="$(setting sourceRef main)"
  case "$ref" in
    -*|*[!A-Za-z0-9._/-]*) status lastError="The source branch in $INSTANCE_JSON is not valid."; return 1 ;;
  esac
  if ! url="$(source_url)"; then
    status lastError="The source address in $INSTANCE_JSON is not valid."
    return 1
  fi
  domain="$(setting domain '')"
  if [[ "$domain" =~ ^[a-z0-9.-]+$ ]] && [[ "$url" == "https://$domain/"* ]]; then
    resolve=(-c "http.curloptResolve=$domain:443:127.0.0.1")
  fi
  if ! git "${resolve[@]}" -C "$SRC" fetch --quiet --depth 1 "$url" "$ref" >"$STAGE.log" 2>&1; then
    status lastError="Could not reach $url: $(tail -n 1 "$STAGE.log" | cut -c1-200)"
    return 1
  fi
  local fields=()
  mapfile -t fields < <(commit_fields upstream FETCH_HEAD)
  status "${fields[@]}" checkedAt:int="$(now)" sourceRef="$ref" sourceUrl="$url" lastError=
}

running_fields() {
  local fields=()
  mapfile -t fields < <(commit_fields current HEAD)
  if [ "${#fields[@]}" -gt 0 ]; then status "${fields[@]}"; fi
}

follow() {
  local line
  while IFS= read -r line || [ -n "$line" ]; do
    printf '%s\n' "$line" >> "$LOG"
    if [[ "$line" =~ ^FMI\ \{\"step\":\"([a-z]+)\",\"state\":\"([a-z]+)\"\}$ ]]; then
      status step="${BASH_REMATCH[1]}" stepState="${BASH_REMATCH[2]}"
    elif [[ "$line" == Error:* ]]; then
      status lastError="${line#Error: }"
    fi
  done
}

run_check() {
  status state=checking action=check startedAt:int="$(now)" finishedAt:int=0
  running_fields || true
  if fetch_latest; then
    status state=idle finishedAt:int="$(now)"
  else
    status state=failed finishedAt:int="$(now)"
  fi
}

run_update() {
  local trigger="$1"
  status state=updating action=update trigger="$trigger" startedAt:int="$(now)" finishedAt:int=0 \
    step= stepState= lastError= exitCode:int=0 toCommit= fromCommit="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)"
  if systemctl is-active --quiet "$SETUP_UNIT" 2>/dev/null; then
    status state=failed finishedAt:int="$(now)" \
      lastError="A setup or update started on the server is still running; try again when it ends."
    return 0
  fi
  if ! fetch_latest; then
    status state=failed finishedAt:int="$(now)"
    return 0
  fi
  if [ "$trigger" != "admin" ] && [ "$(git -C "$SRC" rev-parse FETCH_HEAD)" = "$(git -C "$SRC" rev-parse HEAD)" ]; then
    status state=done exitCode:int=0 finishedAt:int="$(now)" toCommit="$(git -C "$SRC" rev-parse HEAD)" step=
    return 0
  fi
  if ! git -C "$SRC" show FETCH_HEAD:www/public/install-instance.sh > "$INSTALLER.new" ||
      [ ! -s "$INSTALLER.new" ]; then
    rm -f "$INSTALLER.new"
    status state=failed finishedAt:int="$(now)" lastError="The latest source has no instance installer."
    return 0
  fi
  chmod 0700 "$INSTALLER.new"
  mv -f "$INSTALLER.new" "$INSTALLER"
  if [ -s "$LOG" ]; then mv -f "$LOG" "$LOG.1"; fi
  (umask 077 && : > "$LOG")
  local code=0
  set +e
  bash "$INSTALLER" --update 2>&1 | follow
  code="${PIPESTATUS[0]}"
  set -e
  running_fields || true
  if [ "$code" = "0" ]; then
    status state=done exitCode:int=0 finishedAt:int="$(now)" toCommit="$(git -C "$SRC" rev-parse HEAD)"
  else
    status state=failed exitCode:int="$code" finishedAt:int="$(now)"
  fi
}

status_value() {
  python3 -c '
import json, sys
try:
    with open(sys.argv[1], encoding="utf-8") as handle:
        doc = json.load(handle)
except (OSError, ValueError):
    doc = {}
print(doc.get(sys.argv[2], sys.argv[3]) if isinstance(doc, dict) else sys.argv[3])
' "$STATUS" "$1" "$2"
}

follow_on() {
  python3 -I -c '
import os, stat, sys
path = sys.argv[1]
try:
    folder = os.open(os.path.dirname(path), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    fd = os.open(os.path.basename(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK,
                 dir_fd=folder)
except OSError:
    sys.exit(1)
os.close(folder)
with os.fdopen(fd, "rb") as handle:
    if not stat.S_ISREG(os.fstat(handle.fileno()).st_mode):
        sys.exit(1)
    sys.exit(0 if handle.read(8).decode("ascii", "ignore").strip() == "on" else 1)
' "$FOLLOW_FLAG"
}

request_follow() {
  python3 -I -c '
import os, sys
path = sys.argv[1]
folder = os.open(os.path.dirname(path), os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
owner = os.fstat(folder)
name = os.path.basename(path)
tmp = name + ".new"
try:
    os.unlink(tmp, dir_fd=folder)
except FileNotFoundError:
    pass
fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o640, dir_fd=folder)
with os.fdopen(fd, "w", encoding="ascii") as handle:
    os.fchown(handle.fileno(), owner.st_uid, owner.st_gid)
    handle.write("follow\n")
os.replace(tmp, name, src_dir_fd=folder, dst_dir_fd=folder)
os.close(folder)
' "$REQUEST"
}

actions_running() {
  local count
  count="$(sqlite3 -readonly "$VAR/app.db" "SELECT count(*) FROM repo_action_jobs WHERE status = 'running' AND runner != '' AND expires_at > $(( $(now) * 1000 ))" 2>/dev/null || echo 0)"
  [ "${count:-0}" != "0" ]
}

tick() {
  follow_on || return 0
  [ ! -e "$REQUEST" ] || return 0
  ! systemctl is-active --quiet "$UPDATE_UNIT" 2>/dev/null || return 0
  ! systemctl is-active --quiet "$SETUP_UNIT" 2>/dev/null || return 0
  local last
  last="$(status_value followAt 0)"
  [ "$(( $(now) - ${last:-0} ))" -ge "$FOLLOW_EVERY" ] || return 0
  ! actions_running || return 0
  request_follow
}

follow_fields() {
  python3 -c '
import json, sys
try:
    doc = json.loads(sys.argv[1])
except ValueError:
    doc = {"state": "failed", "message": "The follow tool gave no result."}
if not isinstance(doc, dict):
    doc = {}
state = doc.get("state") if doc.get("state") in ("current", "merged", "conflict", "failed") else "failed"
print("followState=" + state)
print("followUpstream=" + str(doc.get("upstream") or "")[:64])
print("followCommit=" + str(doc.get("commit") or "")[:64])
print("followBehind:int=" + str(int(doc.get("behind") or 0)))
print("followMessage=" + str(doc.get("message") or "").replace("\n", " ")[:300])
print("followFiles=" + " ".join(str(f) for f in (doc.get("files") or [])[:20])[:480])
' "$1"
}

run_follow() {
  status state=updating action=follow trigger=follow startedAt:int="$(now)" finishedAt:int=0 \
    step=follow stepState=running lastError= exitCode:int=0 toCommit= \
    fromCommit="$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)"
  local url ref follow_url follow_ref owner domain path org name result state
  local org_args=()
  if systemctl is-active --quiet "$SETUP_UNIT" 2>/dev/null; then
    status state=failed finishedAt:int="$(now)" \
      lastError="A setup or update started on the server is still running; try again when it ends."
    return 0
  fi
  ref="$(setting sourceRef main)"
  domain="$(setting domain '')"
  follow_url="$(setting followUrl https://app.forkmesh.com/forkmesh/forkmesh)"
  follow_ref="$(setting followRef main)"
  owner="$(setting followOwner '')"
  if ! url="$(source_url)" || [[ ! "$domain" =~ ^[a-z0-9.-]+$ ]] || [[ "$url" != "https://$domain/"* ]]; then
    status state=failed finishedAt:int="$(now)" \
      lastError="Following ForkMesh needs sourceUrl in $INSTANCE_JSON to be a repository on this instance."
    return 0
  fi
  path="${url#https://"$domain"/}"
  path="${path%.git}"
  org="${path%%/*}"
  name="${path#*/}"
  if [[ ! "$org" =~ ^[A-Za-z0-9][A-Za-z0-9-]{0,38}$ ]] || [[ ! "$name" =~ ^[A-Za-z0-9._-]{1,100}$ ]] ||
      [[ ! "$ref" =~ ^[A-Za-z0-9._/-]{1,100}$ ]] || [[ "$ref" == -* ]] ||
      [[ ! "$owner" =~ ^[A-Za-z0-9][A-Za-z0-9-]{0,38}$ ]] ||
      [[ ! "$follow_url" =~ ^https://[A-Za-z0-9.-]+(:[0-9]{1,5})?/[A-Za-z0-9._/-]+$ ]] ||
      [[ ! "$follow_ref" =~ ^[A-Za-z0-9._/-]{1,100}$ ]] || [[ "$follow_ref" == -* ]]; then
    status state=failed finishedAt:int="$(now)" \
      lastError="Set followOwner (the account that owns $path) and a valid sourceRef, followUrl and followRef in $INSTANCE_JSON."
    return 0
  fi
  if [ "${org,,}" != "${owner,,}" ]; then org_args=(--organization "$org"); fi
  [ -d "$FOLLOW_BARE" ] || git init --quiet --bare "$FOLLOW_BARE"
  if [ -s "$LOG" ]; then mv -f "$LOG" "$LOG.1"; fi
  (umask 077 && : > "$LOG")
  result="$(python3 -I "$FOLLOW_TOOL" --work "$FOLLOW_WORK" --source-url "$url" \
    --source-ref "$ref" --follow-url "$follow_url" --follow-ref "$follow_ref" \
    --resolve "$domain:443:127.0.0.1" --publish "$FOLLOW_BARE" \
    --name "ForkMesh follow" --email "noreply@$domain" 2>>"$LOG" | tail -n 1)" || true
  printf '%s\n' "$result" >> "$LOG"
  local fields=()
  mapfile -t fields < <(follow_fields "$result")
  status "${fields[@]}" followAt:int="$(now)"
  state="$(status_value followState failed)"
  case "$state" in
    merged)
      if ! (cd "$SRC/app/host" && "$VENV/bin/python" -m forkmesh_host instance-repo \
          --owner "$owner" "${org_args[@]}" --name "$name" --from "$FOLLOW_BARE" \
          --env-file "$APP_ENV" --db "sqlite:$VAR/app.db" --data-dir "$VAR/data") >>"$LOG" 2>&1; then
        status state=failed finishedAt:int="$(now)" followState=failed \
          lastError="The ForkMesh merge could not be published to $path; see $LOG."
        return 0
      fi
      run_update follow ;;
    current)
      run_update follow ;;
    *)
      status state=failed finishedAt:int="$(now)" \
        lastError="Following ForkMesh stopped: $(status_value followMessage 'see the log')" ;;
  esac
}

main() {
  [ -d "$STATE" ] || { printf 'Missing %s\n' "$STATE" >&2; exit 1; }
  [ -d "$SRC/.git" ] || { status state=failed lastError="No ForkMesh source at $SRC."; exit 0; }
  if [ "${1:-}" = "--tick" ]; then tick; return 0; fi
  local action
  action="$(read_request)"
  case "$action" in
    follow) run_follow ;;
    check) run_check ;;
    update) run_update admin ;;
    auto) run_update merge ;;
    *) printf 'No valid update request.\n' ;;
  esac
}

main "$@"
exit 0
}
