"""Repository webhooks: outbound ``forkmesh-webhook-v1`` delivery.

ForkMesh already specifies the outbound webhook wire format — canonical JSON
body, HMAC-SHA256 signature over
``forkmesh-webhook-v1\\n<ts>\\n<id>\\n<body>``,
the five ``ForkMesh-*`` headers and the retry/dead-letter rules (see
``www/docs/integrations.md`` and ``tools/forkmesh_integration.py``). Until now
the only sender was that client-side tool, which polls the public catalog: a CI
provider could not be told "build when this repository moves" without somebody
running a watcher.

This module is the relay-side sender. A repository owner registers a URL
and the events it wants; the four event points that already exist server-side —

* ``repository.push``    an owner-signed publication whose refs digest moved
* ``branch.push``        the same publication, when the default branch moved
* ``pull.merged``        a merge job that reached ``succeeded`` and published
* ``release.published``  the desktop's release publish hook

— queue one row per subscribed hook and drain it with the documented retry
policy. Deliveries are queued, never inline: an unreachable third-party host
must not add its timeout to the request that triggered the event.

Boundaries this module keeps:
- The URL and the signing secret are private. They live inside the encrypted
  ``data`` blob, never in an operator-visible column, and the secret is
  returned to the owner exactly once (at creation or rotation).
- Only public, routable https destinations. A webhook is a server-side fetch
  the owner controls, so the loopback/link-local/private ranges and internal
  names are refused outright rather than attempted (SSRF).
- Private repositories do not federate their activity to third parties here:
  the event points call in only for repositories that are public.
- The Workers adapter in ``entry.py`` owns network and D1 primitives, supplied
  through ``_bind_runtime``. Everything above that line is pure Python so the
  policy, the signature and the retry classification can be tested without a
  Cloudflare runtime.
"""

from __future__ import annotations

import hashlib
import hmac
import ipaddress
import json
import re
from urllib.parse import urlparse


WEBHOOK_SCHEME = "forkmesh-webhook-v1"
EVENT_SCHEMA = "forkmesh-event-v1"
USER_AGENT = "forkmesh-relay/1.0 (+https://forkmesh.com/docs/integrations)"

# Mirrors tools/forkmesh_integration.py: a receiver rejects a delivery whose
# timestamp is further from now than this. The timestamp is taken fresh on
# every attempt, so a row that waited hours in the outbox still arrives inside
# the window; the constant is here so the pin against the published contract
# fails if either side moves.
TOLERANCE_SECONDS = 300
RETRY_STATUSES = frozenset({408, 425, 429, 500, 502, 503, 504})
MAX_ATTEMPTS = 6
BASE_DELAY_MS = 30 * 1000
MAX_DELAY_MS = 30 * 60 * 1000

MAX_WEBHOOKS_PER_REPO = 20
MAX_URL_LENGTH = 600
MAX_DESCRIPTION_LENGTH = 120
MAX_BODY_BYTES = 64 * 1024
SECRET_BYTES = 32
DELIVERY_HISTORY_PER_HOOK = 20
# Deliveries attempted inline right after an enqueue. The cron batch size is
# entry.py's REPO_WEBHOOK_CRON_DELIVERIES: it is a per-tick subrequest budget
# shared with the other cron phases, so entry owns it.
OUTBOX_DRAIN_IMMEDIATE = 4
DELIVERY_TIMEOUT_SECONDS = 12

# The event vocabulary an owner may subscribe to, in display order. The keys
# are the ``ForkMesh-Event-Type`` header values; the labels are what the
# dashboard check-boxes read, and they are deliberately the names CI providers
# use for the same triggers ("Repository push", "Branch push", "PR merge").
EVENT_CATALOG = (
    ("repository.push", "Repository push",
     "Any ref (branch or tag) on the repository moved."),
    ("branch.push", "Branch push",
     "The repository's default branch tip moved."),
    ("pull.merged", "PR merge",
     "A pull request was merged and the merge was published."),
    ("release.published", "Release published",
     "A signed release manifest was published for a tag."),
)
EVENT_TYPES = tuple(key for key, _label, _hint in EVENT_CATALOG)

