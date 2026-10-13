#!/usr/bin/env bash
# Fediverse (ActivityPub) go-live helper for the ForkMesh relay
#
# "Connecting to the fediverse" needs no registration anywhere: once the
# Worker with the /ap/* layer is deployed, being reachable at the well-known
# discovery endpoints on your domain IS joining the network. This script
# proves that from the outside, exactly the way a Mastodon server would.
#
#   ./federate.sh                    verify https://forkmesh.com (default)
#   ./federate.sh verify [origin]    run every discovery/protocol check
#   ./federate.sh verify --user alice --repo owner/widget
#                                    check specific actors (otherwise a public
#                                    repo + its owner are picked from the
#                                    live catalog automatically)
#   ./federate.sh comments owner/repo issue 7 [origin]
#                                    show a thread's federated comments
#   ./federate.sh deploy             ./deploy.sh then verify (needs
#                                    .env.production; on machines without it,
#                                    push to main and let CI deploy instead)
#
# Exit code = number of failed checks, so it slots straight into CI.
set -u
cd "$(dirname "$0")"

ORIGIN="https://forkmesh.com"
USER_HANDLE=""
REPO_PATH=""
FAILS=0
CHECKS=0
BODY=""
HTTP_STATUS="000"

say()  { printf '%s\n' "$*"; }
pass() { CHECKS=$((CHECKS + 1)); say "  PASS  $1"; }
fail() { CHECKS=$((CHECKS + 1)); FAILS=$((FAILS + 1)); say "  FAIL  $1${2:+ — $2}"; }

# fetch <url> [accept] -> sets $BODY and $HTTP_STATUS (globals, NOT a command
# substitution: a subshell would silently drop the status).
fetch() {
    local url="$1" accept="${2:-}" tmp
    tmp="$(mktemp)"
    if [ -n "$accept" ]; then
        HTTP_STATUS="$(curl -sS -o "$tmp" -w '%{http_code}' -H "Accept: $accept" "$url" 2>/dev/null || echo 000)"
    else
        HTTP_STATUS="$(curl -sS -o "$tmp" -w '%{http_code}' "$url" 2>/dev/null || echo 000)"
    fi
    BODY="$(cat "$tmp")"
    rm -f "$tmp"
}

# jget <python-expr over d> -> value from $BODY or empty (body via stdin so
# arbitrary response bodies can't break quoting)
jget() {
    printf '%s' "$BODY" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
    v = eval(sys.argv[1])
    print("" if v is None else v)
except Exception:
    pass' "$1" 2>/dev/null
}

pick_sample() {
    # Auto-pick a public repo (and a federating user) from the live catalog so
    # zero-argument verification just works.
    fetch "$ORIGIN/api/repositories"
    if [ -z "$REPO_PATH" ]; then
        REPO_PATH="$(jget 'd["repositories"][0]["owner"] + "/" + d["repositories"][0]["name"]')"
    fi
    if [ -z "$USER_HANDLE" ]; then
        # Only USER-kind accounts get Person actors (node/mirror accounts
        # correctly 404), so probe catalog owners until one webfingers.
        local domain="${ORIGIN#*://}" owner
        for owner in $(jget '" ".join(dict.fromkeys(r["owner"] for r in d["repositories"][:25]))'); do
            fetch "$ORIGIN/.well-known/webfinger?resource=acct:$owner@$domain"
            if [ "$HTTP_STATUS" = 200 ]; then USER_HANDLE="$owner"; break; fi
        done
    fi
}

