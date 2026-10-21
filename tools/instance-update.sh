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
  local action=""
  if [ -L "$REQUEST" ]; then
    rm -f "$REQUEST"
  elif [ -f "$REQUEST" ]; then
    action="$(head -c 32 "$REQUEST" | tr -cd 'a-z')"
    rm -f "$REQUEST"
  fi
  printf '%s' "$action"
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
  if [ "$trigger" = "merge" ] && [ "$(git -C "$SRC" rev-parse FETCH_HEAD)" = "$(git -C "$SRC" rev-parse HEAD)" ]; then
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

main() {
  [ -d "$STATE" ] || { printf 'Missing %s\n' "$STATE" >&2; exit 1; }
  [ -d "$SRC/.git" ] || { status state=failed lastError="No ForkMesh source at $SRC."; exit 0; }
  local action
  action="$(read_request)"
  case "$action" in
    check) run_check ;;
    update) run_update admin ;;
    auto) run_update merge ;;
    *) printf 'No valid update request.\n' ;;
  esac
}

main
exit 0
}
