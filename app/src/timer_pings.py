"""Timer pings: POST /api/notifications/timer.

The desktop's top-bar clock (the Timers package) keeps its countdowns and
alarms on the machine, so only the desktop knows when one runs out. When it
does, the desktop reports it here and the account gets an ordinary ping —
which is what reaches the phone: enqueue_notification rings the pings topic,
and that topic is pushed over APNs.

A ping raised here only ever lands in the caller's own inbox. The request is
authorised by the account's session or by a key-signed `user`/`ts`/`sig`
triple whose canonical binds the one expiry being reported, so a captured
proof cannot announce a different timer. The Worker writes the title itself
from the timer's kind; the caller supplies only the label it was given.

Split out of entry.py so Cloudflare's startup-memory validation (error 10021)
does not pay for this route on every isolate; entry.py loads it through a
_LazyModule proxy on the first request.
"""

import re


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by this domain."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


TIMER_PING_PROOF = "forkmesh-account-timer-ping-v1"
TIMER_PING_PATH_RE = re.compile(r"^/api/notifications/timer/?$")
TIMER_PING_ID_RE = re.compile(r"^[A-Za-z0-9-]{1,40}$")
# The Timers package caps a label at 40 characters; a little slack for a
# future package version, and a hard refusal past it rather than a trim that
# would no longer match what was signed.
TIMER_PING_LABEL_MAX = 80
# A countdown that ran out while the desktop was closed is announced when the
# app opens again. On the desktop that is still news; on a phone, an hour-old
# "Countdown finished" buzz is noise, so stale expiries are refused.
TIMER_PING_MAX_AGE_MS = 15 * 60 * 1000
# Desktop and relay clocks disagree a little; an expiry reported a moment
# before the relay's clock says it happened is still a real one.
TIMER_PING_FUTURE_SKEW_MS = 2 * 60 * 1000
TIMER_PING_TITLES = {
    "countdown": "Countdown finished",
    "alarm": "Alarm",
}
TIMER_PING_EMPTY_BODIES = {
    "countdown": "Your countdown ran out.",
    "alarm": "Your alarm is ringing.",
}


def timer_ping_canonical(user, ts, timer_id, kind, ends_at, label):
    """The bytes a desktop signs for one timer expiry.

    Same layout as the desktop's signOrgTaskProof: proof, signer, the
    resource, then the timestamp. The resource names the timer, its kind, the
    expiry instant and the label, so the proof covers exactly one ping.
    """
    resource = "\n".join([timer_id, kind, str(ends_at), label])
    return "\n".join([TIMER_PING_PROOF, user, resource, str(ts)]).encode()


def _timer_ping_fields(data):
    """Validate the body; returns (timer_id, kind, ends_at, label) or None."""
    if not isinstance(data, dict):
        return None
    timer_id = data.get("timerId")
    kind = data.get("kind")
    label = data.get("label", "")
    if not isinstance(timer_id, str) or not TIMER_PING_ID_RE.match(timer_id):
        return None
    if kind not in TIMER_PING_TITLES:
        return None
    if not isinstance(label, str) or len(label) > TIMER_PING_LABEL_MAX:
        return None
    ends_at = data.get("endsAt")
    if isinstance(ends_at, bool):
        return None
    try:
        ends_at = int(ends_at)
    except (TypeError, ValueError, OverflowError):
        return None
    if ends_at <= 0:
        return None
    return timer_id, kind, ends_at, label


async def _timer_ping_signed_account(env, request, fields):
    """The account behind a key-signed timer ping, or ""."""
    url = urlparse(request.url)
    if not TIMER_PING_PATH_RE.match(url.path):
        return ""
    params = parse_qs(url.query)
    user = clean_string(params.get("user", [""])[0], MAX_NODE_NAME).lower()
    ts = clean_string(params.get("ts", [""])[0], 20)
    sig = clean_string(params.get("sig", [""])[0], 200)
    if not valid_node_name(user) or not sig or not _ts_ok(ts):
        return ""
    timer_id, kind, ends_at, label = fields
    canonical = timer_ping_canonical(user, ts, timer_id, kind, ends_at, label)
    if not await _verify_owner_signature(env, user, sig, canonical):
        return ""
    _, record = await _account_row(env, user)
    if not record or record.get("status") != "active":
        return ""
    return user


async def timer_ping_handler(env, request):
    await ensure_schema(env)
    no_store = "no-store"
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control=no_store, extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control=no_store)
    fields = _timer_ping_fields(data)
    if fields is None:
        return json_response(
            {"error": "bad_request"}, status=400, cache_control=no_store)
    account = await _authed_account_name(env, request, data)
    if not valid_node_name(account):
        account = await _timer_ping_signed_account(env, request, fields)
    if not valid_node_name(account):
        return json_response(
            {"error": "unauthorized"}, status=401, cache_control=no_store)

    timer_id, kind, ends_at, label = fields
    now = int(Date.now())
    if (ends_at > now + TIMER_PING_FUTURE_SKEW_MS
            or now - ends_at > TIMER_PING_MAX_AGE_MS):
        return json_response(
            {"error": "stale", "maxAgeMs": TIMER_PING_MAX_AGE_MS},
            status=409, cache_control=no_store)

    body = clean_string(label, TIMER_PING_LABEL_MAX).strip()
    # One ping per expiry: a retry, or a second desktop on the same account
    # holding the same synced timer, rewrites the row instead of adding one.
    # A snoozed alarm has a new expiry, so it rings again.
    dedupe = "timer:" + timer_id + ":" + str(ends_at)
    await enqueue_notification(
        env, account, "timer_finished", TIMER_PING_TITLES[kind],
        body=body or TIMER_PING_EMPTY_BODIES[kind],
        source="timer", dedupe=dedupe, ts=now,
        meta={"timerId": timer_id, "timerKind": kind, "endsAt": ends_at})
    return json_response({"ok": True}, cache_control=no_store)
