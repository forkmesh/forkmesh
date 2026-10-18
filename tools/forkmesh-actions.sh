#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
forkmesh-actions: talk to the ForkMesh job API from a workflow step.

  forkmesh-actions event                         print the event payload (JSON)
  forkmesh-actions issue <n>                     print issue <n> (JSON)
  forkmesh-actions comment <n> <body|->          comment on issue <n> (- reads stdin)
  forkmesh-actions label <n> [--add a,b] [--remove c]
  forkmesh-actions close <n>
  forkmesh-actions reopen <n>
  forkmesh-actions open-issue <title> <body|-> [label,label]

Needs FORKMESH_API_URL and FORKMESH_TOKEN (set for trusted jobs), curl, and
jq or python3. See docs/server/instance-actions.md.
EOF
}

die() { printf 'forkmesh-actions: %s\n' "$1" >&2; exit "${2:-1}"; }

have_jq() { [ "${FORKMESH_ACTIONS_JSON:-}" != "python" ] && command -v jq >/dev/null 2>&1; }

json_object() {
  if have_jq; then
    local args=() filter="{" sep=""
    while [ $# -gt 0 ]; do
      case "$1" in
        str) args+=(--arg "$2" "$3"); filter+="$sep\"$2\": \$$2" ;;
        list) args+=(--arg "$2" "$3"); filter+="$sep\"$2\": (\$$2 | split(\",\") | map(gsub(\"^\\\\s+|\\\\s+\$\"; \"\")) | map(select(length > 0)))" ;;
      esac
      sep=", "
      shift 3
    done
    jq -cn "${args[@]}" "$filter}"
  else
    python3 -c '
import json, sys
items = sys.argv[1:]
out = {}
for index in range(0, len(items), 3):
    kind, key, value = items[index:index + 3]
    out[key] = value if kind == "str" else [part.strip() for part in value.split(",") if part.strip()]
print(json.dumps(out))
' "$@"
  fi
}

require_env() {
  [ -n "${FORKMESH_API_URL:-}" ] || die "FORKMESH_API_URL is not set (this runs inside a ForkMesh Actions job)."
  [ -n "${FORKMESH_TOKEN:-}" ] || die "FORKMESH_TOKEN is not set (only trusted jobs get one)."
  command -v curl >/dev/null 2>&1 || die "curl is required."
  have_jq || command -v python3 >/dev/null 2>&1 || die "jq or python3 is required."
}

api() {
  local method="$1" path="$2" body="${3:-}" answer status
  require_env
  local url="${FORKMESH_API_URL%/}/api/actions/job$path"
  if [ -n "$body" ]; then
    answer="$(printf '%s' "$body" | curl -sS -X "$method" --max-time 60 \
      -H @<(printf 'Authorization: Bearer %s\n' "$FORKMESH_TOKEN") \
      -H 'Content-Type: application/json' -H 'Accept: application/json' \
      --data-binary @- -w '\n%{http_code}' "$url")" || die "could not reach $FORKMESH_API_URL."
  else
    answer="$(curl -sS -X "$method" --max-time 60 \
      -H @<(printf 'Authorization: Bearer %s\n' "$FORKMESH_TOKEN") \
      -H 'Accept: application/json' -w '\n%{http_code}' "$url")" || die "could not reach $FORKMESH_API_URL."
  fi
  status="${answer##*$'\n'}"
  answer="${answer%$'\n'*}"
  if [ "${status:0:1}" != "2" ]; then
    printf '%s\n' "$answer" >&2
    die "$method $path answered HTTP $status."
  fi
  printf '%s\n' "$answer"
}

number() {
  [[ "${1:-}" =~ ^[1-9][0-9]{0,8}$ ]] || die "an issue number is required." 2
  printf '%s' "$1"
}

text_arg() {
  if [ "${1:-}" = "-" ]; then
    cat
  else
    printf '%s' "${1:-}"
  fi
}

command="${1:-}"
[ $# -gt 0 ] && shift
case "$command" in
  event)
    if [ -n "${FORKMESH_EVENT_PATH:-}" ] && [ -r "$FORKMESH_EVENT_PATH" ]; then
      cat "$FORKMESH_EVENT_PATH"
      printf '\n'
    else
      api GET /event
    fi
    ;;
  issue)
    n="$(number "${1:-}")"
    api GET "/issues/$n"
    ;;
  comment)
    n="$(number "${1:-}")"
    [ $# -ge 2 ] || die "comment needs a body (or - for standard input)." 2
    body="$(text_arg "$2")"
    [ -n "$body" ] || die "the comment is empty." 2
    api POST "/issues/$n/comments" "$(json_object str body "$body")"
    ;;
  label)
    n="$(number "${1:-}")"
    shift
    add="" remove=""
    while [ $# -gt 0 ]; do
      case "$1" in
        --add) add="${2:-}"; shift 2 ;;
        --add=*) add="${1#*=}"; shift ;;
        --remove) remove="${2:-}"; shift 2 ;;
        --remove=*) remove="${1#*=}"; shift ;;
        *) die "unknown label option: $1" 2 ;;
      esac
    done
    [ -n "$add$remove" ] || die "label needs --add or --remove." 2
    api POST "/issues/$n/labels" "$(json_object list add "$add" list remove "$remove")"
    ;;
  close|reopen)
    n="$(number "${1:-}")"
    state="closed"
    [ "$command" = "reopen" ] && state="open"
    api POST "/issues/$n/state" "$(json_object str state "$state")"
    ;;
  open-issue)
    [ -n "${1:-}" ] || die "open-issue needs a title." 2
    [ $# -ge 2 ] || die "open-issue needs a body (or - for standard input)." 2
    title="$1"
    body="$(text_arg "$2")"
    api POST /issues "$(json_object str title "$title" str body "$body" list labels "${3:-}")"
    ;;
  -h|--help|help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