verify() {
    local domain="${ORIGIN#*://}"
    say "Verifying fediverse surface of $ORIGIN"

    # 0. Origin sanity: the Worker answers at all.
    fetch "$ORIGIN/api/version"
    if [ "$HTTP_STATUS" = 200 ]; then pass "/api/version (rev $(jget 'd["rev"][:12]'))"
    else fail "/api/version" "HTTP $HTTP_STATUS — is the Worker deployed?"; return; fi

    pick_sample
    [ -n "$REPO_PATH" ] || say "  note: no public repos in catalog; repo-actor checks will be skipped"
    [ -n "$USER_HANDLE" ] || say "  note: no user-kind account found among catalog owners; user-actor checks skipped (node accounts correctly have no Person actor)"
    local owner="${REPO_PATH%%/*}" repo="${REPO_PATH#*/}"

    # 1. NodeInfo discovery chain.
    fetch "$ORIGIN/.well-known/nodeinfo"
    case "$(jget 'd["links"][0]["href"]')" in
        "$ORIGIN"/nodeinfo/2.1) pass "/.well-known/nodeinfo";;
        *) fail "/.well-known/nodeinfo" "HTTP $HTTP_STATUS";;
    esac
    fetch "$ORIGIN/nodeinfo/2.1"
    if [ "$(jget 'd["software"]["name"]')" = "forkmesh" ] \
       && [ "$(jget 'd["protocols"][0]')" = "activitypub" ]; then
        pass "/nodeinfo/2.1 (activitypub advertised)"
    else fail "/nodeinfo/2.1" "HTTP $HTTP_STATUS"; fi

    # 2. WebFinger + actor documents for the user and repo actors.
    if [ -n "$USER_HANDLE" ]; then
        fetch "$ORIGIN/.well-known/webfinger?resource=acct:$USER_HANDLE@$domain"
        if [ "$(jget 'd["subject"]')" = "acct:$USER_HANDLE@$domain" ]; then
            pass "webfinger @$USER_HANDLE@$domain"
        else fail "webfinger @$USER_HANDLE@$domain" "HTTP $HTTP_STATUS"; fi

        fetch "$ORIGIN/ap/users/$USER_HANDLE" application/activity+json
        if [ "$(jget 'd["preferredUsername"]')" = "$USER_HANDLE" ] \
           && [ -n "$(jget 'd["publicKey"]["publicKeyPem"]')" ]; then
            pass "actor /ap/users/$USER_HANDLE (RSA key published)"
        else fail "actor /ap/users/$USER_HANDLE" "HTTP $HTTP_STATUS"; fi

        # Content negotiation on the human profile URL.
        fetch "$ORIGIN/@$USER_HANDLE" application/activity+json
        if [ "$(jget 'd["id"]')" = "$ORIGIN/ap/users/$USER_HANDLE" ]; then
            pass "content negotiation on /@$USER_HANDLE"
        else fail "content negotiation on /@$USER_HANDLE" "HTTP $HTTP_STATUS"; fi
    fi

    if [ -n "$REPO_PATH" ]; then
        fetch "$ORIGIN/.well-known/webfinger?resource=acct:$owner.$repo@$domain"
        if [ "$(jget 'd["subject"]')" = "acct:$owner.$repo@$domain" ]; then
            pass "webfinger @$owner.$repo@$domain"
        else fail "webfinger @$owner.$repo@$domain" "HTTP $HTTP_STATUS"; fi

        fetch "$ORIGIN/ap/repos/$owner/$repo" application/activity+json
        if [ "$(jget 'd["type"]')" = "Group" ] \
           && [ -n "$(jget 'd["publicKey"]["publicKeyPem"]')" ]; then
            pass "actor /ap/repos/$owner/$repo (Group, RSA key published)"
        else fail "actor /ap/repos/$owner/$repo" "HTTP $HTTP_STATUS"; fi

        fetch "$ORIGIN/ap/repos/$owner/$repo/followers" application/activity+json
        if [ "$(jget 'd["type"]')" = "OrderedCollection" ]; then
            pass "followers collection ($(jget 'd["totalItems"]') followers)"
        else fail "followers collection" "HTTP $HTTP_STATUS"; fi

        fetch "$ORIGIN/api/repo/$owner/$repo/fedi-comments?kind=issue&number=1"
        if [ "$(jget 'd["ok"]')" = "True" ]; then
            pass "fedi-comments API"
        else fail "fedi-comments API" "HTTP $HTTP_STATUS"; fi
    fi

    # 3. Inbox exists and ENFORCES HTTP signatures (unsigned POST must 401).
    HTTP_STATUS="$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
        -H 'Content-Type: application/activity+json' \
        -d '{"type":"Follow","actor":"https://example.com/u/x"}' \
        "$ORIGIN/ap/inbox" 2>/dev/null || echo 000)"
    if [ "$HTTP_STATUS" = 401 ]; then pass "shared inbox rejects unsigned POSTs (401)"
    else fail "shared inbox signature enforcement" "expected 401, got $HTTP_STATUS"; fi

    say ""
    if [ "$FAILS" = 0 ]; then
        say "All $CHECKS checks passed — $domain is live on the fediverse."
        [ -n "$REPO_PATH" ] && say "From any Mastodon account, search:  @$owner.$repo@$domain   and follow it."
    else
        say "$FAILS of $CHECKS checks FAILED."
    fi
}

cmd="${1:-verify}"
case "$cmd" in
    verify|deploy) shift || true;;
    comments)
        shift
        rp="${1:?usage: federate.sh comments owner/repo kind number [origin]}"
        kind="${2:?kind (issue|pull|discussion|commit|release)}"
        num="${3:?number/ref}"
        ORIGIN="${4:-$ORIGIN}"
        fetch "$ORIGIN/api/repo/${rp%%/*}/${rp#*/}/fedi-comments?kind=$kind&number=$num"
        printf '%s\n' "$BODY"
        exit 0;;
    -*|http*) cmd=verify;;   # bare origin or flags → verify
    *) say "unknown command: $cmd"; exit 2;;
esac
while [ $# -gt 0 ]; do
    case "$1" in
        --user) USER_HANDLE="$2"; shift 2;;
        --repo) REPO_PATH="$2"; shift 2;;
        http*) ORIGIN="${1%/}"; shift;;
        *) say "unknown arg: $1"; exit 2;;
    esac
done

if [ "$cmd" = deploy ]; then
    ./deploy.sh || exit $?
fi
verify
exit "$FAILS"
