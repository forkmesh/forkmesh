#!/usr/bin/env bash
set -euo pipefail
DEPLOY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPLOY_SELF="$DEPLOY_DIR/$(basename "${BASH_SOURCE[0]}")"
cd "$DEPLOY_DIR"

. ./pywrangler.sh

ENV_FILE=".env.production"
USE_WRANGLER_OAUTH="${FORKMESH_USE_WRANGLER_OAUTH:-0}"
# The checked-in canonical script name is forkmesh-next-app. The one-time prepare
# stage explicitly sets FORKMESH_APP_WORKER_NAME=forkmesh-relay so consolidated
# code and secrets land on the existing script before its in-place UUID rename.
APP_WORKER_NAME="${FORKMESH_APP_WORKER_NAME:-forkmesh-next-app}"
# Wrangler otherwise takes the secret target from wrangler.toml, which may
# already carry the final name while the prepare deploy must update the
# existing relay script. Keep code and secret publication on one exact target.
SPLIT_SECRET_WORKER="$APP_WORKER_NAME"
# During the one-time edge removal, the old specific routes continue to win
# until an operator removes them after the App and its snapshot rows are ready.
APP_CONSOLIDATION_PREPARE="${FORKMESH_APP_CONSOLIDATION_PREPARE:-0}"
if [ "$APP_CONSOLIDATION_PREPARE" = "1" ] && \
   [ "${FORKMESH_APP_WORKER_NAME:-}" != "forkmesh-relay" ]; then
    echo "ERROR: consolidation prepare must explicitly target the existing forkmesh-relay script." >&2
    echo "       Set FORKMESH_APP_WORKER_NAME=forkmesh-relay with FORKMESH_APP_CONSOLIDATION_PREPARE=1." >&2
    exit 2
fi
if [ "$APP_CONSOLIDATION_PREPARE" = "1" ] && \
   [ "${FORKMESH_LEGACY_EDGE_QUIESCED:-0}" != "1" ]; then
    echo "ERROR: consolidation prepare requires a quiesced legacy edge schedule and pre-expired legacy rows." >&2
    echo "       Complete that control-plane precondition, then set FORKMESH_LEGACY_EDGE_QUIESCED=1." >&2
    exit 2
fi

# Every deploy command also accepts --force (-f): deploy the target even when
# its live fingerprint already matches the local inputs and nothing changed.
# The flag is exported so the recursive "deploy"/"changed" runs force each
# per-target deploy they invoke.
DEPLOY_COMMAND="${1:-deploy}"
case "$DEPLOY_COMMAND" in
    -*)
        # A bare "deploy.sh --force" still runs the default deploy command.
        DEPLOY_COMMAND="deploy"
        ;;
    *)
        if [ "$#" -gt 0 ]; then
            shift
        fi
        ;;
esac
DEPLOY_ARGS=()
for deploy_arg in "$@"; do
    case "$deploy_arg" in
        --force|-f)
            export FORKMESH_FORCE_DEPLOY=1
            ;;
        *)
            DEPLOY_ARGS+=("$deploy_arg")
            ;;
    esac
done
set -- "$DEPLOY_COMMAND" ${DEPLOY_ARGS[@]+"${DEPLOY_ARGS[@]}"}

trim() {
    local s="$1"
    s="${s%$'\r'}"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

# Secret inventory is a read-only Cloudflare API call. A clean Actions sandbox
# has no npm or workers-py cache, so making this preservation gate depend on a
# package-manager bootstrap adds a failure mode before the request is even
# sent. Use the API directly when no explicit pywrangler installation was
# supplied. The token is passed to curl over stdin (not argv), and the response
# is reduced to the same JSON array Wrangler returns. Local/test environments
# that deliberately provide pywrangler retain that transport.
cloudflare_worker_secret_inventory() {
    local account="${CLOUDFLARE_ACCOUNT_ID:-}"
    local token="${CLOUDFLARE_API_TOKEN:-}"
    local worker="$SPLIT_SECRET_WORKER"
    local endpoint response
    command -v curl >/dev/null 2>&1 || return 1
    command -v python3 >/dev/null 2>&1 || return 1
    [[ "$account" =~ ^[A-Za-z0-9_-]+$ ]] || return 1
    [[ "$worker" =~ ^[A-Za-z0-9_-]+$ ]] || return 1
    [[ "$token" =~ ^[A-Za-z0-9._~-]+$ ]] || return 1
    endpoint="https://api.cloudflare.com/client/v4/accounts/$account/workers/scripts/$worker/secrets"
    response="$({
        printf 'header = "Authorization: Bearer %s"\n' "$token"
        printf '%s\n' 'header = "Accept: application/json"'
    } | curl --config - --fail --silent --show-error \
        --connect-timeout 10 --max-time 30 --retry 2 --retry-delay 2 \
        --retry-all-errors "$endpoint")" || return 1
    printf '%s' "$response" | python3 -c '
import json
import sys

payload = json.load(sys.stdin)
result = payload.get("result") if isinstance(payload, dict) else None
if payload.get("success") is not True or not isinstance(result, list):
    raise SystemExit(1)
if any(not isinstance(item, dict) or not isinstance(item.get("name"), str)
       for item in result):
    raise SystemExit(1)
json.dump(result, sys.stdout, separators=(",", ":"))
' || return 1
}

list_worker_secrets() {
    local attempt output use_direct_api=1
    if _pywrangler_executable_is_usable "$PYWRANGLER_BIN"; then
        use_direct_api=0
    fi
    for attempt in 1 2 3; do
        if [ "$use_direct_api" = "1" ]; then
            output="$(cloudflare_worker_secret_inventory 2>/dev/null)" || output=""
            [ -n "$output" ] || {
                [ "$attempt" = "3" ] || sleep $((attempt * 2))
                continue
            }
        elif ! output="$(pywrangler secret list --env "" \
            ${SPLIT_SECRET_WORKER:+--name "$SPLIT_SECRET_WORKER"} 2>/dev/null)"; then
            [ "$attempt" = "3" ] || sleep $((attempt * 2))
            continue
        fi
        if [ -n "$output" ]; then
            printf '%s\n' "$output"
            return 0
        fi
        [ "$attempt" = "3" ] || sleep $((attempt * 2))
    done
    return 1
}

VAR_ARGS=()
CF_ACCOUNT_ID_SET=0
EDGE_ROUTER_PUBLIC_KEY=""
EDGE_ROUTER_SIGNING_SEED=""
if [ -f "$ENV_FILE" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|'#'*) continue ;; esac   # skip blanks/comments
        case "$line" in *=*) ;; *) continue ;; esac # skip lines without KEY=VALUE
        key="$(trim "${line%%=*}")"
        value="$(trim "${line#*=}")"
        [ -z "$key" ] && continue
        if [ "$key" = "MIRROR_ROUTER_PUBLIC_KEY" ]; then
            EDGE_ROUTER_PUBLIC_KEY="$value"
        elif [ "$key" = "MIRROR_ROUTER_SIGNING_SEED" ]; then
            EDGE_ROUTER_SIGNING_SEED="$value"
        fi
        case "$key" in
            CLOUDFLARE_ACCOUNT_ID)
                if [ -n "$value" ]; then
                    CF_ACCOUNT_ID_SET=1
                    export "$key=$value"
                fi
                continue
                ;;
            CLOUDFLARE_API_TOKEN)
                if [ "$USE_WRANGLER_OAUTH" != "1" ] && [ -n "$value" ]; then
                    export "$key=$value"
                fi
                continue
                ;;
            CLOUDFLARE_*)
                [ -n "$value" ] && export "$key=$value"
                continue
                ;;
        esac
        VAR_ARGS+=(--var "${key}:${value}")          # .env uses =, wrangler uses :
    done < "$ENV_FILE"
fi

require_cloudflare_account() {
    if [ "$CF_ACCOUNT_ID_SET" = "1" ] || [ -n "${CLOUDFLARE_ACCOUNT_ID:-}" ]; then
        return 0
    fi
    echo "ERROR: CLOUDFLARE_ACCOUNT_ID is missing." >&2
    echo "       Set it in $ENV_FILE (or the CI secret that writes that file) before deploying." >&2
    return 1
}

adopt_cloudflare_token_alias() {
    [ -n "${CLOUDFLARE_API_TOKEN:-}" ] && return 0
    local name val
    for name in CF_API_TOKEN CLOUDFLARE_TOKEN CF_TOKEN; do
        val="$(trim "${!name:-}")"
        if [ -n "$val" ]; then
            export CLOUDFLARE_API_TOKEN="$val"
            echo "note: found a Cloudflare token in \$$name; using it as CLOUDFLARE_API_TOKEN." >&2
            return 0
        fi
    done
    return 1
}

require_cloudflare_auth() {
    if [ "$USE_WRANGLER_OAUTH" = "1" ]; then
        unset CLOUDFLARE_API_TOKEN
        echo "note: using the existing Wrangler OAuth session for this deploy." >&2
        return 0
    fi
    adopt_cloudflare_token_alias || true
    [ -n "${CLOUDFLARE_API_TOKEN:-}" ] && return 0
    if [ -t 0 ] && [ -t 1 ]; then
        return 0   # a human at a terminal: let wrangler do its OAuth login
    fi
    echo "ERROR: no Cloudflare credentials for a non-interactive deploy." >&2
    echo "       wrangler can't open an interactive OAuth login here, so set a" >&2
    echo "       scoped API token (permissions: Workers Scripts: Edit + D1: Edit)." >&2
    echo "       In ForkMesh: Settings -> Secrets & Coves, add CLOUDFLARE_API_TOKEN" >&2
    echo "       (and CLOUDFLARE_ACCOUNT_ID); the deploy workflow substitutes those" >&2
    echo "       vars into $ENV_FILE for you. Outside ForkMesh, set it directly in" >&2
    echo "       $ENV_FILE (deploy.sh exports it for wrangler):" >&2
    echo "         CLOUDFLARE_API_TOKEN=..." >&2
    echo "       Create one: https://developers.cloudflare.com/fundamentals/api/get-started/create-token/" >&2
    local seen
    seen="$(compgen -v 2>/dev/null | grep -E '^(CF_|CLOUDFLARE_)' \
        | grep -v '^CLOUDFLARE_ACCOUNT_ID$' \
        | awk 'NR>1{printf ", "} {printf "%s",$0}' || true)"
    if [ -n "$seen" ]; then
        echo "       NOTE: this run's environment has Cloudflare-related vars named:" >&2
        echo "         $seen" >&2
        echo "       If one of those holds your API token, rename it to" >&2
        echo "       CLOUDFLARE_API_TOKEN in Settings -> Secrets & Coves." >&2
    fi
    return 1
}

build_rev() {
    local rev
    if rev="$(git rev-parse --short=12 HEAD 2>/dev/null)" && [ -n "$rev" ]; then
        if ! git diff --quiet HEAD 2>/dev/null || \
           [ -n "$(git status --porcelain 2>/dev/null)" ]; then
            rev="${rev}-dirty-$(date -u +%s)"
        fi
        printf '%s' "$rev"
    else
        printf 'ts-%s' "$(date -u +%Y%m%d%H%M%S)"
    fi
}

app_version() {
    local cmake="../desktop/CMakeLists.txt"
    [ -f "$cmake" ] || return 0
    sed -n 's/^project(ForkMesh VERSION \([0-9][0-9.]*\).*/\1/p' "$cmake" | head -n1
}

signal_deploy_status() {
    local state="$1"
    local revision="$2"
    local now_ms
    local sql
    now_ms="$(date -u +%s)000"
    case "$state" in
        deploying)
            sql="INSERT INTO world_deploy_status(singleton,state,revision,started_at,finished_at) VALUES(1,'deploying','$revision',$now_ms,0) ON CONFLICT(singleton) DO UPDATE SET state='deploying',revision='$revision',started_at=$now_ms,finished_at=0"
            ;;
        ready|failed)
            sql="INSERT INTO world_deploy_status(singleton,state,revision,started_at,finished_at) VALUES(1,'$state','$revision',$now_ms,$now_ms) ON CONFLICT(singleton) DO UPDATE SET state='$state',revision='$revision',finished_at=$now_ms"
            ;;
        *)
            return 2
            ;;
    esac
    if ! pywrangler d1 execute "${FORKMESH_D1_NAME:-forkmesh}" --remote \
        --command "$sql"; then
        return 1
    fi
    echo "Deploy alert semaphore: state=$state revision=$revision at=$now_ms"
}

DEPLOY_SIGNAL_ACTIVE=0
mark_interrupted_deploy() {
    local exit_code=$?
    if [ "$DEPLOY_SIGNAL_ACTIVE" = "1" ] && [ -n "${BUILD_REV:-}" ]; then
        signal_deploy_status failed "$BUILD_REV" >/dev/null 2>&1 || true
    fi
    return "$exit_code"
}

build_dashboard_assets() {
    python3 tools/build_dashboard_assets.py
    python3 tools/build_worker_python.py
    python3 ../world/tools/build_worker_footprint.py --staged
}

