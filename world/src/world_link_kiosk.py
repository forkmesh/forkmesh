"""Consented World lobby links with explainable reach estimates.

The kiosk never fetches a submitted URL. It combines an authenticated
ForkMesh account's server-side follower count with aggregate visits already
recorded for the submitted hostname. The result estimates potential traffic;
it is not a ranking of a person and has no effect on access, merges, or
governance. The separate tiny SOL appreciation estimate is optional,
non-custodial, and never a promise of payment.
"""

import math
import re
from urllib.parse import parse_qsl, urlencode, urlsplit, urlunsplit


PREFIX = "/api/world/link-kiosk"
BODY_MAX_BYTES = 8 * 1024
MAX_LINKS = 1000
MAX_LINKS_PER_ACCOUNT = 20
PUBLIC_LIMIT = 25
MAX_URL = 1200
MAX_TITLE = 120
CHANNELS = frozenset({"article", "community", "social", "video", "other"})
CHANNEL_RATES = {
    "article": (0.03, 0.12),
    "community": (0.05, 0.18),
    "social": (0.04, 0.16),
    "video": (0.025, 0.10),
    "other": (0.02, 0.08),
}
SENSITIVE_QUERY_KEYS = frozenset({
    "access_token", "api_key", "apikey", "auth", "authorization", "code",
    "credential", "email", "key", "password", "secret", "session",
    "sessionid", "sig", "signature", "token",
})
HOST_RE = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$"
)
SOLANA_ADDRESS_RE = re.compile(r"^[1-9A-HJ-NP-Za-km-z]{32,44}$")
# A deliberately tiny, deterministic SOL-denominated appreciation estimate.
# Scores map across ten steps; the UI publishes only SOL and never presents
# this as a guaranteed payout or a balance.
REWARD_SOL_MIN = 0.00005
REWARD_SOL_MAX = 0.00050


def reward_sol(score):
    step = max(
        1,
        min(10, 1 + round(max(0, min(100, int(score or 0))) * 9 / 100)),
    )
    value = REWARD_SOL_MIN + (step - 1) * (
        (REWARD_SOL_MAX - REWARD_SOL_MIN) / 9
    )
    return float(f"{value:.8f}")


def _response(runtime, payload, status=200):
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers={"x-content-type-options": "nosniff"},
    )


def _text(value, maximum, fallback=""):
    clean = "".join(
        character if character.isprintable() and character not in "<>" else " "
        for character in str(value or "")
    )
    clean = " ".join(clean.split()).strip()
    return (clean or fallback)[:maximum]


def normalize_public_url(value):
    """Return a safe HTTPS URL and hostname, without doing any network I/O."""
    raw = str(value or "").strip()
    if not raw or len(raw.encode("utf-8")) > MAX_URL * 2:
        return "", ""
    try:
        parsed = urlsplit(raw)
        host = str(parsed.hostname or "").strip().lower().rstrip(".")
        if (
            parsed.scheme != "https"
            or not host
            or not HOST_RE.fullmatch(host)
            or parsed.username
            or parsed.password
            or host.endswith((".local", ".internal"))
            or host in {"localhost", "forkmesh.internal"}
            or host.replace(".", "").isdigit()
            or parsed.port not in (None, 443)
        ):
            return "", ""
        query = urlencode([
            (
                str(key)[:120],
                "[redacted]"
                if str(key).lower() in SENSITIVE_QUERY_KEYS
                else str(item)[:300],
            )
            for key, item in parse_qsl(
                parsed.query, keep_blank_values=True, max_num_fields=40
            )
        ], doseq=True)
        normalized = urlunsplit((
            "https", host, parsed.path or "/", query, "",
        ))[:MAX_URL]
        return normalized, host
    except Exception:
        return "", ""