# Hosts that must never be a webhook destination even though they resolve
# publicly: a delivery aimed back at ForkMesh would let an owner replay signed
# relay traffic onto the relay's own authenticated surfaces.
_BLOCKED_HOST_SUFFIXES = (
    # All of forkmesh.com: this deployment's own names (forkmesh.com and
    # below) and production's (app.forkmesh.com, the mirrors). ForkMesh Next
    # holds imported production webhooks and must never deliver into
    # production (audit 2026-09-24 M6).
    "forkmesh.com",
    "www.forkmesh.com",
    "localhost",
    ".local",
    ".internal",
    ".localdomain",
    ".test",
    ".invalid",
    ".example",
    ".onion",
)


# --- pure policy -------------------------------------------------------------

def clean_url(value):
    """Return a bounded, public https webhook destination, or ``""``.

    Unlike a browser-facing media URL a webhook destination may be an IP
    literal — plenty of CI runners are addressed that way — but only a globally
    routable one. Credentials, fragments, non-https schemes, non-global
    addresses and ForkMesh's own names are refused.
    """
    raw = str(value or "").strip()
    if not raw or len(raw) > MAX_URL_LENGTH or "\\" in raw:
        return ""
    if any(ch in raw for ch in ("\n", "\r", "\t", " ")):
        return ""
    try:
        parsed = urlparse(raw)
        host = str(parsed.hostname or "").lower().rstrip(".")
        port = parsed.port
    except (TypeError, ValueError):
        return ""
    if (
        parsed.scheme.lower() != "https"
        or not host
        or parsed.username is not None
        or parsed.password is not None
        or parsed.fragment
    ):
        return ""
    # A bare entry matches that exact name and anything under it; a dotted
    # entry is a suffix. Neither may match a merely similar-looking name, so
    # "notforkmesh.com" stays a legitimate destination.
    if any(
        host == suffix or host.endswith("." + suffix)
        if not suffix.startswith(".")
        else host.endswith(suffix)
        for suffix in _BLOCKED_HOST_SUFFIXES
    ):
        return ""
    try:
        if not ipaddress.ip_address(host).is_global:
            return ""
    except ValueError:
        # A name, not a literal. Require a dotted public-looking name; DNS
        # rebinding onto a private address is out of scope for a destination
        # the repository owner chose and can see in their own delivery log.
        if "." not in host:
            return ""
    netloc = "[" + host + "]" if ":" in host else host
    if port not in (None, 443):
        if port is None or not 1 <= int(port) <= 65535:
            return ""
        netloc += ":" + str(port)
    return parsed._replace(
        scheme="https", netloc=netloc, fragment="").geturl()


def redact_url(value):
    """Scheme, host and a trimmed path — safe for a log line or an error."""
    try:
        parsed = urlparse(str(value or ""))
    except (TypeError, ValueError):
        return ""
    host = str(parsed.hostname or "")
    if not host:
        return ""
    path = (parsed.path or "")[:40]
    return parsed.scheme + "://" + host + path


def normalize_events(values):
    """Subscribed event types, deduplicated and in catalog order."""
    if isinstance(values, str):
        values = values.split()
    chosen = {str(value or "").strip().lower() for value in (values or [])}
    return tuple(key for key in EVENT_TYPES if key in chosen)


def clean_description(value):
    text = re.sub(r"\s+", " ", str(value or "")).strip()
    return text[:MAX_DESCRIPTION_LENGTH]


def canonical_body(event):
    """The exact bytes that are signed and sent (sorted keys, no spaces)."""
    return json.dumps(event, sort_keys=True, separators=(",", ":")).encode(
        "utf-8")


def event_identity(subject, event_type, digest):
    """Deterministic idempotency key, as ``make_event`` builds one.

    A receiver deduplicates on ``ForkMesh-Webhook-Id``, so two deliveries for
    the same repository state must carry the same id: a catalog publication
    replayed by a retrying node is one event, not two builds.
    """
    return hashlib.sha256(
        ("%s\n%s\n%s" % (subject, event_type, digest)).encode("utf-8")
    ).hexdigest()[:32]