_py_http_get() {
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - "$1" <<'PYEOF'
import sys, urllib.request
req = urllib.request.Request(sys.argv[1], headers={
    "User-Agent": "forkmesh-deploy-verify/1.0", "Accept": "application/json"})
try:
    with urllib.request.urlopen(req, timeout=25) as r:
        sys.stdout.write(r.read().decode(errors="replace"))
        sys.stdout.write("\n" + str(r.status))
except Exception as e:
    code = getattr(e, "code", None)
    if code is None:
        sys.exit(1)          # no HTTP response at all (DNS/TCP/TLS failure)
    sys.stdout.write("\n" + str(code))
PYEOF
}

verify_deploy() {
    local expected="$1"
    local endpoint="${2:-/api/version}"
    local base="${3:-${DEPLOY_VERIFY_URL:-https://app.forkmesh.com}}"
    local expected_fingerprint="${4:-}"
    local expected_router_public_key="${5:-}"
    base="${base%/}"
    case "$endpoint" in
        /*) ;;
        *)
            echo "ERROR: deploy verification endpoint must be an absolute path." >&2
            return 2
            ;;
    esac
    local url="$base$endpoint"
    echo "Verifying $url is serving BUILD_REV=$expected ..."
    local attempts="${FORKMESH_DEPLOY_VERIFY_ATTEMPTS:-30}"
    local max_time="${FORKMESH_DEPLOY_VERIFY_TIMEOUT:-25}"
    local sleep_s="${FORKMESH_DEPLOY_VERIFY_BACKOFF:-6}"
    local attempt body got worker fingerprint router_public_key router_ready
    local http_code curl_rc via
    for attempt in $(seq 1 "$attempts"); do
        if command -v curl >/dev/null 2>&1; then
            curl_rc=0
            via=curl
            body="$(curl -sS --max-time "$max_time" -w $'\n%{http_code}' "$url" 2>/dev/null)" || curl_rc=$?
            if [ "$curl_rc" != 0 ] && body="$(_py_http_get "$url")"; then
                curl_rc=0
                via=python3
            fi
        else
            curl_rc=0
            via=python3
            body="$(_py_http_get "$url")" || curl_rc=$?
        fi
        http_code="${body##*$'\n'}"
        body="${body%$'\n'*}"
        got="$(printf '%s' "$body" | sed -n 's/.*"rev"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
        worker="$(printf '%s' "$body" | sed -n 's/.*"worker"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
        fingerprint="$(printf '%s' "$body" | sed -n 's/.*"deployFingerprint"[[:space:]]*:[[:space:]]*"\([0-9a-f]\{64\}\)".*/\1/p')"
        router_public_key="$(printf '%s' "$body" | sed -n 's/.*"routerPublicKey"[[:space:]]*:[[:space:]]*"\([A-Za-z0-9_-]\{43\}\)".*/\1/p')"
        router_ready="$(printf '%s' "$body" | sed -n 's/.*"routerIdentityReady"[[:space:]]*:[[:space:]]*\(true\|false\).*/\1/p')"
        if [ "$curl_rc" = 0 ] && [ "$http_code" = "200" ] && \
           [ "$got" = "$expected" ] && [ "$worker" = "app" ] && \
           { [ -z "$expected_fingerprint" ] || \
             [ "$fingerprint" = "$expected_fingerprint" ]; } && \
           { [ -z "$expected_router_public_key" ] || \
             { [ "$router_public_key" = "$expected_router_public_key" ] && \
               [ "$router_ready" = "true" ]; }; }; then
            echo "Verified: live origin is serving build $expected (via $via)."
            if [ "$via" = "python3" ]; then
                echo "note: curl could not reach $url but python3 could — a host" >&2
                echo "      firewall rule is likely blocking the curl binary itself" >&2
                echo "      (e.g. an OpenSnitch 'deny /usr/bin/curl' rule)." >&2
            fi
            return 0
        fi
        if [ "$curl_rc" != 0 ]; then
            echo "  attempt $attempt/$attempts: curl failed (exit $curl_rc, e.g. timeout/DNS/TLS); retrying in ${sleep_s}s..." >&2
        else
            echo "  attempt $attempt/$attempts: HTTP $http_code, live rev='${got:-<none>}', worker='${worker:-<none>}', fingerprint='${fingerprint:-<none>}', router-ready='${router_ready:-<none>}' (want '$expected' via app with the deployed identity, via $via); retrying in ${sleep_s}s..." >&2
        fi
        sleep "$sleep_s"
    done
    echo "ERROR: $base never reported BUILD_REV=$expected after the deploy." >&2
    if [ "$curl_rc" != 0 ]; then
        echo "       Last attempt failed to connect (curl exit $curl_rc) — check network egress" >&2
        echo "       from the deploy runner and that app.forkmesh.com resolves and is reachable." >&2
    elif [ "$http_code" != "200" ]; then
        echo "       Last attempt got HTTP $http_code from $url (expected 200). That's a" >&2
        echo "       server-side/routing error, not a stale-code mismatch — check the Worker's" >&2
        echo "       error log (admin dashboard) for what's failing on that origin." >&2
    else
        echo "       Last live rev/fingerprint was '${got:-<none>}'/'${fingerprint:-<none>}'. The upload did NOT take effect on" >&2
        echo "       this origin (most likely it hit the wrong Cloudflare account, or the" >&2
        echo "       custom domain still routes to an old Worker). Check that" >&2
        echo "       CLOUDFLARE_ACCOUNT_ID in $ENV_FILE matches the account that owns" >&2
        echo "       app.forkmesh.com, then redeploy." >&2
    fi
    return 1
}

_json_string_field() {
    local field="$1"
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - "$field" 3<&0 <<'PYEOF'
import json
import os
import sys

try:
    value = json.load(os.fdopen(3)).get(sys.argv[1], "")
except (AttributeError, json.JSONDecodeError, OSError, TypeError):
    raise SystemExit(1)
if not isinstance(value, str):
    raise SystemExit(1)
sys.stdout.write(value)
PYEOF
}

_json_boolean_field() {
    local field="$1"
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - "$field" 3<&0 <<'PYEOF'
import json
import os
import sys

try:
    value = json.load(os.fdopen(3)).get(sys.argv[1])
except (AttributeError, json.JSONDecodeError, OSError, TypeError):
    raise SystemExit(1)
if value is True:
    sys.stdout.write("true")
elif value is False:
    sys.stdout.write("false")
else:
    raise SystemExit(1)
PYEOF
}

_router_identity_is_disabled() {
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - 3<&0 <<'PYEOF'
import json
import os

try:
    value = json.load(os.fdopen(3))
except (json.JSONDecodeError, OSError, TypeError):
    raise SystemExit(1)
valid = (
    isinstance(value, dict)
    and value.get("ok") is False
    and value.get("protocol") == "forkmesh-masked-proxy-v1"
    and value.get("registration") == "forkmesh-https-endpoint-v1"
    and value.get("routerPublicKey") == ""
)
raise SystemExit(0 if valid else 1)
PYEOF
}

_data_key_readiness_is_proven() {
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - 3<&0 <<'PYEOF'
import json
import os

try:
    value = json.load(os.fdopen(3))
except (json.JSONDecodeError, OSError, TypeError):
    raise SystemExit(1)
valid = (
    isinstance(value, dict)
    and value.get("ok") is True
    and value.get("database") is True
    and value.get("encryption") is True
)
raise SystemExit(0 if valid else 1)
PYEOF
}

_http_json_get() {
    local url="$1" response status
    if command -v curl >/dev/null 2>&1; then
        response="$(curl -sS --max-time 25 -w $'\n%{http_code}' \
            -H "Accept: application/json" "$url" 2>/dev/null)" || response=""
    else
        response="$(_py_http_get "$url")" || response=""
    fi
    [ -n "$response" ] || return 1
    status="${response##*$'\n'}"
    [ "$status" = "200" ] || return 1
    printf '%s' "${response%$'\n'*}"
}

live_router_public_key() {
    local base="${1:-$(app_control_verify_url)}" body key
    body="$(_http_json_get "${base%/}/api/mirrors/https")" || return 1
    key="$(printf '%s' "$body" | _json_string_field routerPublicKey)" || return 1
    [[ "$key" =~ ^[A-Za-z0-9_-]{43}$ ]] || return 1
    printf '%s' "$key"
}

live_router_public_key_with_retry() {
    local base="${1:-$(app_control_verify_url)}"
    local attempts="${FORKMESH_ROUTER_VERIFY_ATTEMPTS:-10}"
    local sleep_s="${FORKMESH_ROUTER_VERIFY_BACKOFF:-3}"
    local attempt live_key=""
    for attempt in $(seq 1 "$attempts"); do
        if live_key="$(live_router_public_key "$base")"; then
            printf '%s' "$live_key"
            return 0
        fi
        if [ "$attempt" -lt "$attempts" ]; then
            echo "  attempt $attempt/$attempts: mirror-router identity is unavailable; retrying in ${sleep_s}s..." >&2
            sleep "$sleep_s"
        fi
    done
    return 1
}

verify_live_router_public_key() {
    local expected="$1"
    local base="${2:-$(app_control_verify_url)}"
    local attempts="${FORKMESH_ROUTER_VERIFY_ATTEMPTS:-${3:-10}}"
    local sleep_s="${FORKMESH_ROUTER_VERIFY_BACKOFF:-${4:-3}}" attempt live=""
    for attempt in $(seq 1 "$attempts"); do
        live="$(live_router_public_key "$base")" || live=""
        if [ "$live" = "$expected" ]; then
            echo "Verified the live mirror-router public identity."
            return 0
        fi
        if [ "$attempt" -lt "$attempts" ]; then
            echo "  attempt $attempt/$attempts: mirror-router identity is unavailable or different; retrying in ${sleep_s}s..." >&2
            sleep "$sleep_s"
        fi
    done
    echo "ERROR: ${base%/}/api/mirrors/https did not report the expected mirror-router public key." >&2
    return 1
}

_router_key_pair_matches() {
    command -v python3 >/dev/null 2>&1 || return 1
    python3 - 3<<<"$EDGE_ROUTER_PUBLIC_KEY" \
        4<<<"$EDGE_ROUTER_SIGNING_SEED" <<'PYEOF'
import base64
import hashlib
import os


def decode(value):
    if len(value) != 43:
        raise ValueError
    raw = base64.b64decode(value + "=", altchars=b"-_", validate=True)
    if len(raw) != 32:
        raise ValueError
    canonical = base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")
    if canonical != value:
        raise ValueError
    return raw


def add(left, right):
    prime = 2**255 - 19
    curve_d = -121665 * pow(121666, prime - 2, prime) % prime
    x1, y1 = left
    x2, y2 = right
    product = x1 * x2 * y1 * y2 % prime
    x3 = (x1 * y2 + y1 * x2) * pow(1 + curve_d * product, prime - 2, prime)
    y3 = (y1 * y2 + x1 * x2) * pow(1 - curve_d * product, prime - 2, prime)
    return x3 % prime, y3 % prime


def public_from_seed(seed):
    digest = hashlib.sha512(seed).digest()
    scalar = int.from_bytes(digest[:32], "little")
    scalar = (scalar & ((1 << 254) - 8)) | (1 << 254)
    point = (0, 1)
    addend = (
        15112221349535400772501151409588531511454012693041857206046113283949847762202,
        46316835694926478169428394003475163141307993866256225615783033603165251855960,
    )
    while scalar:
        if scalar & 1:
            point = add(point, addend)
        addend = add(addend, addend)
        scalar >>= 1
    x, y = point
    return (y | ((x & 1) << 255)).to_bytes(32, "little")


try:
    public = decode(os.fdopen(3).read().strip())
    seed = decode(os.fdopen(4).read().strip())
except (OSError, UnicodeError, ValueError):
    raise SystemExit(1)
raise SystemExit(0 if public_from_seed(seed) == public else 1)
PYEOF
}

official_multi_host_enabled() {
    grep -q 'PUBLIC_BASE_URL = "https://forkmesh.com"' wrangler.toml
}

_split_check() {
    local url="$1" want_worker="$2" want_status="$3"
    local attempts=10 sleep_s=3 attempt headers status worker
    for attempt in $(seq 1 "$attempts"); do
        headers="$(curl -sSI --max-time 20 "$url" 2>/dev/null || true)"
        status="$(printf '%s\n' "$headers" | awk 'toupper($1) ~ /^HTTP\// { code=$2 } END { print code }')"
        worker="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-worker" { value=$2 } END { sub(/\r$/, "", value); print value }')"
        if [ "$status" = "$want_status" ] && [ "$worker" = "$want_worker" ]; then
            return 0
        fi
        sleep "$sleep_s"
    done
    echo "ERROR: $url answered HTTP ${status:-<none>} via '${worker:-unknown}' (expected $want_status via '$want_worker')." >&2
    return 1
}