def reach_estimate(followers, observed_visits, channel, verified_domain=False):
    """Return a bounded deterministic score and potential-traffic range."""
    followers = max(0, min(int(followers or 0), 1_000_000_000))
    observed_visits = max(
        0, min(int(observed_visits or 0), 1_000_000_000)
    )
    channel = channel if channel in CHANNELS else "other"
    follower_points = min(
        60, int(round(15 * math.log10(followers + 1)))
    )
    traffic_points = min(
        30, int(round(10 * math.log10(observed_visits + 1)))
    )
    verification_points = 10 if verified_domain else 0
    score = min(100, follower_points + traffic_points + verification_points)
    low_rate, high_rate = CHANNEL_RATES[channel]
    observed_signal = int(round(math.sqrt(observed_visits)))
    low = max(0, int(round(followers * low_rate)) + observed_signal)
    high = max(
        low,
        int(round(followers * high_rate)) + observed_signal * 3,
    )
    return {
        "score": score,
        "potentialTraffic": {"low": low, "high": high},
        "factors": {
            "followerPoints": follower_points,
            "trafficPoints": traffic_points,
            "verifiedDomainPoints": verification_points,
            "followers": followers,
            "observedAggregateVisits": observed_visits,
            "channel": channel,
        },
    }


def _verified_profile_host(record, host):
    links = record.get("profile_links") if isinstance(record, dict) else []
    if not isinstance(links, list):
        return False
    for link in links[:20]:
        if not isinstance(link, dict) or not bool(link.get("verified")):
            continue
        try:
            link_host = str(
                urlsplit(str(link.get("url") or "")).hostname or ""
            ).strip().lower().rstrip(".")
        except Exception:
            link_host = ""
        if link_host == host:
            return True
    return False


async def _public_links(runtime):
    rows = await runtime.d1_all(
        "SELECT l.link_id,l.account_bi,l.data,l.score,l.potential_low,"
        "l.potential_high,l.created_at,u.data AS account_data "
        "FROM world_lobby_links l "
        "LEFT JOIN users u ON u.user_bi=l.account_bi "
        "ORDER BY l.created_at DESC,l.link_id DESC LIMIT ?",
        PUBLIC_LIMIT,
    )
    links = []
    accounts = {}
    for row in rows or []:
        try:
            data = await runtime.open(row.get("data"))
        except Exception:
            data = None
        if not isinstance(data, dict):
            continue
        account_key = str(row.get("account_bi") or "")
        if account_key not in accounts:
            try:
                accounts[account_key] = await runtime.open(
                    row.get("account_data")
                )
            except Exception:
                accounts[account_key] = None
        account = accounts.get(account_key)
        url, host = normalize_public_url(data.get("url"))
        if not url:
            continue
        wallet = str((account or {}).get("solana") or "").strip()
        if (
            (account or {}).get("status") != "active"
            or not SOLANA_ADDRESS_RE.fullmatch(wallet)
        ):
            wallet = ""
        paid_at = max(0, int(data.get("paidAt") or 0))
        score = max(0, min(100, int(row.get("score") or 0)))
        links.append({
            "id": str(row.get("link_id") or ""),
            "url": url,
            "host": host,
            "title": _text(data.get("title"), MAX_TITLE, host),
            "submittedBy": _text(data.get("account"), 64, "member"),
            "channel": (
                str(data.get("channel") or "other")
                if str(data.get("channel") or "") in CHANNELS else "other"
            ),
            "score": score,
            "potentialTraffic": {
                "low": max(0, int(row.get("potential_low") or 0)),
                "high": max(0, int(row.get("potential_high") or 0)),
            },
            "walletAddress": wallet,
            "rewardSol": reward_sol(score),
            "paymentStatus": "paid" if paid_at else "unpaid",
            "paidAt": paid_at,
            "createdAt": int(row.get("created_at") or 0),
        })
    return links