def build_event(owner, repo, event_type, digest, data, occurred_at, sequence,
                origin="https://forkmesh.com"):
    subject = str(owner or "").lower() + "/" + str(repo or "")
    return {
        "schema": EVENT_SCHEMA,
        "id": event_identity(subject, event_type, digest),
        "type": event_type,
        "subject": subject,
        # Per-subject ordering, increasing by exactly one per observed change,
        # so a receiver can detect a gap (contract policies.ordering).
        "sequence": max(1, int(sequence or 1)),
        "digest": str(digest or ""),
        "occurredAt": int(occurred_at or 0),
        "source": origin,
        "repository": {
            "owner": str(owner or "").lower(),
            "name": str(repo or ""),
            "fullName": subject,
            "url": origin.rstrip("/") + "/" + str(owner or "").lower()
                   + "/" + str(repo or ""),
            "cloneUrl": origin.rstrip("/") + "/api/repo/"
                        + str(owner or "").lower() + "/" + str(repo or "")
                        + "/git",
        },
        "data": data if isinstance(data, dict) else {},
    }


def signing_message(event_id, timestamp, body):
    return ("%s\n%d\n%s\n" % (
        WEBHOOK_SCHEME, int(timestamp), str(event_id))).encode("utf-8") + body


def sign_body(secret, event_id, timestamp, body):
    """Reference signature. The Worker uses WebCrypto; both must agree."""
    key = secret.encode("utf-8") if isinstance(secret, str) else bytes(secret)
    return "v1=" + hmac.new(
        key, signing_message(event_id, timestamp, body),
        hashlib.sha256).hexdigest()


def delivery_headers(signature, event, attempt, timestamp):
    return {
        "content-type": "application/json",
        "user-agent": USER_AGENT,
        "ForkMesh-Webhook-Id": str(event.get("id") or ""),
        "ForkMesh-Webhook-Timestamp": str(int(timestamp)),
        "ForkMesh-Webhook-Signature": signature,
        "ForkMesh-Webhook-Attempt": str(int(attempt)),
        "ForkMesh-Event-Type": str(event.get("type") or ""),
    }


def classify(status):
    """``"delivered"``, ``"retry"`` or ``"dropped"`` for a response status.

    ``0`` stands for "no response arrived" (DNS, TLS, timeout), which the
    documented policy retries. ``410 Gone`` means the receiver has retired the
    endpoint, so the event stops here rather than burning the attempt budget.
    A 3xx is "dropped" too: deliveries are sent with ``redirect: "manual"``
    because following a redirect would replay a signed body at a host the
    owner never registered, and re-POSTing it at the new location would defeat
    the point of signing it for the one they did.
    """
    code = int(status or 0)
    if 200 <= code < 300:
        return "delivered"
    if code == 0 or code in RETRY_STATUSES:
        return "retry"
    return "dropped"


def backoff_ms(attempt):
    """Deterministic capped exponential backoff for the next attempt."""
    step = max(1, int(attempt or 1))
    return min(MAX_DELAY_MS, BASE_DELAY_MS * (2 ** (step - 1)))


def public_view(row, record, secret=""):
    """The shape the dashboard renders. Never carries a stored secret."""
    view = {
        "id": str(row.get("webhook_id") or ""),
        "url": str((record or {}).get("url") or ""),
        "events": list(normalize_events((record or {}).get("events"))),
        "description": str((record or {}).get("description") or ""),
        "active": int(row.get("active") or 0) == 1,
        "managed": bool(str(row.get("seed_key") or "")),
        "createdAt": int(row.get("created_at") or 0),
        "updatedAt": int(row.get("updated_at") or 0),
        "lastStatus": int(row.get("last_status") or 0),
        "lastDeliveryAt": int(row.get("last_delivery_at") or 0),
        "consecutiveFailures": int(row.get("failures") or 0),
    }
    if secret:
        view["secret"] = secret
    return view