_status_check() {
    local url="$1" want_status="$2"
    local attempts=10 sleep_s=3 attempt status
    for attempt in $(seq 1 "$attempts"); do
        status="$(curl -sS -o /dev/null -w '%{http_code}' --max-time 20 "$url" 2>/dev/null || true)"
        if [ "$status" = "$want_status" ]; then
            return 0
        fi
        sleep "$sleep_s"
    done
    echo "ERROR: $url answered HTTP ${status:-<none>} (expected $want_status)." >&2
    return 1
}

_static_fingerprint_check() {
    local target="$1" base="$2" expected="$3"
    local mode="${4:-verify}"
    # WWW's root document intentionally bypasses Worker execution through
    # Static Assets, so it cannot report the Worker environment fingerprint.
    # Probe a Worker-first static route instead. World runs its root first and
    # can continue using `/`.
    local probe_path="/"
    [ "$target" = "www" ] && probe_path="/rss.xml"
    local attempts="${FORKMESH_DEPLOY_VERIFY_ATTEMPTS:-30}"
    local sleep_s="${FORKMESH_DEPLOY_VERIFY_BACKOFF:-6}"
    local attempt headers live="" streak=0
    for attempt in $(seq 1 "$attempts"); do
        headers="$(curl -sSI --max-time 20 \
            -H "Cache-Control: no-cache" \
            "${base%/}${probe_path}?forkmesh-deploy-check=${expected:0:16}" \
            2>/dev/null || true)"
        live="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-deploy-fingerprint" { value=$2 } END { sub(/\r$/, "", value); print value }')"
        if [ -n "$live" ] && [ "$live" = "$expected" ]; then
            streak=$((streak + 1))
            if [ "$streak" -ge 2 ]; then
                return 0
            fi
        else
            streak=0
            if [ "$mode" = "probe" ]; then
                return 1
            fi
        fi
        if [ "$attempt" -lt "$attempts" ]; then
            if [ "$mode" != "probe" ]; then
                echo "  attempt $attempt/$attempts: waiting for two consecutive $target deployment fingerprint matches; current streak=$streak; retrying in ${sleep_s}s..." >&2
            fi
            sleep "$sleep_s"
        fi
    done
    if [ "$mode" != "probe" ]; then
        echo "ERROR: $target did not report the expected deployment fingerprint twice consecutively after $attempts attempts." >&2
    fi
    return 1
}

_secret_bulk_error_is_transient() {
    case "$1" in
        *"code: 10013"*|\
        *"Internal Server Error"*|*"Bad Gateway"*|*"Service Unavailable"*|\
        *"Gateway Time-out"*|*"Gateway Timeout"*|\
        *"fetch failed"*|*"socket hang up"*|*"ECONNRESET"*|*"ETIMEDOUT"*|\
        *"EAI_AGAIN"*|*"Client network socket disconnected"*)
            return 0
            ;;
    esac
    return 1
}

verify_remote_data_key() {
    local value="$1"
    local action="${2:-publish}"
    local base="${DEPLOY_VERIFY_URL:-https://app.forkmesh.com}"
    local attempts="${FORKMESH_DATA_KEY_VERIFY_ATTEMPTS:-4}"
    local sleep_s="${FORKMESH_DATA_KEY_VERIFY_BACKOFF:-3}"
    local attempt timestamp proof response status body curl_rc
    for attempt in $(seq 1 "$attempts"); do
        timestamp="$(date -u +%s)000"
        proof="$(python3 - "$timestamp" 3< <(printf '%s' "$value") <<'PYEOF'
import hashlib
import hmac
import os
import sys

key = os.fdopen(3, "rb").read()
canonical = b"forkmesh-bootstrap-readiness-v1\n" + sys.argv[1].encode("ascii")
print(hmac.new(key, canonical, hashlib.sha256).hexdigest())
PYEOF
)" || return 1
        curl_rc=0
        response="$(curl -sS --max-time 25 \
            -H "Accept: application/json" \
            -H "Authorization: Bearer $proof" \
            -H "X-ForkMesh-Readiness-Timestamp: $timestamp" \
            -w $'\n%{http_code}' \
            "${base%/}/api/bootstrap/readiness" 2>/dev/null)" || curl_rc=$?
        if [ "$curl_rc" != "0" ]; then
            if [ "$attempt" -lt "$attempts" ]; then
                echo "  attempt $attempt/$attempts: DATA_KEY readiness transport failed; retrying in ${sleep_s}s..." >&2
                sleep "$sleep_s"
                continue
            fi
            echo "ERROR: the live DATA_KEY readiness probe could not be reached after $attempts attempts." >&2
            echo "       The existing remote key was not changed; retry when the App is reachable." >&2
            return 1
        fi
        status="${response##*$'\n'}"
        body="${response%$'\n'*}"
        if [ "$status" = "404" ] && [ "$action" = "validate" ]; then
            echo "  first-cutover preflight: the live Worker predates authenticated DATA_KEY readiness"
            echo "  preserving its remote DATA_KEY; the new Worker must prove the local recovery key before secrets publish"
            return 3
        fi
        case "$status" in
            5[0-9][0-9])
                if [ "$attempt" -lt "$attempts" ]; then
                    echo "  attempt $attempt/$attempts: DATA_KEY readiness returned HTTP $status; retrying in ${sleep_s}s..." >&2
                    sleep "$sleep_s"
                    continue
                fi
                if [ "$action" = "validate" ] && \
                   [ "${FORKMESH_RECOVER_UNAVAILABLE_HTTP:-0}" = "1" ]; then
                    echo "  outage recovery preflight: preserving the existing remote DATA_KEY while HTTP readiness is unavailable"
                    echo "  the post-deploy secret publication must still authenticate the local recovery key"
                    return 3
                fi
                echo "ERROR: the live DATA_KEY readiness probe remained unavailable (HTTP $status) after $attempts attempts." >&2
                echo "       The existing remote key was not changed; retry when the App is healthy." >&2
                return 1
                ;;
        esac
        if [ "$status" != "200" ]; then
            echo "ERROR: local DATA_KEY did not authenticate against the live Worker (HTTP ${status:-<none>})." >&2
            echo "       The existing remote key was not changed. Restore the matching recovery value before deploying." >&2
            return 1
        fi
        if ! printf '%s' "$body" | _data_key_readiness_is_proven; then
            echo "ERROR: live DATA_KEY readiness did not prove database encryption." >&2
            return 1
        fi
        return 0
    done
    echo "ERROR: the live DATA_KEY readiness probe exhausted its retry budget." >&2
    return 1
}