async def handle(runtime, path):
    if str(path or "").rstrip("/") != PREFIX:
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    if method not in ("GET", "POST"):
        return _response(runtime, {"error": "method_not_allowed"}, status=405)
    await runtime.ensure_schema()
    if method == "GET":
        return _response(runtime, {
            "ok": True,
            "links": await _public_links(runtime),
            "policy": {
                "label": "Estimated reach score",
                "minimum": 0,
                "maximum": 100,
                "notUsedFor": [
                    "merge access", "governance", "guaranteed payouts"
                ],
            },
        })

    if not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, status=403)
    data, error = await runtime.json_body(BODY_MAX_BYTES)
    if error:
        return _response(
            runtime,
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
        )
    account_bi, record = await runtime.session(data)
    actor = _text((record or {}).get("name"), 64).lower()
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, status=401)
    if data.get("consent") is not True:
        return _response(runtime, {"error": "consent_required"}, status=400)
    url, host = normalize_public_url(data.get("url"))
    title = _text(data.get("title"), MAX_TITLE, host)
    channel = str(data.get("channel") or "other").strip().lower()
    if not url:
        return _response(runtime, {"error": "invalid_public_url"}, status=400)
    if channel not in CHANNELS:
        return _response(runtime, {"error": "invalid_channel"}, status=400)
    account_count = await runtime.d1_first(
        "SELECT COUNT(*) AS count FROM world_lobby_links WHERE account_bi=?",
        str(account_bi),
    )
    if int((account_count or {}).get("count") or 0) >= MAX_LINKS_PER_ACCOUNT:
        return _response(runtime, {"error": "account_link_limit"}, status=429)
    total = await runtime.d1_first(
        "SELECT COUNT(*) AS count FROM world_lobby_links"
    )
    if int((total or {}).get("count") or 0) >= MAX_LINKS:
        await runtime.d1_run(
            "DELETE FROM world_lobby_links WHERE link_id IN ("
            "SELECT link_id FROM world_lobby_links "
            "ORDER BY created_at ASC,link_id ASC LIMIT 25)"
        )
    follower_row = await runtime.d1_first(
        "SELECT COUNT(*) AS count FROM profile_follows WHERE target_bi=?",
        str(account_bi),
    )
    traffic_row = await runtime.d1_first(
        "SELECT visits FROM site_referrers WHERE host=?",
        host,
    )
    estimate = reach_estimate(
        int((follower_row or {}).get("count") or 0),
        int((traffic_row or {}).get("visits") or 0),
        channel,
        _verified_profile_host(record or {}, host),
    )
    link_id = str(runtime.new_id())
    url_bi = await runtime.blind("world-link:" + url)
    now = int(runtime.now())
    sealed = await runtime.seal({
        "url": url,
        "title": title,
        "account": actor,
        "channel": channel,
    })
    try:
        await runtime.d1_run(
            "INSERT INTO world_lobby_links "
            "(link_id,account_bi,url_bi,data,score,potential_low,"
            "potential_high,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?)",
            link_id, str(account_bi), url_bi, sealed, estimate["score"],
            estimate["potentialTraffic"]["low"],
            estimate["potentialTraffic"]["high"], now, now,
        )
    except Exception:
        return _response(runtime, {"error": "link_already_submitted"}, status=409)
    await runtime.audit(
        actor,
        "world.link_kiosk_submit",
        "world_lobby_link",
        link_id,
        details={
            "score": estimate["score"],
            "channel": channel,
            "verifiedDomain": bool(
                estimate["factors"]["verifiedDomainPoints"]
            ),
        },
    )
    return _response(runtime, {
        "ok": True,
        "submission": {
            "id": link_id,
            "url": url,
            "title": title,
            "score": estimate["score"],
            "potentialTraffic": estimate["potentialTraffic"],
            "factors": estimate["factors"],
            "walletAddress": (
                str((record or {}).get("solana") or "").strip()
                if SOLANA_ADDRESS_RE.fullmatch(
                    str((record or {}).get("solana") or "").strip())
                else ""
            ),
            "rewardSol": reward_sol(estimate["score"]),
            "paymentStatus": "unpaid",
            "createdAt": now,
        },
        "links": await _public_links(runtime),
    }, status=201)