# Webhooks ForkMesh itself declares for a repository it operates. They are
# materialized into ``repo_webhooks`` once (tracked by ``seed_key``), so the
# owner sees, edits and can delete them like any other hook — a deleted seed is
# not resurrected. Codemagic builds the ForkMesh mobile apps and, unlike the
# hosted providers it supports natively, has to be told about a push by a
# webhook carrying the three trigger kinds it watches; a release is the fourth
# because that is the point the mobile artifacts have to be rebuilt for.
SEEDED_WEBHOOKS = (
    {
        "key": "codemagic-2026-09",
        "owner": "forkmesh",
        "repo": "forkmesh",
        "url": "https://api.codemagic.io/hooks/6aac6e1e7b35578e30ad5a76",
        "events": ("repository.push", "branch.push", "pull.merged",
                   "release.published"),
        "description": "Codemagic CI (mobile builds)",
    },
)


def seeds_for(owner, repo):
    owner = str(owner or "").strip().lower()
    repo = str(repo or "").strip().lower()
    return tuple(
        seed for seed in SEEDED_WEBHOOKS
        if seed["owner"] == owner and seed["repo"].lower() == repo
    )


# --- Worker runtime ----------------------------------------------------------

def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _now():
    return int(Date.now())


def _random_hex(length):
    raw = js_crypto.getRandomValues(Uint8Array.new(length))
    return bytes(raw.to_py()).hex()


async def _repo_bi(env, owner, repo):
    return await blind_index(env, str(owner).lower() + "/" + str(repo))


async def _hmac_signature(secret, event_id, timestamp, body):
    key = await js_crypto.subtle.importKey(
        "raw", Uint8Array.new(_to_js(secret.encode("utf-8"))),
        to_js({"name": "HMAC", "hash": "SHA-256"}), False, _to_js(["sign"]))
    signature = await js_crypto.subtle.sign(
        "HMAC", key,
        Uint8Array.new(_to_js(bytes(signing_message(
            event_id, timestamp, body)))))
    return "v1=" + bytes(Uint8Array.new(signature).to_py()).hex()


async def _next_sequence(env, repo_bi, webhook_id):
    """Allocate the next event sequence for one endpoint's stream.

    The contract orders events per subject by a sequence that increases by
    exactly one per observed change, and a consumer treats a gap as lost
    delivery. An endpoint only ever sees the event types it subscribed to, so
    the counter has to be per endpoint: a per-repository counter would make
    every event another endpoint received look like a gap to this one, and the
    documented ledger would sit waiting for it and then ask for a resync.
    """
    try:
        await d1_run(
            env,
            "INSERT INTO repo_webhook_sequence (webhook_id, repo_bi, seq)"
            " VALUES (?,?,1) ON CONFLICT(webhook_id) DO UPDATE SET seq=seq+1",
            webhook_id, repo_bi)
        row = await d1_first(
            env, "SELECT seq FROM repo_webhook_sequence WHERE webhook_id=?",
            webhook_id)
        return max(1, int((row or {}).get("seq") or 1))
    except Exception:
        return 1


def _webhook_origin(env, request=None):
    base = (getattr(env, "PUBLIC_BASE_URL", "") or "").strip().rstrip("/")
    if base.startswith("https://"):
        return base
    if request is not None:
        try:
            host = urlparse(request.url).netloc.split(":", 1)[0].lower()
        except Exception:
            host = ""
        if host and host not in (
                "app.forkmesh.com", "forkmesh.com", "world.forkmesh.com"):
            return "https://" + host
    return "https://forkmesh.com"


async def _load_webhooks(env, repo_bi):
    """Every hook row for a repository, newest first, with its record."""
    rows = await d1_all(
        env,
        "SELECT webhook_id, repo_bi, active, seed_key, created_at, updated_at,"
        " last_status, last_delivery_at, failures, data FROM repo_webhooks"
        " WHERE repo_bi=? ORDER BY created_at DESC LIMIT ?",
        repo_bi, MAX_WEBHOOKS_PER_REPO + 1)
    loaded = []
    for row in rows or []:
        record = await decrypt_row(env, row.get("data"))
        if isinstance(record, dict) and record.get("url"):
            loaded.append((row, record))
    return loaded