push_secrets() {
    local action="${1:-publish}"
    case "$action" in
        validate|publish|publish-preserve-router) ;;
        *)
            echo "ERROR: invalid internal push_secrets action." >&2
            return 2
            ;;
    esac
    if [ ! -f "$ENV_FILE" ]; then
        echo "ERROR: $ENV_FILE not found; production secrets cannot be validated or published." >&2
        echo "       Copy .env.production.example, supply real values, and keep the file untracked." >&2
        return 1
    fi
    local required=" ADMIN_PATH ADMIN_PASS MAILTRAP_API_TOKEN MAILTRAP_WEBHOOK_SECRET DATA_KEY TREASURY_SOLANA_ADDRESS MIRROR_ROUTER_PUBLIC_KEY MIRROR_ROUTER_SIGNING_SEED DISCORD_CLIENT_ID DISCORD_CLIENT_SECRET DISCORD_BOT_TOKEN APNS_AUTH_KEY APNS_KEY_ID APNS_TEAM_ID "
    local allowed=" ADMIN_ALERT_EMAILS ADMIN_PASS ADMIN_PATH APNS_AUTH_KEY APNS_KEY_ID APNS_TEAM_ID COMMUNITY_REWARD_POOL_ADDRESS DATA_KEY DISCORD_BOT_TOKEN DISCORD_CLIENT_ID DISCORD_CLIENT_SECRET DISCORD_OAUTH_REDIRECT_URI MAILTRAP_API_TOKEN MAILTRAP_API_URL MAILTRAP_SENDER MAILTRAP_SENDER_NAME MAILTRAP_WEBHOOK_SECRET MIRROR_ROUTER_PUBLIC_KEY MIRROR_ROUTER_SIGNING_SEED PATREON_CAMPAIGN_ID PATREON_CLIENT_ID PATREON_CLIENT_SECRET PATREON_CREATOR_ACCESS_TOKEN PATREON_WEBHOOK_SECRET PRODUCT_ANALYSIS_PASS PRODUCT_ANALYSIS_USER REWARD_SIGNER_ACCOUNT SENTRY_CRON_CHECKIN_MARGIN_MINUTES SENTRY_CRON_MAX_RUNTIME_MINUTES SENTRY_CRON_MONITOR_SLUG SENTRY_CRON_TIMEZONE SENTRY_DSN SSH_GATEWAY_ALLOWED_ADDRESSES SSH_GATEWAY_HOST SSH_GATEWAY_NODE_HOSTS SSH_GATEWAY_PORT SSH_GATEWAY_REPOSITORIES SSH_GATEWAY_TOKEN SSH_GATEWAY_TOKEN_NEXT TREASURY_SOLANA_ADDRESS USAGE_ANALYTICS_API_TOKEN WORKERS_OBSERVABILITY_ACCOUNT_ID WORKERS_OBSERVABILITY_API_TOKEN WORKERS_OBSERVABILITY_SERVICE X_API_BEARER_TOKEN PACKAGE_CRM_ZOHO_OAUTH_CLIENT_ID PACKAGE_CRM_ZOHO_OAUTH_CLIENT_SECRET PACKAGE_CRM_ZOHO_OAUTH_REDIRECT_URI "

    if [ "$action" = "validate" ]; then
        echo "Validating production secrets in: $(cd "$(dirname "$ENV_FILE")" && pwd)/$(basename "$ENV_FILE")"
    elif [ "$action" = "publish-preserve-router" ]; then
        echo "Pushing non-router secrets from: $(cd "$(dirname "$ENV_FILE")" && pwd)/$(basename "$ENV_FILE")"
    else
        echo "Pushing secrets from: $(cd "$(dirname "$ENV_FILE")" && pwd)/$(basename "$ENV_FILE")"
    fi
    local count=0
    local pushed=()
    local secret_values=()
    local empties=()
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|'#'*) continue ;; esac
        case "$line" in *=*) ;; *) continue ;; esac
        local key value
        key="$(trim "${line%%=*}")"
        value="$(trim "${line#*=}")"
        [ -z "$key" ] && continue
        case "$key" in
            CLOUDFLARE_ACCOUNT_ID)
                key="WORKERS_OBSERVABILITY_ACCOUNT_ID"
                ;;
            CLOUDFLARE_OBSERVABILITY_API_TOKEN)
                key="WORKERS_OBSERVABILITY_API_TOKEN"
                ;;
            CLOUDFLARE_*)
                continue
                ;;
            VULTR_API_KEY|VULTR_API_TOKEN|VULTR_TOKEN|VULTR_KEY)
                continue
                ;;
        esac
        case "$allowed" in
            *" $key "*) ;;
            *)
                echo "  skip (not an App runtime secret): $key" >&2
                continue
                ;;
        esac
        if [ -z "$value" ]; then
            echo "  skip (empty): $key" >&2
            empties+=("$key")
            continue
        fi
        case "$value" in
            *CHANGE-ME*|change-me*)
                echo "  warning: $key still has a placeholder value" >&2 ;;
        esac
        case "$key" in
            APNS_KEY_ID|APNS_TEAM_ID)
                # Apple's Key IDs and Team IDs are exactly ten characters of
                # A-Z0-9. Any other shape imports fine and is then refused on
                # every single push (403 InvalidProviderToken), silently.
                if ! [[ "$value" =~ ^[A-Z0-9]{10}$ ]]; then
                    echo "  WARNING: $key is not a ten-character A-Z0-9 Apple identifier; no push notification can be delivered until it is corrected." >&2
                fi
                ;;
        esac
        echo "  secret: $key"
        pushed+=("$key")
        secret_values+=("$value")
        count=$((count + 1))
    done < "$ENV_FILE"

    local remote_secrets=""
    if ! remote_secrets="$(list_worker_secrets)"; then
        if [ "${FORKMESH_FIRST_DEPLOY:-0}" != "1" ]; then
            echo "ERROR: the existing Worker secret names could not be read." >&2
            echo "       Refusing to risk replacing DATA_KEY. Use FORKMESH_FIRST_DEPLOY=1 only when no Worker exists yet." >&2
            return 1
        fi
        echo "  first deployment: no existing Worker secret set to preserve"
        remote_secrets="[]"
    fi

    local req missing_req=() remote_only=()
    for req in $required; do
        case " ${pushed[*]-} " in *" $req "*) ;; *) missing_req+=("$req") ;; esac
    done
    if [ "${#missing_req[@]}" -gt 0 ]; then
        local still_missing=()
        for req in "${missing_req[@]}"; do
            case "$remote_secrets" in
                *"\"$req\""*) remote_only+=("$req") ;;
                *) still_missing+=("$req") ;;
            esac
        done
        if [ "${#still_missing[@]}" -gt 0 ]; then
            echo "ERROR: required secret(s) are missing both locally and on the Worker: ${still_missing[*]}" >&2
            echo "       Set them (real values, not blank) and re-run './deploy.sh secrets'." >&2
            return 1
        fi
        echo "  preserving existing remote secret(s): ${remote_only[*]}"
    fi
    if [ "$action" = "publish-preserve-router" ]; then
        local router_secret
        for router_secret in MIRROR_ROUTER_PUBLIC_KEY MIRROR_ROUTER_SIGNING_SEED; do
            if [[ "$remote_secrets" != *"\"$router_secret\""* ]]; then
                echo "ERROR: $router_secret is not present on the existing App Worker." >&2
                echo "       Generic secret updates cannot create or rotate the mirror-router identity." >&2
                echo "       Use './deploy.sh rotate-mirror-router' with a matched local key pair." >&2
                return 1
            fi
        done
        local router_filtered_names=() router_filtered_values=()
        local router_index
        for router_index in "${!pushed[@]}"; do
            case "${pushed[$router_index]}" in
                MIRROR_ROUTER_PUBLIC_KEY|MIRROR_ROUTER_SIGNING_SEED) ;;
                *)
                    router_filtered_names+=("${pushed[$router_index]}")
                    router_filtered_values+=("${secret_values[$router_index]}")
                    ;;
            esac
        done
        pushed=("${router_filtered_names[@]}")
        secret_values=("${router_filtered_values[@]}")
        count="${#pushed[@]}"
        for router_secret in MIRROR_ROUTER_PUBLIC_KEY MIRROR_ROUTER_SIGNING_SEED; do
            case " ${remote_only[*]-} " in
                *" $router_secret "*) ;;
                *) remote_only+=("$router_secret") ;;
            esac
        done
        echo "  preserving the coordinated remote mirror-router key pair"
    fi
    if [[ "$remote_secrets" == *'"DATA_KEY"'* ]]; then
        local data_key_value="" filtered_names=() filtered_values=()
        local data_index
        for data_index in "${!pushed[@]}"; do
            if [ "${pushed[$data_index]}" = "DATA_KEY" ]; then
                data_key_value="${secret_values[$data_index]}"
            else
                filtered_names+=("${pushed[$data_index]}")
                filtered_values+=("${secret_values[$data_index]}")
            fi
        done
        if [ -z "$data_key_value" ]; then
            echo "ERROR: DATA_KEY exists remotely but no local recovery value is available." >&2
            echo "       Restore DATA_KEY in $ENV_FILE before deploying; the live secret cannot be exported." >&2
            return 1
        fi
        local data_key_proof="authenticated"
        if verify_remote_data_key "$data_key_value" "$action"; then
            :
        else
            local proof_status=$?
            if [ "$action" = "validate" ] && [ "$proof_status" = "3" ]; then
                data_key_proof="preserved for first cutover"
            else
                return "$proof_status"
            fi
        fi
        pushed=("${filtered_names[@]}")
        secret_values=("${filtered_values[@]}")
        count=$((count - 1))
        case " ${remote_only[*]-} " in
            *" DATA_KEY "*) ;;
            *) remote_only+=("DATA_KEY") ;;
        esac
        echo "  preserving $data_key_proof remote secret: DATA_KEY"
    fi
    local discord_client_id=""
    local index
    for index in "${!pushed[@]}"; do
        if [ "${pushed[$index]}" = "DISCORD_CLIENT_ID" ]; then
            discord_client_id="${secret_values[$index]}"
            break
        fi
    done
    if [ -n "$discord_client_id" ] && ! [[ "$discord_client_id" =~ ^[0-9]{17,20}$ ]]; then
        echo "ERROR: DISCORD_CLIENT_ID must be a 17-20 digit Discord application ID." >&2
        return 1
    fi
    if [ "$action" = "validate" ]; then
        echo "Validated ${#pushed[@]} local and ${#remote_only[@]} preserved remote production secret(s)."
        return 0
    fi

    if ! command -v python3 >/dev/null 2>&1; then
        echo "ERROR: python3 is required to encode the bulk secret payload." >&2
        return 1
    fi
    if ! (
        umask 077
        secret_bulk_file="$(mktemp "${TMPDIR:-/tmp}/forkmesh-worker-secrets.XXXXXX.json")" \
            || exit 1
        trap 'rm -f -- "$secret_bulk_file"' EXIT
        trap 'exit 129' HUP
        trap 'exit 130' INT
        trap 'exit 143' TERM
        chmod 0600 "$secret_bulk_file" || exit 1
        python3 - "$secret_bulk_file" 3< <(
            for index in "${!pushed[@]}"; do
                printf '%s\0%s\0' "${pushed[$index]}" "${secret_values[$index]}"
            done
        ) <<'PYEOF' || exit 1
import json
import os
import sys

raw = os.fdopen(3, "rb").read()
parts = raw.split(b"\0")
if not parts or parts[-1] != b"" or len(parts) % 2 != 1:
    raise SystemExit("invalid internal secret record stream")

payload = {}
for index in range(0, len(parts) - 1, 2):
    key = parts[index].decode("utf-8")
    value = parts[index + 1].decode("utf-8")
    payload[key] = value

with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(payload, stream, ensure_ascii=False, separators=(",", ":"))
PYEOF
        [ -s "$secret_bulk_file" ] || exit 1
        attempts="${FORKMESH_SECRET_BULK_ATTEMPTS:-4}"
        backoff="${FORKMESH_SECRET_BULK_BACKOFF:-5}"
        attempt=1
        while :; do
            if bulk_output="$(pywrangler secret bulk --env "" ${SPLIT_SECRET_WORKER:+--name "$SPLIT_SECRET_WORKER"} "$secret_bulk_file" 2>&1)"; then
                if [ -n "$bulk_output" ]; then
                    printf '%s\n' "$bulk_output"
                fi
                if [ "$attempt" -gt 1 ]; then
                    echo "Bulk secret update succeeded on attempt $attempt/$attempts."
                fi
                exit 0
            fi
            if [ -n "$bulk_output" ]; then
                printf '%s\n' "$bulk_output" >&2
            fi
            if ! _secret_bulk_error_is_transient "$bulk_output"; then
                exit 1
            fi
            if [ "$attempt" -ge "$attempts" ]; then
                echo "  hit a transient Cloudflare API error on all $attempts attempts." >&2
                exit 1
            fi
            echo "  attempt $attempt/$attempts hit a transient Cloudflare API error (the" >&2
            echo "  Worker settings endpoint 5xx'd); re-sending the same bulk update in ${backoff}s..." >&2
            sleep "$backoff"
            attempt=$((attempt + 1))
            backoff=$((backoff * 3))
        done
    ); then
        echo "ERROR: bulk secret update failed; no per-secret retry was attempted." >&2
        echo "       (Per-secret 'secret put' is deliberately never used: it publishes one" >&2
        echo "       Worker version per secret, restarting Durable Objects mid-deploy.)" >&2
        echo "       If the error above is Cloudflare's '[code: 10013] An unknown error has" >&2
        echo "       occurred', the settings endpoint is having a bad minute and every" >&2
        echo "       secret still holds its previous value — re-run './deploy.sh secrets'." >&2
        return 1
    fi
    echo "Pushed $count secret(s) from $ENV_FILE in one bulk update."

    local listed
    if listed="$(list_worker_secrets)"; then
        local missing=()
        local k
        for k in $required; do
            case "$listed" in *"\"$k\""*) ;; *) missing+=("$k") ;; esac
        done
        if [ "${#missing[@]}" -gt 0 ]; then
            echo "ERROR: these secrets did not register: ${missing[*]}" >&2
            echo "       re-run './deploy.sh secrets' or check 'pywrangler secret list'." >&2
            return 1
        fi
        echo "Verified every required secret is present on the Worker."
    else
        echo "ERROR: could not list Worker secrets after the bulk update." >&2
        echo "       Refusing to continue without proving preserved remote secrets still exist." >&2
        return 1
    fi
}

publish_release_binary() {
    local force="${1:-0}"
    if ! command -v cmake >/dev/null 2>&1; then
        if [ "$force" = "1" ]; then
            echo "ERROR: cmake is required to force-republish a release binary." >&2
            return 1
        fi
        echo "note: cmake not found — skipping release binary build." >&2
        return 0
    fi
    if ! command -v go >/dev/null 2>&1; then
        echo "ERROR: Go is required to publish the native mirror-node companion." >&2
        return 1
    fi

    local os arch
    os="$(uname -s)"
    arch="$(uname -m)"
    case "$os" in
        Linux)                            os="linux" ;;
        Darwin)                           os="macos" ;;
        MINGW*|MSYS*|CYGWIN*|Windows_NT)  os="windows" ;;
        *) os="$(printf '%s' "$os" | tr '[:upper:]' '[:lower:]')" ;;
    esac
    case "$arch" in
        x86_64|amd64|x64) arch="x86_64" ;;
        aarch64|arm64)    arch="arm64" ;;
    esac
    local asset="forkmesh-${os}-${arch}"
    [ "$os" = "windows" ] && asset="${asset}.exe"
    local mirror_asset="forkmesh-mirror-node-${os}-${arch}"
    [ "$os" = "windows" ] && mirror_asset="${mirror_asset}.exe"

    if [ -f ../.forkmesh/releases/latest/SHASUMS256.txt ] &&
       grep -q "  $asset" ../.forkmesh/releases/latest/SHASUMS256.txt 2>/dev/null &&
       grep -q "  $mirror_asset" ../.forkmesh/releases/latest/SHASUMS256.txt 2>/dev/null; then
        if [ "$force" != "1" ]; then
            echo "Release binary for $asset is already published."
            echo "To rebuild the same version from the current clean commit, run:" >&2
            echo "  FORKMESH_RELEASE_CAS=/path/to/served/cas ./deploy.sh republish-release-binary" >&2
            return 0
        fi
        echo "Force-republishing existing release asset $asset."
    fi

    local release_version release_tag tag_commit build_commit
    release_version="$(app_version)"
    if ! printf '%s' "$release_version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
        echo "ERROR: could not resolve a clean app version from desktop/CMakeLists.txt." >&2
        return 1
    fi
    release_tag="${FORKMESH_TAG:-v${release_version}}"
    build_commit="$(git -C .. log -1 --format=%H -- . \
        ':(exclude).forkmesh/releases/**' 2>/dev/null || true)"
    if ! printf '%s' "$build_commit" |
         grep -Eq '^([0-9a-f]{40}|[0-9a-f]{64})$'; then
        echo "ERROR: could not resolve the exact source commit for the release binary." >&2
        return 1
    fi
    if [ "$release_tag" != "v${release_version}" ]; then
        echo "ERROR: FORKMESH_TAG=$release_tag does not match app version v${release_version}." >&2
        return 1
    fi
    tag_commit="$(git -C .. rev-list -n1 "$release_tag" 2>/dev/null || true)"
    if ! printf '%s' "$tag_commit" |
         grep -Eq '^([0-9a-f]{40}|[0-9a-f]{64})$'; then
        echo "ERROR: $release_tag does not resolve to an exact Git commit." >&2
        return 1
    fi

    if [ "$force" = "1" ]; then
        if [ -z "${FORKMESH_RELEASE_CAS:-}" ]; then
            echo "ERROR: forced republish requires FORKMESH_RELEASE_CAS to name the served release CAS." >&2
            echo "       Refusing to publish metadata for bytes stored only in a throwaway default directory." >&2
            return 1
        fi
    fi
    local worktree_status
    if ! worktree_status="$(git -C .. status --porcelain=v1 \
            --untracked-files=all -- . \
            ':(exclude).forkmesh/releases/**' 2>&1)"; then
        echo "ERROR: could not verify that the release source worktree is clean." >&2
        printf '%s\n' "$worktree_status" >&2
        return 1
    fi
    if [ -n "$worktree_status" ]; then
        echo "ERROR: release binary build requires a clean source worktree." >&2
        echo "       Commit the exact code to publish, then retry." >&2
        return 1
    fi

    echo "Building and publishing ForkMesh v${release_version} for ${os}/${arch}…"

    local jobs total_kb=0 available_kb=0 reserve_kb usable_kb memory_jobs
    jobs="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )"
    [ "$jobs" -gt 4 ] && jobs=4
    if [ -r /proc/meminfo ]; then
        total_kb="$(awk '/^MemTotal:/ {print $2}' /proc/meminfo)"
        available_kb="$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)"
    fi
    if [ "${total_kb:-0}" -gt 0 ]; then
        memory_jobs=$((total_kb / (4 * 1024 * 1024)))
        [ "$memory_jobs" -lt 1 ] && memory_jobs=1
        [ "$jobs" -gt "$memory_jobs" ] && jobs="$memory_jobs"
    fi
    if [ "${available_kb:-0}" -gt 0 ]; then
        reserve_kb=$((6 * 1024 * 1024))
        if [ "${total_kb:-0}" -gt 0 ] && [ "$reserve_kb" -gt $((total_kb / 2)) ]; then
            reserve_kb=$((total_kb / 2))
        fi
        usable_kb=$((available_kb - reserve_kb))
        [ "$usable_kb" -lt 0 ] && usable_kb=0
        memory_jobs=$((usable_kb / (2 * 1024 * 1024)))
        [ "$memory_jobs" -lt 1 ] && memory_jobs=1
        [ "$jobs" -gt "$memory_jobs" ] && jobs="$memory_jobs"
    fi
    local built=""
    if [ "$os" = "linux" ]; then
        local baseline_out
        baseline_out="$(mktemp -d "${TMPDIR:-/tmp}/forkmesh-linux-baseline.XXXXXX")"
        echo "Building the Linux release in the Debian 12 baseline container (tools/package/linux-baseline.sh)."
        if ! ../tools/package/linux-baseline.sh build --source .. --out "$baseline_out" \
            --version "$release_version" --commit "$build_commit"; then
            echo "ERROR: the Debian 12 baseline build of the release binary failed." >&2
            return 1
        fi
        built="$baseline_out/$asset"
    else
        echo "Building the desktop release with -j$jobs."
        local configure_output
        if ! configure_output="$(cmake -S ../desktop -B ../desktop/build-release \
            -DCMAKE_BUILD_TYPE=Release -DFORKMESH_BUILD_TESTS=OFF \
            -DFORKMESH_BUILD_COMMIT_OVERRIDE="$build_commit" \
            -DFORKMESH_VERSION_OVERRIDE="$release_version" 2>&1)"; then
            printf '%s\n' "$configure_output" >&2
            echo "ERROR: failed to configure the release binary." >&2
            return 1
        fi
        printf '%s\n' "$configure_output" | grep -v "^--" || true
        if ! cmake --build ../desktop/build-release -j"$jobs" 2>&1 | tail -5; then
            echo "ERROR: failed to build release binary." >&2
            return 1
        fi
    fi
    local mirror_built="../server/forkmesh-mirror-node"
    [ "$os" = "windows" ] && mirror_built="${mirror_built}.exe"
    if ! (cd ../server && CGO_ENABLED=0 go build -trimpath \
        -ldflags='-s -w' -o "$(basename "$mirror_built")" \
        ./cmd/forkmesh-mirror-node); then
        echo "ERROR: failed to build the Go mirror-node companion." >&2
        return 1
    fi

    if [ -z "$built" ]; then
        for cand in \
            "../desktop/build-release/forkmesh" \
            "../desktop/build-release/forkmesh.exe" \
            "../desktop/build-release/ForkMesh.app/Contents/MacOS/ForkMesh"; do
            if [ -x "$cand" ]; then built="$cand"; break; fi
        done
    fi
    if [ -z "$built" ]; then
        echo "ERROR: build did not produce an executable." >&2
        return 1
    fi

    local reported_version reported_commit
    if ! reported_version="$("$built" --version 2>&1)" || \
       [ "$reported_version" != "ForkMesh ${release_version}" ]; then
        echo "ERROR: built release reports '${reported_version:-<no version>}' (expected 'ForkMesh ${release_version}')." >&2
        return 1
    fi
    if ! reported_commit="$("$built" --build-commit 2>&1)" || \
       [ "$reported_commit" != "$build_commit" ]; then
        echo "ERROR: built release reports source commit '${reported_commit:-<unknown>}' (expected '$build_commit')." >&2
        return 1
    fi

    cp "$built" "$asset"
    chmod 0755 "$asset" || true
    cp "$mirror_built" "$mirror_asset"
    chmod 0755 "$mirror_asset" || true
    local cas_dir="${FORKMESH_RELEASE_CAS:-../.forkmesh/release-blobs}"
    case "$cas_dir" in
        /*) ;;
        *) cas_dir="$PWD/$cas_dir" ;;
    esac
    mkdir -p "$cas_dir"
    local publish_output
    local publish_args=(
        --channel latest
        --tag "$release_tag"
        --tag-commit "$tag_commit"
        --build-commit "$build_commit"
        --cas-dir "$cas_dir"
    )
    if [ -n "${FORKMESH_REPO:-}" ]; then
        publish_args+=(--repo "$FORKMESH_REPO")
    fi
    publish_args+=("app/$asset")
    publish_args+=("app/$mirror_asset")
    if ! publish_output="$(cd .. && tools/forkmesh-release-publish.sh \
        "${publish_args[@]}" 2>&1)"; then
        rm -f "$asset" "$mirror_asset" "$mirror_built"
        echo "ERROR: failed to publish release binary." >&2
        return 1
    fi
    printf '%s\n' "$publish_output" | tail -3

    local asset_hash mirror_asset_hash
    if command -v sha256sum >/dev/null 2>&1; then
        asset_hash="$(sha256sum "$asset" | awk '{print $1}')"
        mirror_asset_hash="$(sha256sum "$mirror_asset" | awk '{print $1}')"
    else
        asset_hash="$(shasum -a 256 "$asset" | awk '{print $1}')"
        mirror_asset_hash="$(shasum -a 256 "$mirror_asset" | awk '{print $1}')"
    fi
    if ! grep -Fqx "$asset_hash  $asset" \
            ../.forkmesh/releases/latest/SHASUMS256.txt ||
       ! grep -Fq "\"tag\": \"$release_tag\"" \
            ../.forkmesh/releases/latest/release.json ||
       ! grep -Fq "\"tag_commit\": \"$tag_commit\"" \
            ../.forkmesh/releases/latest/release.json ||
       ! grep -Fq "\"build_commit\": \"$build_commit\"" \
            ../.forkmesh/releases/latest/release.json ||
       ! grep -Fq "\"name\":\"$asset\",\"blob_sha256\":\"$asset_hash\"" \
            ../.forkmesh/releases/latest/release.json ||
       ! grep -Fqx "$mirror_asset_hash  $mirror_asset" \
            ../.forkmesh/releases/latest/SHASUMS256.txt ||
       ! grep -Fq "\"name\":\"$mirror_asset\",\"blob_sha256\":\"$mirror_asset_hash\"" \
            ../.forkmesh/releases/latest/release.json ||
       [ ! -f ../.forkmesh/releases/latest/release.json.sig ] ||
       [ "$(wc -c < ../.forkmesh/releases/latest/release.json.sig | tr -d ' ')" != "64" ] ||
       [ ! -f "$cas_dir/sha256/${asset_hash:0:2}/$asset_hash/data" ]; then
        rm -f "$asset" "$mirror_asset" "$mirror_built"
        echo "ERROR: release metadata/CAS verification did not match the freshly built commit and binary." >&2
        return 1
    fi
    echo "Verified release metadata: v${release_version}, tag ${tag_commit:0:12}, build ${build_commit:0:12}, sha256:${asset_hash:0:12}."
    rm -f "$asset" "$mirror_asset" "$mirror_built"

    git add ../.forkmesh/releases/latest/SHASUMS256.txt \
        ../.forkmesh/releases/latest/release.json \
        ../.forkmesh/releases/latest/release.json.sig || return 1
}

commit_release_metadata() {
    if git diff --quiet --cached ../.forkmesh/releases/latest/ 2>/dev/null; then
        return 0
    fi
    echo "Committing release metadata…"
    git config user.email "deploy@forkmesh.local" >/dev/null 2>&1 || true
    git config user.name  "ForkMesh Deploy"        >/dev/null 2>&1 || true
    git commit -m "release: publish prebuilt binary for $(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)" \
        2>&1 | tail -2
    echo "Release metadata staged. Push this commit alongside the Worker deploy."
}

deploy_target_is_changed() {
    local target="$1"
    local current base headers app_marker="" legacy_edge_marker=""
    current="$(python3 tools/deploy_targets.py fingerprint "$target")" || return 1
    if command -v curl >/dev/null 2>&1; then
        case "$target" in
            app)
                base="${DEPLOY_VERIFY_URL:-https://app.forkmesh.com}"
                local control_base control_body="" backend_body="" identity_body=""
                local control_fingerprint="" control_rev="" control_worker=""
                local backend_fingerprint="" backend_rev="" backend_worker=""
                local backend_router_public_key="" backend_router_ready=""
                local router_public_key=""
                local control_takeover_ready=1 route_spec route_path expected_status
                local route_headers route_status route_marker route_legacy_marker
                control_base="$(app_control_verify_url)"
                control_body="$(_http_json_get "${control_base%/}/api/version")" || control_body=""
                backend_body="$(_http_json_get "${base%/}/api/mainnode")" || backend_body=""
                identity_body="$(_http_json_get "${control_base%/}/api/mirrors/https")" || identity_body=""
                if [ -n "$control_body" ]; then
                    control_fingerprint="$(printf '%s' "$control_body" | _json_string_field deployFingerprint 2>/dev/null || true)"
                    control_rev="$(printf '%s' "$control_body" | _json_string_field rev 2>/dev/null || true)"
                    control_worker="$(printf '%s' "$control_body" | _json_string_field worker 2>/dev/null || true)"
                fi
                if [ -n "$backend_body" ]; then
                    backend_fingerprint="$(printf '%s' "$backend_body" | _json_string_field deployFingerprint 2>/dev/null || true)"
                    backend_rev="$(printf '%s' "$backend_body" | _json_string_field rev 2>/dev/null || true)"
                    backend_worker="$(printf '%s' "$backend_body" | _json_string_field worker 2>/dev/null || true)"
                    backend_router_public_key="$(printf '%s' "$backend_body" | _json_string_field routerPublicKey 2>/dev/null || true)"
                    backend_router_ready="$(printf '%s' "$backend_body" | _json_boolean_field routerIdentityReady 2>/dev/null || true)"
                fi
                if [ -n "$identity_body" ]; then
                    router_public_key="$(printf '%s' "$identity_body" | _json_string_field routerPublicKey 2>/dev/null || true)"
                fi
                # A consolidation prepare run must always reach the migration
                # and D1-row readiness gates. Public routes may still be owned
                # by the legacy edge, so they cannot prove that preparation is
                # complete and must not cause this stage to be skipped.
                headers="$(curl -sSI --max-time 15 "${control_base%/}/api/version" 2>/dev/null || true)"
                app_marker="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-worker" { value=$2 } END { sub(/\r$/, "", value); print value }')"
                legacy_edge_marker="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-edge-control" { value=$2 } END { sub(/\r$/, "", value); print value }')"
                for route_spec in "/|308" "/health|200" "/api/mirrors/https|200"; do
                    route_path="${route_spec%|*}"
                    expected_status="${route_spec##*|}"
                    route_headers="$(curl -sS -D - -o /dev/null --max-time 15 \
                        "${control_base%/}${route_path}" 2>/dev/null || true)"
                    route_status="$(printf '%s\n' "$route_headers" | awk 'toupper($1) ~ /^HTTP\// { value=$2 } END { print value }')"
                    route_marker="$(printf '%s\n' "$route_headers" | awk -F': *' 'tolower($1) == "x-forkmesh-worker" { value=$2 } END { sub(/\r$/, "", value); print value }')"
                    route_legacy_marker="$(printf '%s\n' "$route_headers" | awk -F': *' 'tolower($1) == "x-forkmesh-edge-control" { value=$2 } END { sub(/\r$/, "", value); print value }')"
                    if [ "$route_status" != "$expected_status" ] || \
                       [ "$route_marker" != "app" ] || \
                       [ -n "$route_legacy_marker" ]; then
                        control_takeover_ready=0
                        break
                    fi
                done
                if [ "$control_fingerprint" = "$current" ] && \
                   [ "$backend_fingerprint" = "$current" ] && \
                   [ "$control_worker" = "app" ] && \
                   [ "$backend_worker" = "app" ] && \
                   [ -n "$control_rev" ] && [ "$control_rev" != "dev" ] && \
                   [ "$backend_rev" = "$control_rev" ] && \
                   [ "$backend_router_public_key" = "$EDGE_ROUTER_PUBLIC_KEY" ] && \
                   [ "$backend_router_ready" = "true" ] && \
                   [ "$app_marker" = "app" ] && \
                   [ -z "$legacy_edge_marker" ] && \
                   [ "$control_takeover_ready" = "1" ] && \
                   [ "$router_public_key" = "$EDGE_ROUTER_PUBLIC_KEY" ] && \
                   all_public_snapshot_routes_current "$control_base" "$current"; then
                    return 3
                fi
                return 0
                ;;
            world)
                base="${DEPLOY_VERIFY_WORLD_URL:-https://world.forkmesh.com}"
                ;;
            www)
                base="${DEPLOY_VERIFY_WWW_URL:-https://forkmesh.com}"
                ;;
        esac
        if [ "$target" != "app" ]; then
            if _static_fingerprint_check "$target" "$base" "$current" probe; then
                return 3
            fi
            return 0
        fi
    fi
    return 0
}

mark_deploy_target() {
    python3 tools/deploy_targets.py mark "$1" "${BUILD_REV:-}" >/dev/null
}

prepare_target_deploy() {
    local target="$1"
    require_cloudflare_account
    require_cloudflare_auth
    BUILD_REV="$(build_rev)"
    APP_VERSION="$(app_version)"
    DEPLOYED_AT_MS="$(date -u +%s)000"
    DEPLOY_TARGET_FINGERPRINT="$(python3 tools/deploy_targets.py fingerprint "$target")"
}

flagship_routing_refs_sha256() {
    local git_dir
    git_dir="$(git -C "$DEPLOY_DIR/.." rev-parse --absolute-git-dir)"
    PYTHONPATH="$DEPLOY_DIR/.." python3 - "$git_dir" <<'PY'
import re
import sys
from pathlib import Path

from tools.mirror_gateway import routing_refs_sha256

digest = routing_refs_sha256(Path(sys.argv[1])).strip().lower()
if not re.fullmatch(r"[0-9a-f]{64}", digest):
    raise SystemExit("could not derive the deployed flagship routing digest")
print(digest)
PY
}

deploy_static_target() {
    local target="$1"
    if [ "${FORKMESH_FORCE_DEPLOY:-0}" != "1" ]; then
        if deploy_target_is_changed "$target"; then
            :
        else
            local changed_status=$?
            if [ "$changed_status" = "3" ]; then
                echo "Skipping $target: its Worker inputs have not changed since the last successful deploy."
                return 0
            fi
            return "$changed_status"
        fi
    fi
    prepare_target_deploy "$target"
    if ! command -v npm >/dev/null 2>&1; then
        echo "ERROR: npm is required to deploy $target." >&2
        return 1
    fi
    echo "Deploying $target Worker (build $BUILD_REV)..."
    if [ "$target" = "world" ]; then
        signal_deploy_status deploying "$BUILD_REV"
        DEPLOY_SIGNAL_ACTIVE=1
        trap mark_interrupted_deploy EXIT
    fi
    local config=wrangler.toml
    [ "$target" = "www" ] && config=wrangler.production.toml
    (
        cd "../$target"
        deploy_upload_with_retry \
            npm exec --yes --package "${WRANGLER_NPM_SPEC:-wrangler@4.136.2}" -- \
            wrangler deploy --config "$config" \
            --var "DEPLOY_FINGERPRINT:${DEPLOY_TARGET_FINGERPRINT}"
    )
    if command -v curl >/dev/null 2>&1; then
        case "$target" in
            world)
                local base="${DEPLOY_VERIFY_WORLD_URL:-https://world.forkmesh.com}"
                _split_check "${base%/}/" world 200
                _status_check "${base%/}/world/world.js" 200
                ;;
            www)
                local base="${DEPLOY_VERIFY_WWW_URL:-https://forkmesh.com}"
                _split_check "${base%/}/" www 200
                # Pricing was folded into the homepage; the public route is a
                # permanent canonical redirect to its in-page section.
                _status_check "${base%/}/pricing" 308
                _status_check "${base%/}/blog" 200
                _status_check "${base%/}/docs/" 200
                ;;
        esac
        _static_fingerprint_check \
            "$target" "$base" "$DEPLOY_TARGET_FINGERPRINT"
    fi
    if [ "$target" = "world" ]; then
        signal_deploy_status ready "$BUILD_REV"
        DEPLOY_SIGNAL_ACTIVE=0
        trap - EXIT
    fi
    mark_deploy_target "$target"
    echo "Done: $target Worker is live."
}

# Queues must exist before a deploy that names one: wrangler refuses to publish
# a Worker whose producer/consumer points at a queue the account does not have,
# so this runs first and is idempotent — an "already exists" answer is the state
# we want, not a failure.
#
# Queues are a Workers Paid product. On an account without it the create fails
# and so would the deploy; the fallback is to drop the [[queues.*]] blocks from
# wrangler.toml, which leaves instant delivery working on the cron rail (see
# NOTIFICATION_QUEUE_BINDING in src/entry.py) rather than breaking it.
ensure_notification_queue() {
    local queue="${FORKMESH_NOTIFICATION_QUEUE:-forkmesh-notification-email}"
    if pywrangler queues list 2>/dev/null | grep -qF "$queue"; then
        return 0
    fi
    echo "Creating Cloudflare Queue $queue..."
    local output
    if output="$(pywrangler queues create "$queue" 2>&1)"; then
        return 0
    fi
    if printf '%s' "$output" | grep -qiE "already exists|already created"; then
        return 0
    fi
    echo "ERROR: could not create Cloudflare Queue $queue." >&2
    printf '%s\n' "$output" >&2
    return 1
}

# An upload can fail for reasons outside the project's control: Python
# Workers perform a local workerd validation whose Pyodide package host may not
# resolve (EAI_AGAIN / "Temporary failure in name resolution"), and Cloudflare's
# own API occasionally answers a read during the upload with a 5xx ("Received a
# malformed response from the API … GET …/secrets -> 503 Service Unavailable",
# run 8598 on 2026-09-13). Retry only those transport failures; validation,
# startup-memory, authentication, and other API errors remain immediate
# failures. Runs the upload command given as arguments.
_deploy_upload_is_transient() {
    grep -qiE \
        'pyodide-capnp-bin\.edgeworker\.net|DNS lookup failed|Temporary failure in name resolution|EAI_AGAIN|Received a malformed response from the API|upstream connect error|Service Unavailable|Bad Gateway|Gateway Time-?out|-> 50[234] |ECONNRESET|ETIMEDOUT|socket hang up|fetch failed' \
        "$1"
}

deploy_upload_with_retry() {
    local attempts="${FORKMESH_DEPLOY_UPLOAD_ATTEMPTS:-3}"
    local backoff="${FORKMESH_DEPLOY_UPLOAD_BACKOFF:-3}"
    local attempt=1
    local rc=1
    local output_file
    output_file="$(mktemp "./wrangler.deploy.XXXXXX.log")"
    while [ "$attempt" -le "$attempts" ]; do
        : >"$output_file"
        set +e
        "$@" 2>&1 | tee "$output_file"
        rc="${PIPESTATUS[0]}"
        set -e
        if [ "$rc" -eq 0 ]; then
            rm -f -- "$output_file"
            return 0
        fi
        if [ "$attempt" -ge "$attempts" ] || \
           ! _deploy_upload_is_transient "$output_file"; then
            rm -f -- "$output_file"
            return "$rc"
        fi
        echo "  upload attempt $attempt/$attempts: transient upload failure (Pyodide DNS or Cloudflare API 5xx); retrying in ${backoff}s..." >&2
        sleep "$backoff"
        attempt=$((attempt + 1))
    done
    rm -f -- "$output_file"
    return "$rc"
}

pywrangler_deploy_with_retry() {
    deploy_upload_with_retry pywrangler deploy "$@"
}

EXTRA_VAR_ARGS=()
app_extra_var_args() {
    EXTRA_VAR_ARGS=()
    local specs spec
    IFS=', ' read -r -a specs <<<"${FORKMESH_EXTRA_VARS:-}"
    for spec in ${specs[@]+"${specs[@]}"}; do
        [ -n "$spec" ] || continue
        if [[ ! "$spec" =~ ^[A-Z][A-Z0-9_]*:[A-Za-z0-9._-]*$ ]]; then
            echo "ERROR: FORKMESH_EXTRA_VARS entry '$spec' is not NAME:VALUE." >&2
            return 1
        fi
        EXTRA_VAR_ARGS+=(--var "$spec")
    done
}

deploy_app_version() {
    local worker_args=(--name "$APP_WORKER_NAME")
    ensure_notification_queue || return 1
    app_extra_var_args || return 1
    worker_args+=(${EXTRA_VAR_ARGS[@]+"${EXTRA_VAR_ARGS[@]}"})
    if [ "${FORKMESH_PRESERVE_LEGACY_HOSTS:-0}" != "1" ]; then
        pywrangler_deploy_with_retry --env "" "${worker_args[@]}" \
            --var "BUILD_REV:${BUILD_REV}" \
            --var "FLAGSHIP_ROUTING_REFS_SHA256:${FLAGSHIP_ROUTING_REFS_SHA256}" \
            --var "APP_VERSION:${APP_VERSION}" \
            --var "DEPLOYED_AT_MS:${DEPLOYED_AT_MS}" \
            --var "DEPLOY_FINGERPRINT:${DEPLOY_TARGET_FINGERPRINT}"
        return
    fi
    local cutover_config
    cutover_config="$(mktemp "./wrangler.cutover.XXXXXX.toml")"
    (
        trap 'rm -f -- "$cutover_config"' EXIT
        cp wrangler.toml "$cutover_config"
        sed -i 's/build_site_assets.py app/build_site_assets.py cutover/' "$cutover_config"
        printf '\n[[routes]]\npattern = "forkmesh.com"\ncustom_domain = true\n' >>"$cutover_config"
        printf '\n[[routes]]\npattern = "forkmesh.com"\ncustom_domain = true\n' >>"$cutover_config"
        pywrangler_deploy_with_retry --config "$cutover_config" --env "" \
            "${worker_args[@]}" \
            --var "BUILD_REV:${BUILD_REV}" \
            --var "FLAGSHIP_ROUTING_REFS_SHA256:${FLAGSHIP_ROUTING_REFS_SHA256}" \
            --var "APP_VERSION:${APP_VERSION}" \
            --var "DEPLOYED_AT_MS:${DEPLOYED_AT_MS}" \
            --var "DEPLOY_FINGERPRINT:${DEPLOY_TARGET_FINGERPRINT}"
    )
}

require_edge_router_public_key() {
    if [[ "$EDGE_ROUTER_PUBLIC_KEY" =~ ^[A-Za-z0-9_-]{43}$ ]] && \
       [[ "$EDGE_ROUTER_SIGNING_SEED" =~ ^[A-Za-z0-9_-]{43}$ ]] && \
       _router_key_pair_matches; then
        return 0
    fi
    echo "ERROR: the local mirror-router public key and signing seed must be a matched" >&2
    echo "       pair of 32-byte base64url Ed25519 values." >&2
    echo "       Restore both values in $ENV_FILE before deploying; no Worker was changed." >&2
    return 1
}

ROUTER_ROTATION_ACTIVE=0
authorize_router_rotation() {
    ROUTER_ROTATION_ACTIVE=0
    local live_key=""
    if ! live_key="$(live_router_public_key_with_retry "$(app_control_verify_url)")"; then
        if [ "${FORKMESH_FIRST_DEPLOY:-0}" = "1" ]; then
            ROUTER_ROTATION_ACTIVE=1
            echo "  first deployment: the App router identity will be established with its secret set"
            return 0
        fi
        if [ "${FORKMESH_ALLOW_ROUTER_KEY_ROTATION:-0}" = "1" ]; then
            ROUTER_ROTATION_ACTIVE=1
            echo "Mirror-router recovery authorized; the matched pair will be replaced atomically on the App."
            return 0
        fi
        echo "ERROR: the live mirror-router identity could not be read safely." >&2
        echo "       Refusing to publish secrets or routes without knowing the active public key." >&2
        echo "       Use FORKMESH_FIRST_DEPLOY=1 only when no App Worker exists yet." >&2
        return 1
    fi
    if [ "$live_key" = "$EDGE_ROUTER_PUBLIC_KEY" ]; then
        return 0
    fi
    if [ "${FORKMESH_ALLOW_ROUTER_KEY_ROTATION:-0}" != "1" ]; then
        echo "ERROR: the local mirror-router public key differs from the live identity." >&2
        echo "       Generic App deploys never rotate this trust root. Review the local pair," >&2
        echo "       then run './deploy.sh rotate-mirror-router' for an atomic pair replacement." >&2
        return 1
    fi
    ROUTER_ROTATION_ACTIVE=1
    echo "Mirror-router rotation authorized; the matched pair will be replaced atomically on the App."
}

app_control_verify_url() {
    if [ -n "${DEPLOY_VERIFY_APP_URL:-}" ]; then
        printf '%s' "${DEPLOY_VERIFY_APP_URL%/}"
    elif [ -n "${DEPLOY_VERIFY_EDGE_URL:-}" ]; then
        # Compatibility for automation driving the one-time two-stage cutover.
        printf '%s' "${DEPLOY_VERIFY_EDGE_URL%/}"
    elif official_multi_host_enabled; then
        printf '%s' "https://app.forkmesh.com"
    else
        printf '%s' "${DEPLOY_VERIFY_URL:-https://app.forkmesh.com}"
    fi
}

public_snapshot_routes_current() {
    local base="${1:-$(app_control_verify_url)}"
    local fingerprint="${2:-$DEPLOY_TARGET_FINGERPRINT}"
    local path="${3:-/api/status}"
    local headers marker legacy_marker snapshot snapshot_fingerprint status
    headers="$(curl -sS -D - -o /dev/null --max-time 25 \
        "${base%/}${path}" 2>/dev/null || true)"
    status="$(printf '%s\n' "$headers" | awk 'toupper($1) ~ /^HTTP\// { value=$2 } END { print value }')"
    marker="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-worker" { value=$2 } END { sub(/\r$/, "", value); print value }')"
    legacy_marker="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-edge-control" { value=$2 } END { sub(/\r$/, "", value); print value }')"
    snapshot="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-public-snapshot" { value=$2 } END { sub(/\r$/, "", value); print value }')"
    snapshot_fingerprint="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-snapshot-fingerprint" { value=$2 } END { sub(/\r$/, "", value); print value }')"
    # A new instance legitimately has no forkmesh/forkmesh repository yet.
    # Its mirror endpoint is deliberately a no-store live 404 instead of an
    # invented snapshot row; require the same safe state that bootstrap's
    # readiness check accepts. Every other public route still needs the exact
    # fresh snapshot generated by this deploy.
    if [ "$path" = "/api/repo/forkmesh/forkmesh/mirrors" ] && \
       [ "$status" = "404" ] && [ "$marker" = "app" ] && \
       [ -z "$legacy_marker" ] && [ "$snapshot" = "missing-fallback" ]; then
        return 0
    fi
    [ "$status" = "200" ] && [ "$marker" = "app" ] && \
        [ -z "$legacy_marker" ] && [ "$snapshot" = "fresh" ] && \
        [ "$snapshot_fingerprint" = "$fingerprint" ]
}

all_public_snapshot_routes_current() {
    local base="${1:-$(app_control_verify_url)}"
    local fingerprint="${2:-$DEPLOY_TARGET_FINGERPRINT}"
    local path
    for path in "/api/repositories" "/api/network/overview" \
        "/api/repo/forkmesh/forkmesh/mirrors" "/api/status" \
        "/api/status/world"; do
        public_snapshot_routes_current "$base" "$fingerprint" "$path" || return 1
    done
}

wait_for_public_snapshot_rows() {
    local attempts="${FORKMESH_SNAPSHOT_READY_ATTEMPTS:-90}"
    local sleep_s="${FORKMESH_SNAPSHOT_READY_BACKOFF:-6}"
    local database="${FORKMESH_D1_NAME:-forkmesh}"
    local attempt result required_ready mirror_ready mirror_active query
    if [[ ! "$DEPLOY_TARGET_FINGERPRINT" =~ ^[0-9a-f]{64}$ ]]; then
        echo "ERROR: public snapshot readiness requires a canonical deploy fingerprint." >&2
        return 1
    fi
    # The four general public projections are required. The flagship mirror
    # projection is optional until that repository exists: its absence is a
    # deliberately safe live 404 (see public_snapshot_routes_current), while
    # an existing current row must still belong to this deploy.
    query="SELECT SUM(CASE WHEN path IN ('/api/repositories','/api/network/overview','/api/status','/api/status/world') AND kind='forkmesh-public-api-snapshot' AND deploy_fingerprint='${DEPLOY_TARGET_FINGERPRINT}' AND fresh_until>(CAST(strftime('%s','now') AS INTEGER)*1000+15000) THEN 1 ELSE 0 END) AS required_ready, SUM(CASE WHEN path='/api/repo/forkmesh/forkmesh/mirrors' AND kind='forkmesh-public-api-snapshot' AND deploy_fingerprint='${DEPLOY_TARGET_FINGERPRINT}' AND fresh_until>(CAST(strftime('%s','now') AS INTEGER)*1000+15000) THEN 1 ELSE 0 END) AS mirror_ready, SUM(CASE WHEN path='/api/repo/forkmesh/forkmesh/mirrors' AND kind='forkmesh-public-api-snapshot' AND fresh_until>(CAST(strftime('%s','now') AS INTEGER)*1000+15000) THEN 1 ELSE 0 END) AS mirror_active FROM public_api_snapshots"
    for attempt in $(seq 1 "$attempts"); do
        result="$(pywrangler d1 execute "$database" --remote --json \
            --command "$query" 2>/dev/null || true)"
        read -r required_ready mirror_ready mirror_active < <(printf '%s' "$result" | python3 -c '
import json,sys
try:
    raw=sys.stdin.read()
    value=None
    lines=raw.splitlines()
    for index,line in enumerate(lines):
        stripped=line.lstrip()
        if not stripped.startswith(("[","{")):
            continue
        candidate="\n".join([stripped,*lines[index+1:]])
        try:
            value,_=json.JSONDecoder().raw_decode(candidate)
            break
        except json.JSONDecodeError:
            continue
    if value is None:
        raise ValueError("missing Wrangler JSON payload")
    blocks=value if isinstance(value,list) else [value]
    rows=[]
    for block in blocks:
        if isinstance(block,dict): rows.extend(block.get("results") or [])
    values = [
        (int(row.get("required_ready", 0) or 0),
         int(row.get("mirror_ready", 0) or 0),
         int(row.get("mirror_active", 0) or 0))
        for row in rows if isinstance(row, dict)
    ]
    print(*max(values, default=(0, 0, 0)))
except Exception:
    print(0, 0, 0)
' 2>/dev/null || printf '0 0 0')
        if [ "$required_ready" = "4" ] && \
           { [ "$mirror_ready" = "1" ] || [ "$mirror_active" = "0" ]; }; then
            return 0
        fi
        if [ "$attempt" -lt "$attempts" ]; then
            echo "  attempt $attempt/$attempts: waiting for four required public snapshots and the optional flagship mirror state; retrying in ${sleep_s}s..." >&2
            sleep "$sleep_s"
        fi
    done
    echo "ERROR: the new App did not publish four required public snapshots and a safe flagship mirror state after $attempts attempts." >&2
    return 1
}

require_legacy_public_snapshot_rows_expired() {
    local database="${FORKMESH_LEGACY_PUBLIC_SNAPSHOTS_D1_NAME:-forkmesh-public-snapshots}"
    local query result active
    query="SELECT COUNT(*) AS active FROM public_api_snapshots WHERE kind='forkmesh-public-api-snapshot' AND fresh_until>generated_at"
    result="$(pywrangler d1 execute "$database" --remote --json \
        --command "$query" 2>/dev/null || true)"
    active="$(printf '%s' "$result" | python3 -c '
import json,sys
try:
    raw=sys.stdin.read(); value=None; lines=raw.splitlines()
    for index,line in enumerate(lines):
        stripped=line.lstrip()
        if not stripped.startswith(("[","{")): continue
        try:
            value,_=json.JSONDecoder().raw_decode("\n".join([stripped,*lines[index+1:]])); break
        except json.JSONDecodeError: continue
    blocks=value if isinstance(value,list) else [value]
    rows=[]
    for block in blocks:
        if isinstance(block,dict): rows.extend(block.get("results") or [])
    print(max((int(row.get("active",-1)) for row in rows if isinstance(row,dict)),default=-1))
except Exception: print(-1)
' 2>/dev/null || printf '%s' '-1')"
    if [ "$active" != "0" ]; then
        echo "ERROR: legacy snapshot D1 is not proven pre-expired (active=${active:-unknown})." >&2
        echo "       Quiesce the legacy edge cron and expire its normal rows before uploading the consolidated App." >&2
        return 1
    fi
}

expire_legacy_public_snapshot_rows() {
    # During the prepare window the legacy exact routes still read their old
    # dedicated D1 binding, while the consolidated App publishes only to the
    # main DB. Expire every normal legacy row immediately after uploading the
    # new App. The old edge then takes its existing no-store live-fallback path
    # and cannot disclose a row after a subsequent privacy mutation. Active
    # tombstones remain untouched and therefore continue to fail closed.
    local database="${FORKMESH_LEGACY_PUBLIC_SNAPSHOTS_D1_NAME:-forkmesh-public-snapshots}"
    local sql
    sql="UPDATE public_api_snapshots SET fresh_until=generated_at, stale_until=generated_at WHERE kind='forkmesh-public-api-snapshot'"
    local attempts="${FORKMESH_LEGACY_SNAPSHOT_EXPIRE_ATTEMPTS:-3}"
    local sleep_s="${FORKMESH_LEGACY_SNAPSHOT_EXPIRE_BACKOFF:-2}" attempt
    echo "Reasserting expiry of all normal rows in legacy snapshot D1 '$database' before the route cutover..."
    for attempt in $(seq 1 "$attempts"); do
        if pywrangler d1 execute "$database" --remote --command "$sql" && \
           require_legacy_public_snapshot_rows_expired; then
            return 0
        fi
        [ "$attempt" -lt "$attempts" ] && sleep "$sleep_s"
    done
    echo "ERROR: failed to re-expire legacy snapshot rows after $attempts attempts." >&2
    return 1
}

verify_public_snapshot_routes() {
    local base="${1:-$(app_control_verify_url)}"
    local attempts="${FORKMESH_DEPLOY_VERIFY_ATTEMPTS:-30}"
    local sleep_s="${FORKMESH_DEPLOY_VERIFY_BACKOFF:-6}"
    local attempt path ok
    for attempt in $(seq 1 "$attempts"); do
        ok=1
        for path in "/api/repositories" "/api/network/overview" \
            "/api/repo/forkmesh/forkmesh/mirrors" "/api/status" \
            "/api/status/world"; do
            if ! public_snapshot_routes_current "$base" \
                "$DEPLOY_TARGET_FINGERPRINT" "$path"; then
                ok=0
                break
            fi
        done
        if [ "$ok" = "1" ]; then
            local bypass_headers auth_headers bypass_status auth_status
            local bypass_snapshot auth_snapshot bypass_fingerprint auth_fingerprint
            bypass_headers="$(curl -sS -D - -o /dev/null --max-time 25 \
                "${base%/}/api/status?fresh=deploy-verification" 2>/dev/null || true)"
            auth_headers="$(curl -sS -D - -o /dev/null --max-time 25 \
                -H 'Authorization: Bearer deployment-verification' \
                "${base%/}/api/status" 2>/dev/null || true)"
            bypass_status="$(printf '%s\n' "$bypass_headers" | awk 'toupper($1) ~ /^HTTP\// { value=$2 } END { print value }')"
            auth_status="$(printf '%s\n' "$auth_headers" | awk 'toupper($1) ~ /^HTTP\// { value=$2 } END { print value }')"
            bypass_snapshot="$(printf '%s\n' "$bypass_headers" | awk -F': *' 'tolower($1) == "x-forkmesh-public-snapshot" { value=$2 } END { sub(/\r$/, "", value); print value }')"
            auth_snapshot="$(printf '%s\n' "$auth_headers" | awk -F': *' 'tolower($1) == "x-forkmesh-public-snapshot" { value=$2 } END { sub(/\r$/, "", value); print value }')"
            bypass_fingerprint="$(printf '%s\n' "$bypass_headers" | awk -F': *' 'tolower($1) == "x-forkmesh-snapshot-fingerprint" { value=$2 } END { sub(/\r$/, "", value); print value }')"
            auth_fingerprint="$(printf '%s\n' "$auth_headers" | awk -F': *' 'tolower($1) == "x-forkmesh-snapshot-fingerprint" { value=$2 } END { sub(/\r$/, "", value); print value }')"
            if [ "$bypass_status" = "200" ] && [ "$auth_status" = "200" ] && \
               [ "$bypass_snapshot" = "fresh" ] && \
               [ "$auth_snapshot" = "fresh" ] && \
               [ "$bypass_fingerprint" = "$DEPLOY_TARGET_FINGERPRINT" ] && \
               [ "$auth_fingerprint" = "$DEPLOY_TARGET_FINGERPRINT" ]; then
                return 0
            fi
        fi
        if [ "$attempt" -lt "$attempts" ]; then
            echo "  attempt $attempt/$attempts: waiting for exact public snapshot routes; retrying in ${sleep_s}s..." >&2
            sleep "$sleep_s"
        fi
    done
    echo "ERROR: the exact public snapshot routes did not become current and fresh." >&2
    return 1
}

verify_app_control() {
    if ! command -v curl >/dev/null 2>&1; then
        echo "ERROR: curl is required to verify consolidated App route ownership." >&2
        return 1
    fi
    local base
    base="$(app_control_verify_url)"
    local attempts="${FORKMESH_DEPLOY_VERIFY_ATTEMPTS:-30}"
    local sleep_s="${FORKMESH_DEPLOY_VERIFY_BACKOFF:-6}"
    local attempt headers marker="" legacy_marker="" status="" route_ok
    local route_spec route_path expected_status
    for attempt in $(seq 1 "$attempts"); do
        route_ok=1
        for route_spec in "/|308" "/health|200" "/api/version|200" "/api/mirrors/https|200"; do
            route_path="${route_spec%|*}"
            expected_status="${route_spec##*|}"
            headers="$(curl -sS -D - -o /dev/null --max-time 25 \
                "${base%/}${route_path}" 2>/dev/null || true)"
            status="$(printf '%s\n' "$headers" | awk 'toupper($1) ~ /^HTTP\// { value=$2 } END { print value }')"
            marker="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-worker" { value=$2 } END { sub(/\r$/, "", value); print value }')"
            legacy_marker="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-edge-control" { value=$2 } END { sub(/\r$/, "", value); print value }')"
            if [ "$status" != "$expected_status" ] || \
               [ "$marker" != "app" ] || [ -n "$legacy_marker" ]; then
                route_ok=0
                break
            fi
        done
        if [ "$route_ok" = "1" ]; then
            break
        fi
        if [ "$attempt" -lt "$attempts" ]; then
            echo "  attempt $attempt/$attempts: waiting for exclusive App ownership of root, health, version, and mirror identity; retrying in ${sleep_s}s..." >&2
            sleep "$sleep_s"
        fi
    done
    if [ "$route_ok" != "1" ]; then
        echo "ERROR: ${base%/} control routes are not exclusively owned by the consolidated App after $attempts attempts." >&2
        return 1
    fi
    verify_live_router_public_key "$EDGE_ROUTER_PUBLIC_KEY" "$base"
}

# Final smoke check of the live public surface: the App role, this deploy's
# fingerprint, and the /api landing page. Every other verification in this
# script retries, and this one must too. Ordinary app.forkmesh.com requests
# are funnelled through one ingress Durable Object, and the free tier aborts
# those mid-request from time to time; the Worker answers such an abort with a
# deliberately marked, retryable 503 (log_durable_object_abort /
# EXPECTED_DEGRADED_HEADER in app/src/entry.py). Probing once turned a single
# abort into a failed deploy of an origin that was already serving the new
# build and answered 200 moments later.
verify_live_app_surface() {
    local base="${1:-${DEPLOY_VERIFY_URL:-https://app.forkmesh.com}}"
    base="${base%/}"
    local attempts="${FORKMESH_DEPLOY_VERIFY_ATTEMPTS:-30}"
    local sleep_s="${FORKMESH_DEPLOY_VERIFY_BACKOFF:-6}"
    local attempt reason response version version_status landing_status
    reason="$base/api/version could not be reached"
    for attempt in $(seq 1 "$attempts"); do
        reason=""
        response="$(curl -sS --max-time 25 -w $'\n%{http_code}' \
            "$base/api/version" 2>/dev/null || true)"
        version_status="${response##*$'\n'}"
        version="${response%$'\n'*}"
        if [ "$version_status" != "200" ]; then
            reason="$base/api/version returned HTTP ${version_status:-<none>}"
        elif ! grep -Eq '"worker"[[:space:]]*:[[:space:]]*"app"' <<<"$version"; then
            reason="$base is not reporting the app worker role"
        elif ! grep -Eq '"deployFingerprint"[[:space:]]*:[[:space:]]*"'"$DEPLOY_TARGET_FINGERPRINT"'"' <<<"$version"; then
            reason="$base is not reporting the deployed App fingerprint"
        else
            landing_status="$(curl -sS -o /dev/null -w '%{http_code}' \
                --max-time 25 "$base/api" 2>/dev/null || true)"
            if [ "$landing_status" != "200" ]; then
                reason="$base/api returned HTTP ${landing_status:-<none>}"
            fi
        fi
        if [ -z "$reason" ]; then
            echo "Verified the live App surface: worker role, deployed fingerprint, and /api landing page."
            return 0
        fi
        if [ "$attempt" -lt "$attempts" ]; then
            echo "  attempt $attempt/$attempts: $reason; retrying in ${sleep_s}s..." >&2
            sleep "$sleep_s"
        fi
    done
    echo "ERROR: $reason after $attempts attempts." >&2
    echo "       The upload itself already verified; this is the live origin staying" >&2
    echo "       unhealthy. Check the Worker error log and the Durable Object abort" >&2
    echo "       rate on /status before redeploying." >&2
    return 1
}

deploy_app_target() {
    require_edge_router_public_key
    if [ "${FORKMESH_FORCE_DEPLOY:-0}" != "1" ] && \
       [ "$APP_CONSOLIDATION_PREPARE" != "1" ]; then
        if deploy_target_is_changed app; then
            :
        else
            local changed_status=$?
            if [ "$changed_status" = "3" ]; then
                echo "Skipping app: its Worker inputs have not changed since the last successful deploy."
                return 0
            fi
            return "$changed_status"
        fi
    fi
    if [ "$APP_CONSOLIDATION_PREPARE" = "1" ]; then
        require_legacy_public_snapshot_rows_expired
    fi
    prepare_target_deploy app
    FLAGSHIP_ROUTING_REFS_SHA256="$(flagship_routing_refs_sha256)"
    authorize_router_rotation
    push_secrets validate
    if signal_deploy_status deploying "$BUILD_REV"; then
        DEPLOY_SIGNAL_ACTIVE=1
        trap mark_interrupted_deploy EXIT
    else
        echo "note: deploy semaphore is not available before migrations; retrying after schema setup." >&2
    fi
    ./migrate.sh
    # Publish request controls before Workers and static assets so rejected or
    # over-limit traffic never enters the Python runtime.
    ./sync_request_block_rules.sh
    ./sync_request_rate_limit.sh
    if [ "$DEPLOY_SIGNAL_ACTIVE" != "1" ]; then
        if ! signal_deploy_status deploying "$BUILD_REV"; then
            echo "ERROR: deploy semaphore is unavailable after migrations." >&2
            return 1
        fi
        DEPLOY_SIGNAL_ACTIVE=1
        trap mark_interrupted_deploy EXIT
    fi
    echo "Deploying $APP_WORKER_NAME as the App, API, Git, cron, and Durable Object owner (build $BUILD_REV)..."
    deploy_app_version
    if [ "$APP_CONSOLIDATION_PREPARE" = "1" ]; then
        expire_legacy_public_snapshot_rows
    fi
    if [ "$ROUTER_ROTATION_ACTIVE" = "1" ]; then
        echo "Publishing the matched mirror-router pair in one App secret update..."
        push_secrets
    else
        verify_live_router_public_key "$EDGE_ROUTER_PUBLIC_KEY" \
            "$(app_control_verify_url)"
        push_secrets publish-preserve-router
    fi
    verify_deploy "$BUILD_REV" "/api/mainnode" \
        "${DEPLOY_VERIFY_URL:-https://app.forkmesh.com}" \
        "$DEPLOY_TARGET_FINGERPRINT" "$EDGE_ROUTER_PUBLIC_KEY"
    # The App Cron Trigger publishes the rows. During the one-time consolidation
    # prepare stage, old edge routes still win the public URLs, so readiness is
    # established directly from the main D1 database before those external
    # routes are removed. Normal deploys additionally verify every live route.
    if [ "${FORKMESH_RECOVER_UNAVAILABLE_HTTP:-0}" = "1" ]; then
        echo "Outage recovery: live HTTP fallbacks are healthy; deferring public snapshot refresh until Durable Object scheduling recovers."
    else
        wait_for_public_snapshot_rows
    fi
    if [ "$APP_CONSOLIDATION_PREPARE" = "1" ]; then
        echo "App consolidation prepare stage is ready."
        echo "The App and five current snapshot rows are deployed; the legacy edge routes were not changed."
        echo "Remove the external edge-control routes/script in the authorized cutover, then run './deploy.sh post-deploy-verify'."
    else
        verify_deploy "$BUILD_REV" "/api/version" "$(app_control_verify_url)" \
            "$DEPLOY_TARGET_FINGERPRINT"
        verify_app_control
        if [ "${FORKMESH_RECOVER_UNAVAILABLE_HTTP:-0}" != "1" ]; then
            verify_public_snapshot_routes "$(app_control_verify_url)"
        fi
    fi
    if [ "$APP_CONSOLIDATION_PREPARE" != "1" ] && \
       command -v curl >/dev/null 2>&1; then
        verify_live_app_surface "${DEPLOY_VERIFY_URL:-https://app.forkmesh.com}"
    fi
    signal_deploy_status ready "$BUILD_REV"
    DEPLOY_SIGNAL_ACTIVE=0
    trap - EXIT
    mark_deploy_target app
    echo "Done: $APP_WORKER_NAME owns the App, API, Git, cron, and Durable Objects."
}

initial_official_cutover_needed() {
    official_multi_host_enabled || return 1
    command -v curl >/dev/null 2>&1 || return 0
    local marker
    marker="$(curl -sSI --max-time 20 https://forkmesh.com/ 2>/dev/null | \
        awk -F': *' 'tolower($1) == "x-forkmesh-worker" { value=$2 } END { sub(/\r$/, "", value); print value }')"
    [ "$marker" != "www" ]
}

publish_preserving_router_identity() {
    if ! command -v curl >/dev/null 2>&1; then
        echo "ERROR: curl is required to preserve and verify the live App router identity." >&2
        return 1
    fi
    local base headers marker preserved_key
    base="$(app_control_verify_url)"
    headers="$(curl -sSI --max-time 25 "${base%/}/api/version" 2>/dev/null || true)"
    marker="$(printf '%s\n' "$headers" | awk -F': *' 'tolower($1) == "x-forkmesh-worker" { value=$2 } END { sub(/\r$/, "", value); print value }')"
    if [ "$marker" != "app" ]; then
        echo "ERROR: the consolidated App route is not active; generic secret publication is unsafe." >&2
        echo "       Deploy the complete App before updating non-router secrets." >&2
        return 1
    fi
    if ! preserved_key="$(live_router_public_key "$base")"; then
        echo "ERROR: the live mirror-router public identity could not be preserved safely." >&2
        return 1
    fi
    push_secrets publish-preserve-router
    verify_live_router_public_key "$preserved_key" "$base"
}

deploy_changed_workers() {
    if ! official_multi_host_enabled; then
        "$DEPLOY_SELF" app
        if [ "${FORKMESH_DEPLOY_WORLD:-0}" = "1" ]; then
            "$DEPLOY_SELF" world
        fi
        return
    fi
    if initial_official_cutover_needed; then
        DEPLOY_VERIFY_URL="${DEPLOY_VERIFY_URL:-https://forkmesh.com}" \
            FORKMESH_FORCE_DEPLOY=1 FORKMESH_PRESERVE_LEGACY_HOSTS=1 "$DEPLOY_SELF" app
        "$DEPLOY_SELF" world
        "$DEPLOY_SELF" www
        FORKMESH_FORCE_DEPLOY=1 "$DEPLOY_SELF" app
    else
        "$DEPLOY_SELF" app
        "$DEPLOY_SELF" world
        "$DEPLOY_SELF" www
    fi
}

case "$1" in
    deploy)
        deploy_changed_workers
        echo "Done: app, world, and www were checked in migration-safe cutover order."
        ;;
    changed)
        deploy_changed_workers
        if [ "${FORKMESH_FORCE_DEPLOY:-0}" = "1" ]; then
            echo "Done: --force deployed every Worker without checking for changes."
        else
            echo "Done: every changed Worker has been deployed; unchanged Workers were skipped."
        fi
        ;;
    app)
        deploy_app_target
        ;;
    rotate-mirror-router)
        FORKMESH_ALLOW_ROUTER_KEY_ROTATION=1 FORKMESH_FORCE_DEPLOY=1 "$0" app
        ;;
    world|www)
        deploy_static_target "$1"
        ;;
    status)
        shift
        python3 tools/deploy_targets.py status "$@"
        ;;
    post-deploy-verify)
        python3 tools/verify_split_deployment.py
        ;;
    republish-release-binary)
        source_rev="$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
        if publish_release_binary 1; then
            commit_release_metadata
            echo "Release binary republished from source commit $source_rev."
            echo "Push the release metadata commit, then refresh mirror catalogs before using the fleet install button."
        else
            exit 1
        fi
        ;;
    secrets)
        require_cloudflare_account
        require_cloudflare_auth
        publish_preserving_router_identity
        ;;
    validate-secrets)
        require_cloudflare_account
        require_cloudflare_auth
        require_edge_router_public_key
        push_secrets validate
        ;;
    dev)
        build_dashboard_assets
        pywrangler dev --env "" ${VAR_ARGS[@]+"${VAR_ARGS[@]}"}
        ;;
    dry-run)
        target="${2:-app}"
        case "$target" in
            app)
                build_dashboard_assets
                python3 tools/build_site_assets.py app
                pywrangler deploy --env "" --name "$APP_WORKER_NAME" --dry-run
                ;;
            world|www)
                config=wrangler.toml
                [ "$target" = "www" ] && config=wrangler.production.toml
                (
                    cd "../$target"
                    npm exec --yes --package "${WRANGLER_NPM_SPEC:-wrangler@4.136.2}" -- \
                        wrangler deploy --config "$config" --dry-run
                )
                ;;
            *)
                echo "Unknown dry-run target: $target" >&2
                exit 2
                ;;
        esac
        ;;
    *)
        echo "Usage: $0 [deploy|changed|app|rotate-mirror-router|world|www|status|post-deploy-verify|republish-release-binary|secrets|validate-secrets|dev|dry-run [target]] [--force]" >&2
        echo "       --force deploys the selected Workers even when no input changes were detected." >&2
        exit 2
        ;;
esac