async def _apply_seeds(env, owner, repo, repo_bi):
    """Materialize any ForkMesh-declared hook for this repository, once."""
    seeds = seeds_for(owner, repo)
    if not seeds:
        return
    for seed in seeds:
        try:
            applied = await d1_first(
                env,
                "SELECT seed_key FROM repo_webhook_seeds"
                " WHERE repo_bi=? AND seed_key=?",
                repo_bi, seed["key"])
            if applied:
                continue
            url = clean_url(seed["url"])
            if not url:
                continue
            now = _now()
            data = await encrypt_row(env, {
                "url": url,
                "secret": _random_hex(SECRET_BYTES),
                "events": list(normalize_events(seed["events"])),
                "description": clean_description(seed["description"]),
                "createdBy": "forkmesh",
            })
            await d1_run(
                env,
                "INSERT OR IGNORE INTO repo_webhooks (webhook_id, repo_bi,"
                " active, seed_key, created_at, updated_at, data)"
                " VALUES (?,?,1,?,?,?,?)",
                _random_hex(16), repo_bi, seed["key"], now, now, data)
            await d1_run(
                env,
                "INSERT OR IGNORE INTO repo_webhook_seeds"
                " (repo_bi, seed_key, applied_at) VALUES (?,?,?)",
                repo_bi, seed["key"], now)
        except Exception:
            # A seed is convenience, never a precondition: a failure here must
            # not stop the owner's own hooks from listing or delivering.
            continue


async def _record_delivery(env, repo_bi, webhook_id, event_type, status,
                           attempts, outcome, now):
    try:
        await d1_run(
            env,
            "INSERT INTO repo_webhook_deliveries (delivery_id, webhook_id,"
            " repo_bi, event_type, status, attempts, outcome, ts)"
            " VALUES (?,?,?,?,?,?,?,?)",
            _random_hex(12), webhook_id, repo_bi, event_type, int(status),
            int(attempts), outcome, now)
        await d1_run(
            env,
            "DELETE FROM repo_webhook_deliveries WHERE webhook_id=? AND"
            " delivery_id NOT IN (SELECT delivery_id FROM"
            " repo_webhook_deliveries WHERE webhook_id=? ORDER BY ts DESC"
            " LIMIT ?)",
            webhook_id, webhook_id, DELIVERY_HISTORY_PER_HOOK)
    except Exception:
        pass


async def _send(url, secret, event, attempt):
    """One delivery attempt. Returns the HTTP status, or 0 for no response."""
    body = canonical_body(event)
    if len(body) > MAX_BODY_BYTES:
        return 0
    # Taken fresh on every attempt, so a row that waited hours in the outbox
    # still arrives inside the receiver's TOLERANCE_SECONDS window.
    timestamp = _now() // 1000
    signature = await _hmac_signature(
        secret, str(event.get("id") or ""), timestamp, body)
    try:
        response = await js_fetch_with_timeout(
            url,
            {
                "method": "POST",
                "headers": delivery_headers(
                    signature, event, attempt, timestamp),
                "body": body.decode("utf-8"),
                "redirect": "manual",
            },
            DELIVERY_TIMEOUT_SECONDS)
    except Exception:
        return 0
    if response is None:
        return 0
    return int(getattr(response, "status", 0) or 0)


async def _attempt_row(env, row, now):
    """Deliver one outbox row and settle it. Returns the outcome."""
    record = await decrypt_row(env, row.get("data"))
    webhook_id = str(row.get("webhook_id") or "")
    repo_bi = str(row.get("repo_bi") or "")
    attempts = int(row.get("attempts") or 0) + 1
    if not isinstance(record, dict) or not record.get("url"):
        await d1_run(
            env, "DELETE FROM repo_webhook_outbox WHERE id=?", row.get("id"))
        return "dropped"
    event = record.get("event")
    if not isinstance(event, dict):
        event = {}
    event_type = str(event.get("type") or "")
    status = await _send(
        record["url"], str(record.get("secret") or ""), event, attempts)
    outcome = classify(status)
    if outcome == "retry" and attempts >= MAX_ATTEMPTS:
        outcome = "dropped"
    if outcome == "retry":
        await d1_run(
            env,
            "UPDATE repo_webhook_outbox SET attempts=?, next_ts=? WHERE id=?",
            attempts, now + backoff_ms(attempts), row.get("id"))
    else:
        await d1_run(
            env, "DELETE FROM repo_webhook_outbox WHERE id=?", row.get("id"))
    failures_sql = (
        "failures=0" if outcome == "delivered" else "failures=failures+1")
    try:
        await d1_run(
            env,
            "UPDATE repo_webhooks SET last_status=?, last_delivery_at=?, "
            + failures_sql + " WHERE webhook_id=?",
            int(status), now, webhook_id)
    except Exception:
        pass
    if outcome != "retry":
        await _record_delivery(
            env, repo_bi, webhook_id, event_type, status, attempts, outcome,
            now)
    return outcome


async def drain_outbox(env, limit):
    """Deliver due rows. Called from the cron phase and after an enqueue."""
    now = _now()
    rows = await d1_all(
        env,
        "SELECT id, webhook_id, repo_bi, attempts, created_at, data"
        " FROM repo_webhook_outbox WHERE next_ts<=? ORDER BY next_ts ASC"
        " LIMIT ?",
        now, int(limit))
    delivered = 0
    for row in rows or []:
        try:
            if await _attempt_row(env, row, now) == "delivered":
                delivered += 1
        except Exception:
            continue
    return delivered


async def queue_event(env, owner, repo, event_type, data, digest,
                      occurred_at=0, origin=""):
    """Queue ``event_type`` for every hook on ``owner/repo`` subscribed to it.

    Best-effort by contract: the callers are catalog publication, merge
    completion and release publication, and none of them may fail because a
    third-party CI endpoint is unhappy. Every failure path here — including a
    missing runtime binding — returns 0 rather than raising, so it fails
    closed: no delivery, and the push, merge or release still succeeds.
    """
    try:
        owner = str(owner or "").strip().lower()
        repo = str(repo or "").strip()
        if not owner or not repo or event_type not in EVENT_TYPES:
            return 0
        app_event = {"repository.push": "repository.push",
                     "pull.merged": "pull.merged"}.get(event_type)
        emit = globals().get("apps_emit_repo_event")
        if app_event and emit is not None:
            await emit(env, owner, repo, app_event, dict(
                data if isinstance(data, dict) else {}, digest=str(
                    digest or "")))
        repo_bi = await _repo_bi(env, owner, repo)
        await _apply_seeds(env, owner, repo, repo_bi)
        # Cheapest question first. Almost every push is to a repository with no
        # webhook at all, and this is one indexed read that usually returns
        # nothing; the visibility lookup below is a second read worth paying
        # for only once an endpoint is actually waiting for the event.
        hooks = await _load_webhooks(env, repo_bi)
        if not hooks:
            return 0
        # A private repository does not hand its branch names, commit ids and
        # contributor names to a third party. The catalog publication path
        # already gates on public visibility, but a merge can be performed on a
        # private repository, so the rule is enforced here for every caller.
        if await _repo_is_private(env, owner, repo):
            return 0
        now = _now()
        queued = 0
        for row, record in hooks:
            if int(row.get("active") or 0) != 1:
                continue
            if event_type not in normalize_events(record.get("events")):
                continue
            url = clean_url(record.get("url"))
            if not url:
                continue
            webhook_id = str(row.get("webhook_id") or "")
            # Same event, same id: only the per-endpoint sequence differs, so
            # two endpoints deduplicate the delivery the same way.
            event = build_event(
                owner, repo, event_type, digest, data, occurred_at or now,
                await _next_sequence(env, repo_bi, webhook_id),
                origin or _webhook_origin(env))
            payload = await encrypt_row(env, {
                "url": url,
                "secret": str(record.get("secret") or ""),
                "event": event,
            })
            await d1_run(
                env,
                "INSERT INTO repo_webhook_outbox (webhook_id, repo_bi,"
                " event_id, attempts, next_ts, created_at, data)"
                " VALUES (?,?,?,0,?,?,?)",
                webhook_id, repo_bi, event["id"], now, now, payload)
            queued += 1
        if queued:
            await drain_outbox(env, min(queued, OUTBOX_DRAIN_IMMEDIATE))
        return queued
    except Exception:
        return 0


# --- owner-facing route ------------------------------------------------------

async def repo_webhooks_handler(env, request, owner, repo):
    """Owner-only webhook management for one repository.

    POST-only so the session token rides in the body rather than a query string
    an access log would keep — the same envelope the About editor and the
    fediverse post manager use.
    """
    if method_name(request) != "POST":
        return json_response({"error": "method_not_allowed"}, status=405)
    await ensure_schema(env)
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not await _authorize_repo_owner_web(env, request, owner, repo, data):
        return json_response({"error": "not_authorized"}, status=403)
    action = clean_string(data.get("action", "list"), 12).lower()
    repo_bi = await _repo_bi(env, owner, repo)
    await _apply_seeds(env, owner, repo, repo_bi)
    hooks = await _load_webhooks(env, repo_bi)

    if action == "list":
        deliveries = await d1_all(
            env,
            "SELECT webhook_id, event_type, status, attempts, outcome, ts"
            " FROM repo_webhook_deliveries WHERE repo_bi=? ORDER BY ts DESC"
            " LIMIT ?",
            repo_bi, DELIVERY_HISTORY_PER_HOOK * 4)
        return json_response({
            "ok": True,
            "webhooks": [public_view(row, record) for row, record in hooks],
            "events": [
                {"type": key, "label": label, "hint": hint}
                for key, label, hint in EVENT_CATALOG
            ],
            "deliveries": [
                {
                    "webhookId": str(row.get("webhook_id") or ""),
                    "eventType": str(row.get("event_type") or ""),
                    "status": int(row.get("status") or 0),
                    "attempts": int(row.get("attempts") or 0),
                    "outcome": str(row.get("outcome") or ""),
                    "ts": int(row.get("ts") or 0),
                }
                for row in deliveries or []
            ],
            "limits": {
                "maxWebhooks": MAX_WEBHOOKS_PER_REPO,
                "maxAttempts": MAX_ATTEMPTS,
                "signatureScheme": WEBHOOK_SCHEME,
            },
        }, cache_control="no-store")

    if action == "create":
        if len(hooks) >= MAX_WEBHOOKS_PER_REPO:
            return json_response({"error": "too_many_webhooks"}, status=409)
        url = clean_url(data.get("url"))
        if not url:
            return json_response({"error": "invalid_url"}, status=400)
        events = normalize_events(data.get("events"))
        if not events:
            return json_response({"error": "no_events"}, status=400)
        if any(record.get("url") == url for _row, record in hooks):
            return json_response({"error": "duplicate_url"}, status=409)
        secret = _random_hex(SECRET_BYTES)
        now = _now()
        webhook_id = _random_hex(16)
        encrypted = await encrypt_row(env, {
            "url": url,
            "secret": secret,
            "events": list(events),
            "description": clean_description(data.get("description")),
            "createdBy": clean_string(
                data.get("ownerAccount", ""), MAX_NODE_NAME).lower(),
        })
        await d1_run(
            env,
            "INSERT INTO repo_webhooks (webhook_id, repo_bi, active, seed_key,"
            " created_at, updated_at, data) VALUES (?,?,1,'',?,?,?)",
            webhook_id, repo_bi, now, now, encrypted)
        row = {"webhook_id": webhook_id, "active": 1, "seed_key": "",
               "created_at": now, "updated_at": now}
        record = {"url": url, "events": list(events),
                  "description": clean_description(data.get("description"))}
        # The signing secret is shown once. It is stored encrypted and there is
        # no read-back route; an owner who loses it rotates instead.
        return json_response(
            {"ok": True, "webhook": public_view(row, record, secret)},
            status=201, cache_control="no-store")

    webhook_id = clean_string(data.get("id", ""), 64).strip().lower()
    match = next(
        ((row, record) for row, record in hooks
         if str(row.get("webhook_id") or "") == webhook_id), None)
    if not webhook_id or match is None:
        return json_response({"error": "not_found"}, status=404)
    row, record = match

    if action == "update":
        events = normalize_events(
            data.get("events") if "events" in data else record.get("events"))
        if not events:
            return json_response({"error": "no_events"}, status=400)
        url = record.get("url")
        if "url" in data:
            url = clean_url(data.get("url"))
            if not url:
                return json_response({"error": "invalid_url"}, status=400)
        description = (
            clean_description(data.get("description"))
            if "description" in data else record.get("description", ""))
        active = 1 if data.get("active", True) else 0
        now = _now()
        encrypted = await encrypt_row(env, {
            **record,
            "url": url,
            "events": list(events),
            "description": description,
        })
        await d1_run(
            env,
            "UPDATE repo_webhooks SET active=?, updated_at=?, data=?"
            " WHERE webhook_id=? AND repo_bi=?",
            active, now, encrypted, webhook_id, repo_bi)
        updated_row = dict(row)
        updated_row.update({"active": active, "updated_at": now})
        return json_response({
            "ok": True,
            "webhook": public_view(
                updated_row,
                {"url": url, "events": list(events),
                 "description": description}),
        }, cache_control="no-store")

    if action == "rotate":
        secret = _random_hex(SECRET_BYTES)
        now = _now()
        encrypted = await encrypt_row(env, {**record, "secret": secret})
        await d1_run(
            env,
            "UPDATE repo_webhooks SET updated_at=?, data=? WHERE webhook_id=?"
            " AND repo_bi=?",
            now, encrypted, webhook_id, repo_bi)
        updated_row = dict(row)
        updated_row["updated_at"] = now
        return json_response(
            {"ok": True, "webhook": public_view(updated_row, record, secret)},
            cache_control="no-store")

    if action == "delete":
        await d1_run(
            env, "DELETE FROM repo_webhooks WHERE webhook_id=? AND repo_bi=?",
            webhook_id, repo_bi)
        await d1_run(
            env, "DELETE FROM repo_webhook_outbox WHERE webhook_id=?",
            webhook_id)
        await d1_run(
            env, "DELETE FROM repo_webhook_deliveries WHERE webhook_id=?",
            webhook_id)
        await d1_run(
            env, "DELETE FROM repo_webhook_sequence WHERE webhook_id=?",
            webhook_id)
        return json_response({"ok": True, "deleted": webhook_id},
                             cache_control="no-store")

    if action == "test":
        now = _now()
        event = build_event(
            owner, repo, "repository.push",
            _random_hex(16),
            {"test": True,
             "message": "Delivery test from the ForkMesh dashboard."},
            now, await _next_sequence(env, repo_bi, webhook_id),
            _webhook_origin(env, request))
        status = await _send(
            record["url"], str(record.get("secret") or ""), event, 1)
        # A test is a single probe, not a queued event, so there is no third
        # "will be retried" state to report: it either landed or it did not.
        outcome = (
            "delivered" if classify(status) == "delivered" else "failed")
        await _record_delivery(
            env, repo_bi, webhook_id, "repository.push", status, 1,
            outcome, now)
        await d1_run(
            env,
            "UPDATE repo_webhooks SET last_status=?, last_delivery_at=?"
            " WHERE webhook_id=?",
            int(status), now, webhook_id)
        return json_response(
            {"ok": outcome == "delivered", "status": status,
             "outcome": outcome, "url": redact_url(record["url"])},
            cache_control="no-store")

    return json_response({"error": "bad_action"}, status=400)
