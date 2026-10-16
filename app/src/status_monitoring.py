"""On-demand /status page sampler and history projection.

Cloudflare validates a Python Worker by compiling and executing its
entrypoint under the isolate memory limit. The minute sampler runs from the
ForkMeshCronRunner alarm and the history projection only
serves the /status page, so entry.py loads this module on first use instead
of spending scarce Pyodide startup memory on every isolate.
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by the status implementation."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


EMAIL_STATUS_LOOKBACK_MS = 24 * 60 * 60 * 1000
EMAIL_DELIVERY_GRACE_MS = 30 * 60 * 1000
EMAIL_DELIVERY_FAILURE_STATES = {
    "failed", "soft bounce", "bounce", "suspension", "reject",
}
EMAIL_DELIVERY_CONFIRMED_STATES = {
    "delivery", "open", "click", "unsubscribe", "spam",
}
EMAIL_RECIPIENT_ADDRESS_DSNS = {
    "5.1.1", "5.1.2", "5.1.3", "5.1.6", "5.1.10", "5.2.1",
}
EMAIL_DSN_CAUSES = {
    "1": "the recipient address was not accepted",
    "2": "the recipient's mailbox did not accept it",
    "3": "the recipient's mail system did not accept it",
    "4": "the recipient's mail server could not be reached",
    "4.3": "looking up the recipient's mail server in DNS failed",
    "4.4": "the recipient's domain has no mail server",
    "5": "the SMTP exchange with the recipient's server failed",
    "6": "the recipient's server refused the message content",
    "7": "the recipient's server refused it for security or policy reasons",
}
STATUS_EMAIL_SMTP_CHECK = (
    "Checks that the core mail service (Postfix) answers on loopback, then "
    "inspects the delivery result Postfix logged for the most recent email. "
    "Passes when it was delivered, or is still inside its first 30 minutes. "
    "A bounce because the recipient address does not exist is the "
    "recipient's problem and still passes. Recipient addresses, subjects, "
    "and message bodies are never stored in this health signal.")
STATUS_EMAIL_SMTP_CHECK_STEPS = (
    ("Mail service answers",
     "Every minute, Postfix on the core must answer an SMTP NOOP on "
     "loopback."),
    ("No failed sends",
     "The most recent email must not have bounced or been rejected, except "
     "for a recipient address that does not exist."),
    ("Delivery within 30 min",
     "The most recent email must be delivered within 30 minutes. A "
     "deferral names the cause Postfix reported."),
)
STATUS_EMAIL_SMTP_GUIDANCE = (
    "the postfix journal and mail queue on the cores, and the DNS resolver "
    "they use",
    "Run tools/next_mail.py check on each core, read the deferral reason in "
    "the postfix journal, then fix the resolver, outbound port 25 or the "
    "recipient-side cause it names.")
MAILTRAP_EMAIL_LOG_URL = "https://mailtrap.io/api/email_logs"
STATUS_SYSTEM_CHECK_STEPS = {
    "website": (
        ("Homepage loads",
         "Every minute, fetches the production homepage and "
         "requires HTTP 200."),
        ("Homepage marker",
         "The returned index document must carry the ForkMesh homepage "
         "marker, so an error page served with 200 still fails."),
        ("Page-route errors",
         "Every minute, scans the Worker error log for page-route failures. "
         "Error reports clients file about themselves are excluded."),
    ),
    "world_mobile": (
        ("Mobile health endpoint",
         "Every minute, requests World's /health/mobile with a "
         "mobile Safari profile."),
        ("Entry document",
         "The World entry document must be present with the expected type "
         "and a bounded size."),
        ("Scene builder",
         "The scene builder script must be present with the expected type "
         "and a bounded size."),
        ("Three.js modules",
         "Both pinned same-origin Three.js modules must be present with the "
         "expected types and bounded sizes."),
    ),
    "status_page": (
        ("Status page HTML",
         "Loads the deployed status page from the production static-asset "
         "binding."),
        ("Shared stylesheet",
         "Loads the shared stylesheet the status page depends on."),
        ("Status snapshot",
         "The prior /api/status row in the public-snapshot table must match "
         "this deployment and be unexpired."),
    ),
    "api": (
        ("API errors",
         "Scans the last minute of the Worker error log for unhandled "
         "exceptions or 5xx responses on /api/* routes."),
        ("Mirror outages excluded",
         "502/503/504 on direct-mirror content paths are left to the Git "
         "hosting network check."),
    ),
    "errors": (
        ("Worker error log",
         "Scans the last minute for any unexpected page, API, realtime or "
         "Durable Object failure."),
        ("Expected responses excluded",
         "Degraded direct-mirror 502/503/504 responses and client "
         "self-reports do not count."),
    ),
    "database": (
        ("SELECT round trip",
         "Runs a real SELECT against the database every minute and requires "
         "a row back."),
    ),
    "email": (
        ("Sending configured",
         "The Mailtrap sending API must be configured."),
        ("Delivery webhook",
         "The signed delivery webhook must be configured."),
        ("No failed sends",
         "The most recent account email lifecycle must show no failed send."),
        ("Delivery within 30 min",
         "The latest accepted email must receive a delivery lifecycle event "
         "within 30 minutes."),
    ),
    "flagship_repository": (
        ("Repository page",
         "Every minute, loads https://forkmesh.com/forkmesh/forkmesh."),
        ("Root tree",
         "Loads the root repository tree through the public API the page "
         "uses."),
        ("README",
         "Loads README.md pinned to the tree's immutable commit."),
        ("Issue",
         "Loads one issue pinned to the same commit."),
        ("Discussion",
         "Loads one discussion pinned to the same commit."),
        ("Pull request",
         "Loads one pull request from the immutable forkmesh/pulls branch; "
         "an empty ledger is not an outage."),
        ("Serving mirror",
         "Every routed response must identify the eligible mirror that "
         "served it."),
        ("Visitor 5xx",
         "Every minute, fails if visitors' own reads under "
         "/api/repo/forkmesh/forkmesh/ were answered with HTTP 5xx."),
    ),
    "release_linux": (
        ("Signed manifest",
         "Every minute, verifies the signed latest release manifest and, "
         "when main names a newer desktop version, its candidate manifest."),
        ("Newest version",
         "The newest signed release must carry the Linux AppImage with a "
         "valid digest and size."),
        ("Download",
         "The Linux build must download from a reachable mirror by its "
         "content address."),
    ),
    "release_android": (
        ("Signed manifest",
         "Every minute, verifies the signed latest release manifest and, "
         "when main names a newer desktop version, its candidate manifest."),
        ("Newest version",
         "The newest signed release must carry the Android APK with a "
         "valid digest and size."),
        ("Download",
         "The Android build must download from a reachable mirror by its "
         "content address."),
    ),
    "release_windows": (
        ("Signed manifest",
         "Every minute, verifies the signed latest release manifest and, "
         "when main names a newer desktop version, its candidate manifest."),
        ("Newest version",
         "The newest signed release must carry the Windows installer with a "
         "valid digest and size."),
        ("Download",
         "The Windows build must download from a reachable mirror by its "
         "content address."),
    ),
    "release_macos": (
        ("Signed manifest",
         "Every minute, verifies the signed latest release manifest and, "
         "when main names a newer desktop version, its candidate manifest."),
        ("Newest version",
         "The newest signed release must carry both macOS DMGs with a "
         "valid digest and size."),
        ("Download",
         "The macOS build must download from a reachable mirror by its "
         "content address."),
    ),
    "git_hosting": (
        ("Signed mirror proof",
         "At least one account-bound direct HTTPS endpoint must hold a "
         "signed, integrity-matching repository proof."),
        ("Freshness",
         "That proof must have been observed within the last 10 minutes."),
    ),
    "realtime": (
        ("Chat & WebSockets",
         "Scans the last minute of the Worker error log for chat room and "
         "WebSocket failures."),
        ("Smart HTTP",
         "Scans the same minute for repository smart-HTTP request "
         "failures."),
    ),
    "durable_objects": (
        ("Platform aborts",
         "Counts room requests the Durable Objects platform aborted in the "
         "last minute, including the duration cap; any abort fails."),
    ),
}
STATUS_SYSTEM_PROBE_PHASES = {
    "flagship_repository": ("probe-flagship",),
    "release_linux": ("probe-release-platforms",),
    "release_android": ("probe-release-platforms",),
    "release_windows": ("probe-release-platforms",),
    "release_macos": ("probe-release-platforms",),
}
STATUS_SAMPLE_PHASE = "status-sample"
STATUS_NEXT_CHECK_SCAN_MINUTES = 6 * 60 + 1


def _status_phase_slots(current_minute_ms):
    minute = int(current_minute_ms) // 60000
    wanted = {STATUS_SAMPLE_PHASE}
    for phases in STATUS_SYSTEM_PROBE_PHASES.values():
        wanted.update(phases)
    upcoming = {}
    previous = {}
    for offset in range(STATUS_NEXT_CHECK_SCAN_MINUTES):
        ahead = minute + 1 + offset
        for phase in _cron_due_phases(ahead):
            if phase in wanted:
                upcoming.setdefault(phase, ahead * 60000)
        behind = minute - offset
        for phase in _cron_due_phases(behind):
            if phase in wanted:
                previous.setdefault(phase, behind * 60000)
        if len(upcoming) == len(wanted) and len(previous) == len(wanted):
            break
    return upcoming, previous


def _status_next_check(system_id, failing, upcoming, previous):
    phases = STATUS_SYSTEM_PROBE_PHASES.get(system_id)
    if not phases:
        return {
            "at": upcoming.get(STATUS_SAMPLE_PHASE),
            "last": previous.get(STATUS_SAMPLE_PHASE),
            "kind": "sample",
            "everyMs": 60000,
        }
    ahead = [upcoming[phase] for phase in phases if phase in upcoming]
    behind = [previous[phase] for phase in phases if phase in previous]
    if not ahead:
        return None
    return {
        "at": min(ahead),
        "last": max(behind) if behind else None,
        "kind": "probe",
        "everyMs": 60000,
    }


STATUS_MIRROR_CHECK_STEPS = (
    ("Signed health",
     "The mirror's signed health report must be fresh and good."),
    ("Repository proof",
     "Its signed forkmesh/forkmesh repository proof must be fresh with "
     "integrity ok."),
    ("Accepted refs",
     "The refs it serves must match an accepted source revision."),
    ("Release artifact count",
     "Its signed release-artifact count must match the canonical source."),
)
STATUS_CORE_CHECK_STEPS = (
    ("Cluster heartbeat",
     "The core's own cluster membership heartbeat must be under 30 seconds "
     "old, which needs a working rqlite quorum and write path."),
    ("Unsealed and active",
     "It must report itself active: not sealed, quarantined or leaving the "
     "cluster."),
    ("Peer HTTPS probes",
     "Every fresh readiness probe the core nodes publish for it must pass, "
     "both for the shared app hostname and for its own node hostname."),
    ("Intrusion detector",
     "Once its intrusion detector has reported, it must stay fresh and out "
     "of lockdown."),
)
STATUS_RUNNER_CHECK_STEPS = (
    ("Registered and active",
     "The runner must be a registered pool runner that has not been "
     "revoked."),
    ("Recent claim or poll",
     "It must have claimed jobs or polled the runner API within the last "
     "three minutes. Runners poll about every 20 seconds, busy or idle."),
    ("Provider logins",
     "No Claude Code or Codex login on it may have expired. A red row names "
     "the provider; reconnect it from Runners (Connect Claude or Connect "
     "Codex)."),
)
# Mailtrap's Email Logs API reports one settled status per message, in its own
# vocabulary. Map it onto the lifecycle events the webhook writes so both paths
# agree on what the row means. "enqueued" is deliberately absent: it is still in
# flight and settles nothing.
MAILTRAP_LOG_SETTLED_STATUS = {
    "delivered": "delivery",
    "not_delivered": "failed",
    "opted_out": "unsubscribe",
}


def _mailtrap_log_message(payload, send_id):
    """Pick the logged message for ``send_id`` out of either log shape.

    The by-id endpoint answers with the message object itself; the filtered
    list answers with ``{"messages": [...]}``, which is matched on the custom
    variable the send stamped.
    """
    if not isinstance(payload, dict):
        return None
    messages = payload.get("messages")
    if not isinstance(messages, list):
        return payload if payload.get("status") else None
    for message in messages:
        if not isinstance(message, dict):
            continue
        variables = message.get("custom_variables")
        variables = variables if isinstance(variables, dict) else {}
        logged = clean_string(
            variables.get("forkmesh_send_id") or "", 64).lower()
        if logged and logged == send_id:
            return message
    return None


async def _mailtrap_logged_delivery(token, send_id, message_id, sent_at):
    """Ask Mailtrap's own log what became of one accepted message.

    ``mailtrap_email_sends.status`` otherwise only ever leaves ``accepted``
    when an inbound webhook arrives. A single webhook Mailtrap dropped — or
    that this Worker rejected, which it does silently because a bad signature
    is a 401 rather than a logged fault — therefore pinned a delivered message
    at ``accepted`` forever, and the check below read that one pinned row as a
    total email outage. The sending API's log is authoritative, so consult it
    before calling the platform down. Returns the settled lifecycle status and
    the message handle, or ``("", "")`` when nothing is settled yet.
    """
    if message_id:
        url = MAILTRAP_EMAIL_LOG_URL + "/" + quote(message_id, safe="")
    else:
        # Mailtrap orders logs by sent_at descending, so one window starting a
        # grace period before the send holds the message without paging.
        since = time.strftime(
            "%Y-%m-%dT%H:%M:%SZ",
            time.gmtime(max(0, sent_at - EMAIL_DELIVERY_GRACE_MS) / 1000))
        url = (MAILTRAP_EMAIL_LOG_URL + "?filters%5Bsent_after%5D=" +
               quote(since, safe=""))
    try:
        response = await js_fetch_with_timeout(
            url,
            {"method": "GET", "headers": {"api-token": token}},
            CRON_OUTBOUND_FETCH_TIMEOUT_SECONDS,
        )
        if int(getattr(response, "status", 0) or 0) != 200:
            return "", ""
        message = _mailtrap_log_message(
            json.loads(str(await response.text())), send_id)
    except Exception:
        return "", ""
    if not message:
        return "", ""
    logged = clean_string(message.get("status") or "", 40).strip().lower()
    handle = clean_string(message.get("message_id") or "", 160).strip()
    # "enqueued" is still in flight: it settles nothing either way, so leave
    # the row alone and let the grace window keep running.
    return MAILTRAP_LOG_SETTLED_STATUS.get(logged, ""), handle


def _smtp_answers(host="127.0.0.1", port=25, timeout=3):
    import smtplib
    with smtplib.SMTP(host, port, timeout=timeout) as client:
        code, _ = client.noop()
    return code == 250


def _mail_over_smtp(env):
    return (getattr(env, "MAIL_TRANSPORT", "") or "").strip().lower() == "smtp"


def _mail_dsn_cause(dsn):
    parts = str(dsn or "").split(".")
    if len(parts) != 3 or parts[0] not in ("4", "5"):
        return ""
    cause = (EMAIL_DSN_CAUSES.get(parts[1] + "." + parts[2])
             or EMAIL_DSN_CAUSES.get(parts[1]))
    return ": %s (%s)" % (cause, dsn) if cause else " (%s)" % dsn


async def _core_mail_status(env, now):
    import asyncio
    try:
        answers = await asyncio.to_thread(_smtp_answers)
    except Exception:
        answers = False
    if not answers:
        return False, "The core mail service (Postfix) is not answering"
    row = await d1_first(
        env,
        "SELECT status,sent_at,dsn FROM mail_sends WHERE sent_at>=? "
        "AND status<>'' ORDER BY sent_at DESC LIMIT 1",
        int(now) - EMAIL_STATUS_LOOKBACK_MS,
    )
    if not row:
        return True, ""
    status = clean_string(row.get("status") or "", 40).strip().lower()
    dsn = clean_string(row.get("dsn") or "", 16).strip()
    old = int(now) - max(0, int(row.get("sent_at") or 0)) >= EMAIL_DELIVERY_GRACE_MS
    if status in ("bounce", "reject"):
        if dsn in EMAIL_RECIPIENT_ADDRESS_DSNS:
            return True, ""
        return False, ("The most recent email was reported as " + status
                       + _mail_dsn_cause(dsn))
    if status == "delivery":
        return True, ""
    if old and status == "soft bounce":
        return False, ("The most recent email is still deferred after 30 "
                       "minutes" + _mail_dsn_cause(dsn))
    if old:
        return False, (
            "The most recent email has no delivery event after 30 minutes")
    return True, ""


async def _email_delivery_status(env, now):
    """Return the bounded sending and delivery health signal."""
    if _mail_over_smtp(env):
        return await _core_mail_status(env, now)
    token = str(getattr(env, "MAILTRAP_API_TOKEN", "") or "").strip()
    webhook_secret = str(
        getattr(env, "MAILTRAP_WEBHOOK_SECRET", "") or "").strip()
    if not token or token == "CHANGE-ME":
        return False, "Mailtrap sending API is not configured"
    if not webhook_secret or webhook_secret == "CHANGE-ME":
        return False, "Mailtrap delivery webhook is not configured"
    row = await d1_first(
        env,
        "SELECT send_id,accepted,status,sent_at,status_at,message_id "
        "FROM mailtrap_email_sends WHERE sent_at>=? "
        "ORDER BY sent_at DESC LIMIT 1",
        int(now) - EMAIL_STATUS_LOOKBACK_MS,
    )
    if not row:
        return True, ""
    accepted = int(row.get("accepted") or 0) == 1
    status = clean_string(row.get("status") or "", 40).strip().lower()
    sent_at = max(0, int(row.get("sent_at") or 0))
    if not accepted:
        return False, "Mailtrap sending API rejected the most recent email"
    if status in EMAIL_DELIVERY_FAILURE_STATES:
        return False, "Mailtrap reported the most recent email as " + status
    if status in EMAIL_DELIVERY_CONFIRMED_STATES:
        return True, ""
    if sent_at and int(now) - sent_at >= EMAIL_DELIVERY_GRACE_MS:
        send_id = clean_string(row.get("send_id") or "", 64).lower()
        known = clean_string(row.get("message_id") or "", 160).strip()
        settled, handle = await _mailtrap_logged_delivery(
            token, send_id, known, sent_at)
        handle = handle or known
        if settled and send_id:
            # Persist what the log settled so the row stops depending on a
            # webhook that is never going to arrive, and so this reconciliation
            # runs at most once per missed event rather than every minute.
            await d1_run(
                env,
                "UPDATE mailtrap_email_sends "
                "SET status=?,status_at=?,message_id=? WHERE send_id=?",
                settled, int(now), handle, send_id,
            )
        if settled in EMAIL_DELIVERY_FAILURE_STATES:
            return False, (
                "Mailtrap reported the most recent email as " + settled)
        if settled:
            return True, ""
        return False, (
            "The most recent accepted email has no delivery event after "
            "30 minutes")
    return True, ""


def _status_batch_system_id(value):
    """Return one bounded public status id, or an empty string if unsafe."""
    system_id = str(value or "")
    if system_id in {item[0] for item in STATUS_SYSTEMS}:
        return system_id
    if system_id.startswith(STATUS_CORE_PREFIX):
        core_name = system_id[len(STATUS_CORE_PREFIX):]
        return system_id if _status_core_name(core_name) == core_name else ""
    if system_id.startswith(STATUS_RUNNER_PREFIX):
        runner_name = system_id[len(STATUS_RUNNER_PREFIX):]
        return (
            system_id if _status_core_name(runner_name) == runner_name else "")
    if not system_id.startswith(STATUS_MIRROR_PREFIX):
        return ""
    mirror_name = system_id[len(STATUS_MIRROR_PREFIX):]
    if (
        mirror_name in STATUS_RETIRED_MIRRORS
        or not re.fullmatch(
            r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", mirror_name)
    ):
        return ""
    return system_id


def _status_batch_retired_mirror(value):
    system_id = str(value or "")
    return (
        system_id.startswith(STATUS_MIRROR_PREFIX)
        and system_id[len(STATUS_MIRROR_PREFIX):] in STATUS_RETIRED_MIRRORS
    )


def _status_batch_decode(raw, bucket):
    """Validate one roster-independent status bucket before using it.

    Dedicated caps and exact keys make a poisoned D1 row fail as missing data;
    it can never inflate the public response or smuggle mutable fields into it.
    """
    text = str(raw or "")
    max_bytes = STATUS_BATCH_MAX_BYTES[bucket]
    if not text or len(text.encode("utf-8")) > max_bytes:
        return {}
    try:
        payload = json.loads(text)
    except Exception:
        return {}
    if (
        not isinstance(payload, dict)
        or set(payload) != {"schemaVersion", "systems"}
        or payload.get("schemaVersion") != STATUS_BATCH_SCHEMA_VERSION
        or not isinstance(payload.get("systems"), dict)
        or len(payload["systems"])
        > len(STATUS_SYSTEMS) + STATUS_MIRROR_MAX + STATUS_CORE_MAX
        + STATUS_RUNNER_MAX
    ):
        return {}
    decoded = {}
    expected_keys = (
        {"ok", "reason"}
        if bucket == "minute"
        else {"checks", "failures", "reason"}
        if bucket == "hour"
        else {"checks", "failures"}
    )
    max_checks = 1 if bucket == "minute" else 60 if bucket == "hour" else 1440
    predecessors = dict(STATUS_SYSTEM_SUCCESSORS)
    inherited = {}
    for raw_system_id, item in payload["systems"].items():
        if _status_batch_retired_mirror(raw_system_id):
            continue
        system_id = (
            raw_system_id if raw_system_id in predecessors
            else _status_batch_system_id(raw_system_id))
        if (
            not system_id
            or not isinstance(item, dict)
            or set(item) != expected_keys
        ):
            return {}
        if bucket == "minute":
            ok_value = item.get("ok")
            if not isinstance(ok_value, bool):
                return {}
            reason_value = item.get("reason")
            if (
                reason_value is not None
                and (
                    not isinstance(reason_value, str)
                    or len(reason_value) > 240
                )
            ):
                return {}
            (inherited if system_id in predecessors else decoded)[system_id] = {
                "ok": 1 if ok_value else 0,
                "reason": reason_value or None,
            }
            continue
        checks = item.get("checks")
        failures = item.get("failures")
        if (
            isinstance(checks, bool)
            or not isinstance(checks, int)
            or isinstance(failures, bool)
            or not isinstance(failures, int)
            or checks < 0
            or checks > max_checks
            or failures < 0
            or failures > checks
        ):
            return {}
        decoded_item = {"checks": checks, "failures": failures}
        if bucket == "hour":
            reason_value = item.get("reason")
            if (
                reason_value is not None
                and (
                    not isinstance(reason_value, str)
                    or len(reason_value) > 240
                )
            ):
                return {}
            decoded_item["reason"] = reason_value or None
        (inherited if system_id in predecessors else decoded)[system_id] = (
            decoded_item)
    for predecessor, successors in STATUS_SYSTEM_SUCCESSORS:
        if predecessor in inherited:
            for successor in successors:
                decoded.setdefault(successor, dict(inherited[predecessor]))
    return decoded


def _status_batch_encode(bucket, systems):
    admitted = {}
    for raw_system_id, item in systems.items():
        system_id = _status_batch_system_id(raw_system_id)
        if not system_id:
            raise ValueError("status batch contains an invalid system id")
        admitted[system_id] = item
    body = json.dumps(
        {
            "schemaVersion": STATUS_BATCH_SCHEMA_VERSION,
            "systems": admitted,
        },
        ensure_ascii=True,
        separators=(",", ":"),
        sort_keys=True,
        allow_nan=False,
    )
    if (
        len(body.encode("utf-8")) > STATUS_BATCH_MAX_BYTES[bucket]
        or _status_batch_decode(body, bucket) != admitted
    ):
        raise ValueError("status batch payload is invalid or oversized")
    return body


# Per-isolate short cuts for _status_minute_records. Both are looked up
# through globals() so the AST test harnesses, which extract functions
# without module state, keep exercising the plain dual read.
_STATUS_MINUTE_BATCH_MEMO = {"minute_ts": None, "systems": None}
_STATUS_MINUTE_RECORDS_CACHE = {}
STATUS_MINUTE_RECORDS_CACHE_MAX = 180


async def _status_minute_records(env, start, end):
    """Dual-read one exact minute window; a valid batch bucket wins.

    The minute the sampler just wrote is answered from
    _STATUS_MINUTE_BATCH_MEMO, so the per-minute alert phases stop re-reading
    the row this isolate produced seconds earlier; completed minutes decoded
    for an earlier window are served from _STATUS_MINUTE_RECORDS_CACHE, so
    the sixty-minute /api/status rebuild reads only the minutes it has not
    seen (60 rows every three minutes became ~3).
    """
    start = int(start)
    end = int(end)
    memo = globals().get("_STATUS_MINUTE_BATCH_MEMO")
    if (
        start == end
        and isinstance(memo, dict)
        and memo.get("minute_ts") == start
        and isinstance(memo.get("systems"), dict)
    ):
        return [
            {
                "minute_ts": start,
                "system": system_id,
                "ok": 1 if item.get("ok") else 0,
                "reason": clean_string(item.get("reason") or "", 240) or None,
            }
            for system_id, item in memo["systems"].items()
            if _status_batch_system_id(system_id)
        ]
    cache = globals().get("_STATUS_MINUTE_RECORDS_CACHE")
    cached_records = []
    query_start = start
    if isinstance(cache, dict):
        while query_start <= end and query_start in cache:
            cached_records.extend(cache[query_start])
            query_start += STATUS_SAMPLE_WINDOW_MS
        if query_start > end:
            return cached_records
    records = {}
    legacy_rows = await d1_all(
        env,
        "SELECT minute_ts,system,ok,reason FROM system_status_minute "
        "WHERE minute_ts>=? AND minute_ts<=?",
        query_start, end,
    )
    for row in legacy_rows or []:
        minute_ts = int(row.get("minute_ts") or 0)
        system_id = _status_batch_system_id(row.get("system"))
        if system_id and int(start) <= minute_ts <= int(end):
            records[(minute_ts, system_id)] = {
                "minute_ts": minute_ts,
                "system": system_id,
                "ok": 1 if int(row.get("ok") or 0) == 1 else 0,
                "reason": clean_string(row.get("reason") or "", 240) or None,
            }
    batch_rows = await d1_all(
        env,
        "SELECT minute_ts,data FROM system_status_minute_batch "
        "WHERE minute_ts>=? AND minute_ts<=?",
        query_start, end,
    )
    for row in batch_rows or []:
        minute_ts = int(row.get("minute_ts") or 0)
        if not int(start) <= minute_ts <= int(end):
            continue
        decoded = _status_batch_decode(row.get("data"), "minute")
        if not decoded:
            continue
        # Matching system/minute keys are replaced, while disjoint legacy
        # systems survive a mixed-version handoff bucket.
        for system_id, item in decoded.items():
            records[(minute_ts, system_id)] = {
                "minute_ts": minute_ts,
                "system": system_id,
                "ok": item["ok"],
                "reason": item.get("reason"),
            }
    fresh = list(records.values())
    if isinstance(cache, dict):
        # Remember only minutes that can no longer change: the sampler writes
        # minute M during minute M, so anything two windows old is final. A
        # minute with no row stays uncached so a late sample is still seen.
        settled_before = (
            int(Date.now()) // STATUS_SAMPLE_WINDOW_MS - 2
        ) * STATUS_SAMPLE_WINDOW_MS
        by_minute = {}
        for record in fresh:
            by_minute.setdefault(int(record["minute_ts"]), []).append(record)
        for minute_ts, minute_records in by_minute.items():
            if minute_ts < settled_before:
                cache[minute_ts] = minute_records
        while len(cache) > STATUS_MINUTE_RECORDS_CACHE_MAX:
            cache.pop(min(cache))
    return cached_records + fresh


async def _status_hour_records(env, start, end):
    """Dual-read one exact completed-hour window; batch buckets win."""
    records = {}
    legacy_rows = await d1_all(
        env,
        "SELECT hour_ts,system,checks,failures,reason "
        "FROM system_status_hourly WHERE hour_ts>=? AND hour_ts<=?",
        int(start), int(end),
    )
    for row in legacy_rows or []:
        hour_ts = int(row.get("hour_ts") or 0)
        system_id = _status_batch_system_id(row.get("system"))
        if system_id and int(start) <= hour_ts <= int(end):
            checks = max(0, int(row.get("checks") or 0))
            records[(hour_ts, system_id)] = {
                "hour_ts": hour_ts,
                "system": system_id,
                "checks": checks,
                "failures": max(
                    0, min(int(row.get("failures") or 0), checks)),
                "reason": clean_string(row.get("reason") or "", 240) or None,
            }
    batch_rows = await d1_all(
        env,
        "SELECT hour_ts,data FROM system_status_hourly_batch "
        "WHERE hour_ts>=? AND hour_ts<=?",
        int(start), int(end),
    )
    for row in batch_rows or []:
        hour_ts = int(row.get("hour_ts") or 0)
        if not int(start) <= hour_ts <= int(end):
            continue
        decoded = _status_batch_decode(row.get("data"), "hour")
        if not decoded:
            continue
        # Hour batches are absolutely seeded from dual-read raw minutes. Keep
        # any disjoint legacy system during cutover; matching keys use that
        # authoritative seeded batch rather than double-counting it.
        for system_id, item in decoded.items():
            records[(hour_ts, system_id)] = {
                "hour_ts": hour_ts,
                "system": system_id,
                "checks": item["checks"],
                "failures": item["failures"],
                "reason": item.get("reason"),
            }
    return list(records.values())


async def _status_day_records(env, start, end):
    """Dual-read exact completed UTC days; batch buckets win."""
    records = {}
    legacy_rows = await d1_all(
        env,
        "SELECT day_ts,system,checks,failures FROM system_status_daily "
        "WHERE day_ts>=? AND day_ts<=?",
        int(start), int(end),
    )
    for row in legacy_rows or []:
        day_ts = int(row.get("day_ts") or 0)
        system_id = _status_batch_system_id(row.get("system"))
        if system_id and int(start) <= day_ts <= int(end):
            checks = max(0, int(row.get("checks") or 0))
            records[(day_ts, system_id)] = {
                "day_ts": day_ts,
                "system": system_id,
                "checks": checks,
                "failures": max(
                    0, min(int(row.get("failures") or 0), checks)),
            }
    batch_rows = await d1_all(
        env,
        "SELECT day_ts,data FROM system_status_daily_batch "
        "WHERE day_ts>=? AND day_ts<=?",
        int(start), int(end),
    )
    for row in batch_rows or []:
        day_ts = int(row.get("day_ts") or 0)
        if not int(start) <= day_ts <= int(end):
            continue
        decoded = _status_batch_decode(row.get("data"), "day")
        if not decoded:
            continue
        # Day batches are absolutely seeded from dual-read hourly buckets.
        # Preserve disjoint legacy systems, overwrite matching seeded totals.
        for system_id, item in decoded.items():
            records[(day_ts, system_id)] = {
                "day_ts": day_ts,
                "system": system_id,
                "checks": item["checks"],
                "failures": item["failures"],
            }
    return list(records.values())


async def _write_status_minute_batch(env, minute_ts, systems):
    # A retiring worker can win or partly finish the same minute during a
    # rolling cutover. Seed disjoint legacy systems before publishing the first
    # batch row; matching systems use the new sample and are never duplicated.
    legacy_rows = await d1_all(
        env,
        "SELECT minute_ts,system,ok,reason FROM system_status_minute "
        "WHERE minute_ts>=? AND minute_ts<=?",
        int(minute_ts), int(minute_ts),
    )
    systems = dict(systems)
    for row in legacy_rows or []:
        system_id = _status_batch_system_id(row.get("system"))
        if system_id and system_id not in systems:
            systems[system_id] = {
                "ok": int(row.get("ok") or 0) == 1,
                "reason": clean_string(row.get("reason") or "", 240) or None,
            }
    body = _status_batch_encode("minute", systems)
    await d1_run(
        env,
        "INSERT INTO system_status_minute_batch (minute_ts,data) VALUES (?,?) "
        "ON CONFLICT(minute_ts) DO UPDATE SET data=excluded.data "
        "WHERE system_status_minute_batch.data!=excluded.data",
        int(minute_ts), body,
    )
    memo = globals().get("_STATUS_MINUTE_BATCH_MEMO")
    if isinstance(memo, dict):
        memo["minute_ts"] = int(minute_ts)
        memo["systems"] = {
            system_id: dict(item) for system_id, item in systems.items()}


async def _status_oldest_missing_batch(env, bucket, candidates):
    """Return the oldest missing batch key from one bounded candidate set."""
    candidates = tuple(sorted({int(candidate) for candidate in candidates}))
    if not candidates:
        return None
    # D1 currently accepts at most 100 bound parameters. Keep this helper's
    # contract below that ceiling even if a caller later widens a catch-up
    # window without noticing the platform limit.
    if len(candidates) >= 100:
        raise ValueError("status compaction candidate set is too large")
    if bucket == "hour":
        column = "hour_ts"
        table = "system_status_hourly_batch"
    elif bucket == "day":
        column = "day_ts"
        table = "system_status_daily_batch"
    else:
        raise ValueError("invalid status compaction bucket")
    rows = await d1_all(
        env,
        f"SELECT {column} FROM {table} WHERE {column} IN ("
        + ",".join("?" for _candidate in candidates) + ")",
        *candidates,
    )
    existing = set()
    for row in rows or []:
        try:
            value = int(row.get(column))
        except (AttributeError, TypeError, ValueError):
            continue
        if value in candidates:
            existing.add(value)
    return next(
        (candidate for candidate in candidates if candidate not in existing),
        None,
    )


async def _compact_status_hour(env, current_hour, now):
    """Absolutely compact one completed hour, with bounded gap recovery."""
    current_hour = int(current_hour)
    latest = current_hour - STATUS_HOUR_MS
    retrying = int(now) - int(current_hour) < STATUS_COMPACTION_RETRY_MS
    if retrying:
        # The first five minutes deliberately rebuild the latest hour even if
        # it already exists, so a delayed boundary sample can join the batch.
        target = latest
    else:
        # One later invocation can heal one rollout/alarm hole without an
        # unbounded scan. These are exactly the completed hours used by the
        # public 24-hour projection, and the query stays at 24 binds.
        target = await _status_oldest_missing_batch(
            env,
            "hour",
            (current_hour - offset * STATUS_HOUR_MS
             for offset in range(24, 0, -1)),
        )
    if target is None:
        return None
    systems = {}
    if target != latest:
        # During the batch-table cutover, older per-system hour rows are still
        # valid source material after their raw minutes have aged out. The
        # dual reader lets an already-published batch win a cutover race.
        hour_rows = await _status_hour_records(env, target, target)
        for row in hour_rows:
            checks = min(60, max(0, int(row.get("checks") or 0)))
            systems[row["system"]] = {
                "checks": checks,
                "failures": min(
                    checks, max(0, int(row.get("failures") or 0))),
                "reason": clean_string(row.get("reason") or "", 240) or None,
            }
    if not systems:
        minute_rows = await _status_minute_records(
            env, target,
            target + STATUS_HOUR_MS - STATUS_SAMPLE_WINDOW_MS)
        for row in sorted(
                minute_rows, key=lambda item: int(item["minute_ts"])):
            item = systems.setdefault(
                row["system"],
                {"checks": 0, "failures": 0, "reason": None},
            )
            item["checks"] += 1
            if int(row.get("ok") or 0) != 1:
                item["failures"] += 1
                item["reason"] = (
                    clean_string(row.get("reason") or "", 240) or None)
    # An empty bucket is still a completed bucket with zero coverage. Persist
    # that marker so one unreconstructable outage window cannot remain the
    # oldest "missing" candidate and starve every newer catch-up forever.
    body = _status_batch_encode("hour", systems)
    await d1_run(
        env,
        "INSERT INTO system_status_hourly_batch (hour_ts,data) VALUES (?,?) "
        "ON CONFLICT(hour_ts) DO UPDATE SET data=excluded.data "
        "WHERE system_status_hourly_batch.data!=excluded.data",
        target, body,
    )
    return target


async def _compact_status_day(env, current_day, now, force_target=None):
    """Absolutely compact one completed day, with bounded gap recovery."""
    current_day = int(current_day)
    latest = current_day - STATUS_DAY_MS
    retrying = int(now) - int(current_day) < STATUS_COMPACTION_RETRY_MS
    if force_target is not None:
        target = int(force_target)
    elif retrying:
        target = latest
    else:
        # The current day is built from hours directly. The other 29 days in
        # the public 30-day projection are bounded catch-up candidates.
        target = await _status_oldest_missing_batch(
            env,
            "day",
            (current_day - offset * STATUS_DAY_MS
             for offset in range(STATUS_HISTORY_DAYS - 1, 0, -1)),
        )
    if target is None:
        return None
    hour_rows = await _status_hour_records(
        env, target, target + STATUS_DAY_MS - STATUS_HOUR_MS)
    systems = {}
    for row in hour_rows:
        item = systems.setdefault(row["system"], {"checks": 0, "failures": 0})
        item["checks"] += max(0, int(row.get("checks") or 0))
        item["failures"] += max(0, int(row.get("failures") or 0))
    # Legacy duplicate cron deliveries could record more than one check per
    # minute. A public day represents at most 1,440 minute opportunities, so
    # clamp historical overlap before the strict batch encoder validates it.
    for item in systems.values():
        item["checks"] = min(1440, item["checks"])
        item["failures"] = min(item["checks"], item["failures"])
    # Preserve an explicit zero-coverage marker for the same reason as the
    # hourly compactor: missing data is truthful, and must not block later
    # recoverable days in the bounded oldest-first queue.
    body = _status_batch_encode("day", systems)
    await d1_run(
        env,
        "INSERT INTO system_status_daily_batch (day_ts,data) VALUES (?,?) "
        "ON CONFLICT(day_ts) DO UPDATE SET data=excluded.data "
        "WHERE system_status_daily_batch.data!=excluded.data",
        target, body,
    )
    return target


def _status_mirror_name(row):
    """Return one safe, non-retired mirror name from a registry row."""
    mirror_name = str((row or {}).get("node_name") or "").strip().lower()
    if (
        not mirror_name.startswith("mirror")
        or mirror_name in STATUS_RETIRED_MIRRORS
        or not re.fullmatch(
            r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", mirror_name)
    ):
        return ""
    return mirror_name


async def _status_active_mirror_rows(env, now):
    """Return only Hosts-page mirrors with a current signed active proof.

    Historical node registrations are not an active-host roster. A deleted or
    stopped host can retain ``nodes.last_seen`` and old status samples for many
    hours, which used to keep it red on /status after it disappeared from the
    operator's Hosts page. The account-bound ``forkmesh_active`` proof is the
    server-side authority shared by live mirror routing; bound it to the same
    ten-minute freshness window as the health check.
    """
    cutoff = int(now) - HTTPS_MIRROR_STATUS_FRESH_MS
    rows = await d1_all(
        env,
        """SELECT node_name,checked_at,healthy,integrity,abuse_blocked,
                  forkmesh_active,forkmesh_verified_at
             FROM mirror_https_endpoints
            WHERE node_name GLOB 'mirror*'
              AND forkmesh_active=1 AND forkmesh_verified_at>=?
            ORDER BY node_name LIMIT ?""",
        cutoff, STATUS_MIRROR_MAX,
    )
    active = []
    seen = set()
    for row in rows or []:
        mirror_name = _status_mirror_name(row)
        if (
            not mirror_name
            or mirror_name in seen
            or int(row.get("forkmesh_active") or 0) != 1
            or int(row.get("forkmesh_verified_at") or 0) < cutoff
        ):
            continue
        seen.add(mirror_name)
        active.append(row)
    return active


def _status_core_name(value):
    core_name = str(value or "").strip().lower()
    if not re.fullmatch(r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?", core_name):
        return ""
    return core_name


async def _status_core_rows(env, now):
    rows = await d1_all(
        env,
        "SELECT node_id,status,heartbeat_ms FROM cluster_members "
        "WHERE heartbeat_ms>=? ORDER BY node_id LIMIT ?",
        int(now) - STATUS_CORE_ROSTER_FRESH_MS, STATUS_CORE_MAX,
    )
    cores = []
    seen = set()
    for row in rows or []:
        core_name = _status_core_name((row or {}).get("node_id"))
        if not core_name or core_name in seen:
            continue
        seen.add(core_name)
        cores.append(dict(row, node_id=core_name))
    return cores


def _status_core_vote_detail(row):
    try:
        detail = json.loads(str(row.get("detail") or ""))
    except Exception:
        detail = None
    healthy = int(row.get("healthy") or 0) == 1
    if not isinstance(detail, dict):
        return healthy, healthy
    return detail.get("app") is True, detail.get("node") is True


def _status_core_verdicts(core_rows, vote_rows, ids_rows, now):
    now = int(now)
    votes = {}
    for row in vote_rows or []:
        target = _status_core_name(row.get("target"))
        voter = _status_core_name(row.get("voter"))
        if not target or not voter:
            continue
        if int(row.get("observed_ms") or 0) < now - STATUS_CORE_VOTE_FRESH_MS:
            continue
        votes.setdefault(target, {})[voter] = _status_core_vote_detail(row)
    ids = {}
    for row in ids_rows or []:
        node = _status_core_name(row.get("node_id"))
        if node:
            ids[node] = row
    verdicts = {}
    for row in core_rows:
        core_name = row["node_id"]
        problems = []
        age_ms = now - int(row.get("heartbeat_ms") or 0)
        status = str(row.get("status") or "")
        if age_ms > STATUS_CORE_HEARTBEAT_FRESH_MS:
            problems.append(
                "has not written a cluster heartbeat for "
                + _flagship_monitor_duration(age_ms))
        elif status == "sealed":
            problems.append("is sealed and waiting for its vault key")
        elif status == "quarantined":
            problems.append("is quarantined by the intrusion detector")
        elif status == "leaving":
            problems.append("is leaving the cluster")
        elif status != "active":
            problems.append(
                "reports cluster status " + (clean_string(status, 40)
                                              or "unknown"))
        app_failed = sorted(
            voter for voter, (app_ok, _node_ok)
            in votes.get(core_name, {}).items() if not app_ok)
        node_failed = sorted(
            voter for voter, (_app_ok, node_ok)
            in votes.get(core_name, {}).items() if not node_ok)
        if app_failed:
            problems.append(
                "failed the app HTTPS readiness probe from "
                + ", ".join(app_failed))
        if node_failed:
            problems.append(
                "failed its node HTTPS readiness probe from "
                + ", ".join(node_failed))
        beat = ids.get(core_name)
        if beat:
            ids_age_ms = now - int(beat.get("beat_ms") or 0)
            if str(beat.get("state") or "") == "lockdown":
                problems.append("has its intrusion detector in lockdown")
            elif ids_age_ms > STATUS_CORE_IDS_FRESH_MS:
                problems.append(
                    "has no intrusion detector report for "
                    + _flagship_monitor_duration(ids_age_ms))
        verdicts[core_name] = (
            not problems,
            core_name + " " + "; ".join(problems) if problems else "")
    return verdicts


async def _status_core_health(env, now):
    core_rows = await _status_core_rows(env, now)
    if not core_rows:
        return {}
    vote_rows = await d1_all(
        env,
        "SELECT voter,target,healthy,observed_ms,detail "
        "FROM dns_health_votes WHERE observed_ms>=?",
        int(now) - STATUS_CORE_VOTE_FRESH_MS,
    )
    try:
        ids_rows = await d1_all(
            env, "SELECT node_id,beat_ms,state FROM ids_heartbeats")
    except Exception:
        ids_rows = []
    return _status_core_verdicts(core_rows, vote_rows, ids_rows, now)


async def _status_runner_rows(env):
    rows = await d1_all(
        env,
        "SELECT name,last_seen_at,a.data AS availability "
        "FROM runner_nodes LEFT JOIN runner_availability a "
        "ON a.runner=runner_nodes.name "
        "WHERE scope='pool' AND status='active' ORDER BY name LIMIT ?",
        STATUS_RUNNER_MAX,
    )
    runners = []
    seen = set()
    for row in rows or []:
        runner_name = _status_core_name((row or {}).get("name"))
        if not runner_name or runner_name in seen:
            continue
        seen.add(runner_name)
        runners.append(dict(row, name=runner_name))
    return runners


def _status_runner_verdicts(runner_rows, lease_rows, now):
    now = int(now)
    leases = {}
    for row in lease_rows or []:
        runner_name = _status_core_name((row or {}).get("runner"))
        if runner_name:
            leases[runner_name] = int((row or {}).get("n") or 0)
    verdicts = {}
    for row in runner_rows:
        runner_name = row["name"]
        seen_at = int(row.get("last_seen_at") or 0)
        if not seen_at:
            verdicts[runner_name] = (
                False, runner_name + " has never claimed or polled for jobs")
            continue
        age_ms = now - seen_at
        if age_ms > STATUS_RUNNER_SEEN_FRESH_MS:
            held = leases.get(runner_name, 0)
            verdicts[runner_name] = (
                False,
                runner_name + " has not claimed or polled for jobs for "
                + _flagship_monitor_duration(age_ms)
                + (" while holding " + str(held) + " job lease"
                   + ("" if held == 1 else "s") if held else ""))
            continue
        try:
            availability = json.loads(str(row.get("availability") or "{}"))
        except ValueError:
            availability = {}
        import runner_auth
        login_reason = runner_auth.status_reason(runner_name, availability)
        if login_reason:
            verdicts[runner_name] = (False, login_reason)
            continue
        verdicts[runner_name] = (True, "")
    return verdicts


async def _status_runner_health(env, now):
    runner_rows = await _status_runner_rows(env)
    if not runner_rows:
        return {}
    lease_rows = await d1_all(
        env,
        "SELECT runner,COUNT(*) AS n FROM runner_job_leases "
        "WHERE lease_id!='' AND expires_at>=? GROUP BY runner",
        int(now),
    )
    return _status_runner_verdicts(runner_rows, lease_rows, now)


async def _status_flagship_artifact_counts(env, now):
    """Return the source CAS count and each flagship mirror's signed count.

    The gateway catalog publisher derives ``artifactCount`` from physical
    ``sha256/<prefix>/<digest>/data`` objects beside the bare repository. The
    record is node-signed, so comparing it with the canonical source catches a
    mirror that has current Git refs but an empty or partial release store.
    Exact current-release bytes remain independently exercised by the installer
    deep probe; this parity check covers retained historical artifacts too.
    """
    rows = await _decrypted_public_catalog(env, int(now))
    target = None
    for row in rows or []:
        record = (row or {}).get("data") or {}
        if (
            str(record.get("owner") or "").strip().lower() == "forkmesh"
            and str(record.get("name") or "").strip().lower() == "forkmesh"
        ):
            target = record
            break
    if not target:
        return 0, {}

    def artifact_count(record):
        try:
            value = int((record or {}).get("artifactCount") or 0)
        except (TypeError, ValueError):
            return None
        return value if 0 <= value <= 1_000_000 else None

    expected = artifact_count(target)
    if expected is None:
        return 0, {}
    counts = {}
    for row in rows or []:
        record = (row or {}).get("data") or {}
        if not repo_mirror_same_group(target, record):
            continue
        node = clean_string(
            record.get("machineName") or record.get("owner") or "",
            MAX_NODE_NAME,
        ).strip().lower()
        count = artifact_count(record)
        if valid_node_name(node) and count is not None:
            counts[node] = count
    return expected, counts


async def _status_recent_system_ids(env, now):
    """Build deploy-grace samples from core systems plus active Hosts only."""
    systems = {system_id for system_id, _label in STATUS_SYSTEMS}
    try:
        for row in await _status_active_mirror_rows(env, now):
            systems.add(
                STATUS_MIRROR_PREFIX + _status_mirror_name(row))
    except Exception:
        # A roster query failure must not prevent neutral deploy coverage for
        # the fixed platform systems, and must never resurrect historical hosts.
        pass
    try:
        for row in await _status_core_rows(env, now):
            systems.add(STATUS_CORE_PREFIX + row["node_id"])
    except Exception:
        pass
    try:
        for row in await _status_runner_rows(env):
            systems.add(STATUS_RUNNER_PREFIX + row["name"])
    except Exception:
        pass
    return sorted(systems)


STATUS_ALERT_DELIVERY_BATCH = 1
STATUS_ALERT_CONTINUAL_BATCH = 16


_STATUS_SYNTHETIC_PROBE_MEMO = {}


def _status_probe_failure_confirmed(row):
    try:
        checked_at = int(row.get("checked_at") or 0)
        changed_at = int(row.get("changed_at") or 0)
    except (TypeError, ValueError):
        return True
    return checked_at > changed_at


def _status_probe_verdict(row):
    is_up = int(row.get("is_up") or 0) == 1
    if not is_up and not _status_probe_failure_confirmed(row):
        return True, ""
    return is_up, str(row.get("reason") or "")


async def _cached_synthetic_status_probe(
        env, monitor_id, now, probe, failure_prefix="Probe failed: "):
    """Reuse healthy synthetic checks while retrying a failure next minute.

    The minute sampler still evaluates real visitor errors and D1 health on
    every tick. Homepage assets, the World bootstrap graph, and the status-page
    assets are immutable between deployments, so fetching them sixty times per
    hour only multiplies Asset and external subrequests. The durable row makes
    the cache effective across CronRunner objects and isolate recycling.
    """
    # The durable row below is authoritative across objects; this per-isolate
    # copy just spares the three-per-minute point reads while it is fresh.
    memo = globals().get("_STATUS_SYNTHETIC_PROBE_MEMO")
    remembered = memo.get(monitor_id) if isinstance(memo, dict) else None
    if remembered:
        interval = (
            STATUS_SYNTHETIC_HEALTHY_INTERVAL_MS
            if remembered.get("is_up")
            else STATUS_SYNTHETIC_FAILED_RETRY_MS
        )
        if 0 <= int(now) - int(remembered.get("checked_at") or 0) < interval:
            return _status_probe_verdict(remembered)
    row = None
    row_known = True
    try:
        row = await d1_first(
            env,
            "SELECT is_up,reason,changed_at,checked_at "
            "FROM repository_monitor_state WHERE monitor_id=?",
            monitor_id,
        )
    except Exception:
        # The independent database monitor owns D1 availability. A cache read
        # failure must not prevent the direct synthetic check from running.
        row = None
        row_known = False
    if row:
        checked_at = max(0, int(row.get("checked_at") or 0))
        interval = (
            STATUS_SYNTHETIC_HEALTHY_INTERVAL_MS
            if int(row.get("is_up") or 0) == 1
            else STATUS_SYNTHETIC_FAILED_RETRY_MS
        )
        if checked_at and 0 <= int(now) - checked_at < interval:
            remembered = {
                "is_up": int(row.get("is_up") or 0) == 1,
                "reason": str(row.get("reason") or ""),
                "checked_at": checked_at,
                "changed_at": int(row.get("changed_at") or 0),
            }
            if isinstance(memo, dict):
                memo[monitor_id] = remembered
            return _status_probe_verdict(remembered)

    try:
        is_up, reason = await probe(env)
    except Exception as exc:
        is_up, reason = False, failure_prefix + str(exc)[:160]
    is_up = bool(is_up)
    reason = clean_string(reason or "", 240)
    changed_at = (
        int(row.get("changed_at") or 0)
        if row and (int(row.get("is_up") or 0) == 1) == is_up
        else int(now) if row_known else 0)
    try:
        await d1_run(
            env,
            """INSERT INTO repository_monitor_state
                 (monitor_id,is_up,changed_at,outage_started_at,checked_at,
                  reason,notified_state,pinged_state)
               VALUES (?,?,?,?,?,?,?,?)
               ON CONFLICT(monitor_id) DO UPDATE SET
                 is_up=excluded.is_up,
                 changed_at=CASE
                   WHEN repository_monitor_state.is_up!=excluded.is_up
                   THEN excluded.checked_at
                   ELSE repository_monitor_state.changed_at END,
                 outage_started_at=CASE
                   WHEN excluded.is_up=1 THEN 0
                   WHEN repository_monitor_state.is_up!=excluded.is_up
                   THEN excluded.checked_at
                   ELSE repository_monitor_state.outage_started_at END,
                 checked_at=excluded.checked_at,reason=excluded.reason""",
            monitor_id, 1 if is_up else 0, int(now),
            0 if is_up else int(now), int(now), reason, "", "",
        )
    except Exception:
        # The check itself is still valid for this minute. Failure to memoize
        # costs another probe next time but must not manufacture an outage.
        pass
    remembered = {
        "is_up": is_up, "reason": reason, "checked_at": int(now),
        "changed_at": changed_at}
    if isinstance(memo, dict):
        memo[monitor_id] = remembered
    return _status_probe_verdict(remembered)


def _release_version_tuple(tag):
    match = re.fullmatch(r"v?(\d+)\.(\d+)\.(\d+)", str(tag or "").strip())
    return tuple(int(part) for part in match.groups()) if match else None


async def _release_platform_probe(env, system_id, manifest_memo=None):
    _monitor_id, label, names = RELEASE_PLATFORM_MONITORS[system_id]
    no_mirror = "No reachable mirror served the release metadata"

    async def signed_manifests(channels, extra_paths=()):
        key = (tuple(channels), tuple(extra_paths))
        if manifest_memo is not None and key in manifest_memo:
            return manifest_memo[key]
        fetched = await fetch_signed_manifests(channels, extra_paths)
        if manifest_memo is not None and fetched is not None:
            manifest_memo[key] = fetched
        return fetched

    async def fetch_signed_manifests(channels, extra_paths=()):
        # One routed request per manifest set, the same blobs route the
        # installer probe uses. Returns (manifests by channel, extra blobs),
        # or None when no mirror answered.
        paths = list(extra_paths)
        for channel in channels:
            paths.append(".forkmesh/releases/" + channel + "/release.json")
            paths.append(".forkmesh/releases/" + channel + "/release.json.sig")
        response = await _routed_repository_read(
            env,
            "https://forkmesh.internal/api/repo/forkmesh/forkmesh/blobs?"
            + "&".join("path=" + quote(path) for path in paths),
            "blobs",
            bypass_cache=True,
            monitoring=True,
        )
        status = int(getattr(response, "status", 0) or 0)
        raw = str(await response.text())
        try:
            body = json.loads(raw) if len(raw) <= 2 * 1024 * 1024 else {}
        except Exception:
            body = {}
        blobs = body.get("blobs") if isinstance(body, dict) else None
        if status == 404:
            blobs = {}
        elif status != 200 or not isinstance(blobs, dict):
            return None

        def content(path):
            item = blobs.get(path)
            return item if isinstance(item, dict) else {}

        manifests = {}
        for channel in channels:
            root = ".forkmesh/releases/" + channel + "/"
            text = str(content(root + "release.json").get("content") or "")
            sig = content(root + "release.json.sig")
            try:
                if str(sig.get("encoding") or "") != "base64":
                    raise ValueError("signature is not base64")
                signature = base64.b64decode(
                    str(sig.get("content") or "").encode("ascii"),
                    validate=True)
                if len(signature) != 64 or not await ed25519_verify(
                        INSTALLER_RELEASE_PUBLISHER_PUBLIC_KEY,
                        base64.urlsafe_b64encode(signature).decode(
                            "ascii").rstrip("="),
                        text.encode("utf-8")):
                    raise ValueError("signature does not verify")
                manifest = json.loads(text)
            except Exception:
                manifests[channel] = None
                continue
            if (
                not isinstance(manifest, dict)
                or manifest.get("schema") != "forkmesh-release-v2"
                or manifest.get("repo") != "forkmesh/forkmesh"
                or not isinstance(manifest.get("assets"), list)
                or not isinstance(manifest.get("tag"), str)
            ):
                manifests[channel] = None
                continue
            manifests[channel] = manifest
        extras = {
            path: str(content(path).get("content") or "")
            for path in extra_paths}
        return manifests, extras

    fetched = await signed_manifests(("latest",), ("desktop/CMakeLists.txt",))
    if fetched is None:
        return False, no_mirror
    manifests, extras = fetched
    latest = manifests.get("latest")
    if latest is None:
        return False, (
            "The signed latest release manifest is missing or does not "
            "verify")
    release = latest

    version = re.search(
        r"project\(\s*ForkMesh\s+VERSION\s+(\d+\.\d+\.\d+)",
        extras.get("desktop/CMakeLists.txt", ""))
    newest = version.group(1) if version else ""
    latest_parts = _release_version_tuple(latest["tag"])
    if (
        newest
        and latest_parts is not None
        and _release_version_tuple(newest) > latest_parts
    ):
        fetched = await signed_manifests(("candidate-" + newest,))
        candidate = (
            fetched[0].get("candidate-" + newest) if fetched else None)
        if candidate is not None and candidate["tag"] == "v" + newest:
            release = candidate
    tag = release["tag"]

    assets = {}
    for asset in release["assets"]:
        if isinstance(asset, dict) and asset.get("name"):
            assets.setdefault(str(asset["name"]), asset)
    missing = []
    offered = {}
    for name in names:
        asset = assets.get(name) or {}
        digest = str(asset.get("blob_sha256") or "")
        try:
            size = int(asset.get("size") or 0)
        except (TypeError, ValueError):
            size = 0
        if not re.fullmatch(r"[0-9a-f]{64}", digest) or size <= 0:
            missing.append(name)
        else:
            offered.setdefault(digest, name)
    if missing:
        if release is latest:
            return False, "%s is not published in %s: %s" % (
                label, tag, ", ".join(missing))
        return False, "%s is released without its %s build: %s" % (
            tag, label, ", ".join(missing))

    for digest, name in offered.items():
        response = await _routed_repository_read(
            env,
            "https://forkmesh.internal/api/repo/forkmesh/forkmesh/"
            "releases/blob/sha256/" + digest,
            "release-blob",
            release_sha=digest,
            bypass_cache=True,
            monitoring=True,
            method="HEAD",
        )
        status = int(getattr(response, "status", 0) or 0)
        await _discard_response_body(response)
        if status == 404:
            return False, "%s download is unreachable: %s %s" % (
                label, name, tag)
        if status != 200:
            return False, "No reachable mirror served the %s download %s" % (
                label, name)
    return True, ""


async def _release_platform_status(
        env, now, system_id, allow_probe=True, force_probe=False,
        force_failed_probe=False, manifest_memo=None):
    monitor_id, label, _names = RELEASE_PLATFORM_MONITORS[system_id]
    row = await d1_first(
        env,
        "SELECT is_up,reason,changed_at,checked_at "
        "FROM repository_monitor_state WHERE monitor_id=?",
        monitor_id,
    )
    checked_at = int((row or {}).get("checked_at") or 0)
    interval = (
        RELEASE_PLATFORM_CHECK_INTERVAL_MS
        if allow_probe else RELEASE_PLATFORM_RESULT_STALE_MS)
    if (
        not force_probe
        and checked_at > 0
        and now - checked_at < interval
        and not (force_failed_probe and not bool(row.get("is_up")))
    ):
        if not allow_probe:
            return _status_probe_verdict(row)
        return bool(row.get("is_up")), str(row.get("reason") or "")
    if not allow_probe:
        if checked_at > 0:
            return False, (
                "The %s release probe did not complete within its "
                "monitoring interval" % label)
        return False, "The %s release probe has not completed" % label
    is_up, reason = await _release_platform_probe(
        env, system_id, manifest_memo=manifest_memo)
    reason = clean_string(reason or "", 240)
    await d1_run(
        env,
        """INSERT INTO repository_monitor_state
             (monitor_id,is_up,changed_at,outage_started_at,checked_at,reason,
              notified_state)
           VALUES (?,?,?,?,?,?,?)
           ON CONFLICT(monitor_id) DO UPDATE SET
             is_up=excluded.is_up,
             changed_at=CASE
               WHEN repository_monitor_state.is_up!=excluded.is_up
               THEN excluded.checked_at
               ELSE repository_monitor_state.changed_at END,
             checked_at=excluded.checked_at,
             reason=excluded.reason""",
        monitor_id, 1 if is_up else 0, now,
        0 if is_up else now, now, reason, "",
    )
    return is_up, reason


async def _probe_release_platforms(env, now):
    failures = []
    manifest_memo = {}
    for system_id, (_monitor_id, label, _names) in (
            RELEASE_PLATFORM_MONITORS.items()):
        try:
            await _release_platform_status(
                env, now, system_id, allow_probe=True, force_probe=True,
                manifest_memo=manifest_memo)
        except Exception as exc:
            failures.append("%s: %s" % (label, str(exc)[:160]))
    if failures:
        raise RuntimeError(
            "Release platform probes failed: " + "; ".join(failures))


# Per-isolate copy of the status:* transition rows. Steady state — every
# system green and already announced — is the normal minute, and re-reading
# all rows for it 1,728 times a day was the second-heaviest status query.
# Looked up through globals() so the AST test harnesses keep the plain read.
_STATUS_MONITOR_STATE_MEMO = {"ts": 0, "ids": (), "rows": None}
STATUS_MONITOR_STATE_MEMO_TTL_MS = 10 * 60 * 1000
STATUS_MONITOR_STATE_IN_LIST_MAX = 90


async def _status_monitor_prior_rows(env, monitor_ids, now, force=False):
    """Stored transition rows for ``monitor_ids`` -> ``(rows, from_memo)``.

    The memo is only trusted while nothing is about to happen: the caller
    re-reads with ``force=True`` before acting on a transition or an
    undelivered alert, and forgets the memo after a delivery moved a
    notified/pinged marker. The exact IN-list replaces the old
    ``LIKE 'status:%'`` prefix scan, which walked every monitor row
    (including retired mirrors and the non-status monitors) on each call.
    """
    wanted = [str(monitor_id) for monitor_id in monitor_ids]
    memo = globals().get("_STATUS_MONITOR_STATE_MEMO")
    if (
        not force
        and isinstance(memo, dict)
        and isinstance(memo.get("rows"), dict)
        and 0 <= int(now) - int(memo.get("ts") or 0)
        < STATUS_MONITOR_STATE_MEMO_TTL_MS
        and set(wanted) <= set(memo.get("ids") or ())
    ):
        return (
            {key: row for key, row in memo["rows"].items() if key in wanted},
            True,
        )
    columns = (
        "SELECT monitor_id,is_up,changed_at,outage_started_at,notified_state,"
        "pinged_state,reason FROM repository_monitor_state ")
    if wanted and len(wanted) <= STATUS_MONITOR_STATE_IN_LIST_MAX:
        rows = await d1_all(
            env,
            columns + "WHERE monitor_id IN ("
            + ",".join("?" * len(wanted)) + ")",
            *wanted,
        )
    else:
        rows = await d1_all(
            env, columns + "WHERE monitor_id LIKE 'status:%'")
    prior = {str(row.get("monitor_id") or ""): row for row in (rows or [])}
    if isinstance(memo, dict):
        memo["rows"] = dict(prior)
        memo["ids"] = tuple(wanted)
        memo["ts"] = int(now)
    return prior, False


def _status_monitor_row_changed(prior_row, state_row):
    """Python twin of the recorder's ON CONFLICT ... WHERE change gate."""
    if not prior_row:
        return True
    return (
        (1 if prior_row.get("is_up") else 0) != int(state_row["isUp"])
        or int(prior_row.get("changed_at") or 0) != int(state_row["changedAt"])
        or int(prior_row.get("outage_started_at") or 0)
        != int(state_row["outageStartedAt"])
        or str(prior_row.get("reason") or "") != str(state_row["reason"] or "")
        or str(prior_row.get("notified_state") or "")
        != str(state_row["notifiedState"] or "")
        or str(prior_row.get("pinged_state") or "")
        != str(state_row["pingedState"] or "")
    )


def _status_monitor_rows_remember(state_rows, invalidate=False):
    """Fold the rows just written into the memo, or forget it before delivery."""
    memo = globals().get("_STATUS_MONITOR_STATE_MEMO")
    if not isinstance(memo, dict):
        return
    if invalidate or not isinstance(memo.get("rows"), dict):
        # The delivery path moves notified/pinged markers with its own UPDATE
        # statements; the next minute must read them back.
        memo["rows"] = None
        return
    for row in state_rows:
        memo["rows"][str(row["monitorId"])] = {
            "monitor_id": row["monitorId"],
            "is_up": int(row["isUp"]),
            "changed_at": int(row["changedAt"]),
            "outage_started_at": int(row["outageStartedAt"]),
            "notified_state": row["notifiedState"],
            "pinged_state": row["pingedState"],
            "reason": row["reason"],
        }


async def _record_status_monitor_transitions(
        env, ok, reason, now, status_systems=None, delivery_channel="both"):
    """Deduplicate outage/recovery mail for every system shown on /status."""
    status_systems = status_systems or STATUS_SYSTEMS
    monitor_ids = [
        "status:" + system_id for system_id, _label in status_systems]
    prior, from_memo = await _status_monitor_prior_rows(
        env, monitor_ids, now)
    alert_owner, _, alert_repo = FLAGSHIP_MONITOR_ID.partition("/")
    alert_settings = await _repo_alert_settings_get(
        env, alert_owner, alert_repo)

    def evaluate(prior):
        pending = []
        ping_pending = []
        state_rows = []
        transitioned = False
        for system_id, label in status_systems:
            monitor_id = "status:" + system_id
            is_up = bool(ok.get(system_id, True))
            state = "up" if is_up else "down"
            row = prior.get(monitor_id)
            previous_up = bool(row.get("is_up")) if row else True
            previous_changed_at = (
                int(row.get("changed_at") or now) if row else int(now))
            prior_outage = (
                int(row.get("outage_started_at") or 0) if row else 0)
            prior_notified = (
                str(row.get("notified_state") or "") if row else "")
            prior_pinged = (
                str(row.get("pinged_state") or "") if row else "")
            changed = not row or previous_up != is_up
            transitioned = transitioned or changed
            changed_at = int(now) if changed else previous_changed_at
            outage_started_at = (
                0 if is_up else int(now) if not row or previous_up
                else prior_outage or previous_changed_at)
            notified = (
                ("up" if is_up else "") if not row else
                "" if previous_up != is_up else
                prior_notified)
            pinged = (
                ("up" if is_up else "") if not row else
                "" if previous_up != is_up else
                prior_pinged)
            should_notify = notified != state
            should_ping = pinged != state
            continual_ping = (
                not is_up
                and _status_monitor_alert_setting(
                    alert_settings, system_id, "continual")
                and int(now) // STATUS_ALERT_CONTINUAL_INTERVAL_MS !=
                    (int(now) - STATUS_SAMPLE_WINDOW_MS) //
                    STATUS_ALERT_CONTINUAL_INTERVAL_MS
            )
            should_ping = should_ping or continual_ping
            if system_id == "flagship_repository" and is_up and row and not previous_up:
                # A recovery is only useful on a channel that announced the
                # outage. Deployment handoffs are already made green before they
                # reach this transition recorder, so do not apply a second
                # five-minute grace here: it hid genuine short repository outages
                # and their Pings.
                if prior_notified != "down":
                    notified = "up"
                    should_notify = False
                if prior_pinged != "down":
                    pinged = "up"
                    should_ping = False
            if should_notify or should_ping:
                alert = {
                    "system": system_id, "label": label, "state": state,
                    "is_up": is_up, "reason": clean_string(
                        reason.get(system_id, "") or "", 240),
                    "outage_started_at": outage_started_at,
                    "previous_changed_at": previous_changed_at,
                    "changed_at": changed_at,
                    # The last failure this system recorded. On a recovery the
                    # current sample has no reason of its own, so this is the only
                    # description of what actually went wrong.
                    "prior_reason": clean_string(
                        (str(row.get("reason") or "") if row else ""), 240),
                    # The outage this sample closed, on the flip itself and never
                    # on a later re-send of an undelivered recovery.
                    "recovered_outage_started_at": (
                        (prior_outage or previous_changed_at)
                        if is_up and row and not previous_up else 0),
                    "continual": bool(continual_ping),
                }
                if should_notify:
                    pending.append(alert)
                if should_ping:
                    ping_pending.append(alert)
            state_rows.append({
                "monitorId": monitor_id,
                "isUp": 1 if is_up else 0,
                "changedAt": changed_at,
                "outageStartedAt": outage_started_at,
                "checkedAt": int(now),
                "reason": clean_string(reason.get(system_id, "") or "", 240),
                "notifiedState": notified,
                "pingedState": pinged,
            })

        return pending, ping_pending, state_rows, transitioned

    def deliverable(pending, ping_pending):
        # What this invocation would actually send. Alerts the channel filter
        # or the per-system email switch drop are not deliveries: with alert
        # mail off, every system whose notified_state is still blank shows up
        # in ``pending`` on every minute, and that must not cost a read.
        emails = [] if delivery_channel == "ping" else [
            alert for alert in pending
            if _status_monitor_alert_setting(
                alert_settings, alert.get("system", ""), "emails")]
        pings = [] if delivery_channel == "email" else list(ping_pending)
        return bool(emails or pings)

    def changed(prior, state_rows):
        return [
            row for row in state_rows
            if _status_monitor_row_changed(prior.get(row["monitorId"]), row)
        ]

    pending, ping_pending, state_rows, transitioned = evaluate(prior)
    changed_rows = changed(prior, state_rows)
    if from_memo and (changed_rows or deliverable(pending, ping_pending)):
        # Act only on authoritative rows: another object may have recorded or
        # announced this transition since the memo was filled.
        prior, from_memo = await _status_monitor_prior_rows(
            env, monitor_ids, now, force=True)
        pending, ping_pending, state_rows, transitioned = evaluate(prior)
        changed_rows = changed(prior, state_rows)

    # One JSON bind keeps the supported 61-system roster to one D1 statement,
    # instead of seven statements (or exceeding D1's 100-bind ceiling). JSON1
    # is already required by this schema's bounded batch tables. Only rows the
    # Python diff above found different are sent: the ON CONFLICT ... WHERE
    # gate below already skipped identical writes, but json_each still read
    # and probed every row, ~20 rows read per minute for an all-green fleet.
    if changed_rows:
        await d1_run(
            env,
            "INSERT INTO repository_monitor_state "
            "(monitor_id,is_up,changed_at,outage_started_at,checked_at,reason,"
            "notified_state,pinged_state) "
            "SELECT json_extract(value,'$.monitorId'),"
            "CAST(json_extract(value,'$.isUp') AS INTEGER),"
            "CAST(json_extract(value,'$.changedAt') AS INTEGER),"
            "CAST(json_extract(value,'$.outageStartedAt') AS INTEGER),"
            "CAST(json_extract(value,'$.checkedAt') AS INTEGER),"
            "json_extract(value,'$.reason'),"
            "json_extract(value,'$.notifiedState'),"
            "json_extract(value,'$.pingedState') FROM json_each(?) WHERE 1 "
            "ON CONFLICT(monitor_id) DO UPDATE SET "
            "is_up=excluded.is_up,changed_at=excluded.changed_at,"
            "outage_started_at=excluded.outage_started_at,"
            # status:* rows are transition state, not a heartbeat. Keeping
            # checked_at unchanged and skipping identical conflicts turns the
            # normal all-green minute from N changed rows into zero.
            "reason=excluded.reason,"
            "notified_state=excluded.notified_state,"
            "pinged_state=excluded.pinged_state "
            "WHERE repository_monitor_state.is_up!=excluded.is_up "
            "OR repository_monitor_state.changed_at!=excluded.changed_at "
            "OR repository_monitor_state.outage_started_at"
            "!=excluded.outage_started_at "
            "OR repository_monitor_state.reason IS NOT excluded.reason "
            "OR repository_monitor_state.notified_state"
            "!=excluded.notified_state "
            "OR repository_monitor_state.pinged_state"
            "!=excluded.pinged_state",
            json.dumps(changed_rows, sort_keys=True, separators=(",", ":")),
        )
    # A delivery below moves notified/pinged markers with its own UPDATEs, so
    # the memo is forgotten whenever one is attempted; otherwise it now holds
    # exactly what the table holds.
    _status_monitor_rows_remember(
        state_rows, invalidate=deliverable(pending, ping_pending))
    # A fleet-wide transition can discover more alerts than one Free-plan
    # invocation may safely deliver.  Persist every state above, then drain a
    # deterministic bounded prefix; rows whose ping/mail marker is still blank
    # are picked up by the next minute's dedicated alert phase.
    if delivery_channel == "ping":
        pending = []
    elif delivery_channel == "email":
        ping_pending = []
    ping_pending = (
        [alert for alert in ping_pending if not alert["continual"]]
        [:STATUS_ALERT_DELIVERY_BATCH]
        + [alert for alert in ping_pending if alert["continual"]]
        [:STATUS_ALERT_CONTINUAL_BATCH])
    pending = pending[:STATUS_ALERT_DELIVERY_BATCH]
    if ping_pending:
        try:
            if await _enqueue_operational_alert_pings(env, ping_pending, now):
                # Same rule as the mail path: only a delivered transition is
                # recorded, so a deployment with Pings switched off still gets
                # one alert for whatever is red when it switches them on.
                await _record_operational_alert_pings_sent(env, ping_pending)
        except BaseException:
            # Pings are a secondary delivery channel and must never block status
            # sampling or the independently configured email path.
            pass
    if not pending:
        return transitioned
    # notified_state is deliberately left untouched while alert mail is off:
    # whatever is red when an admin turns it on gets one email then, instead
    # of the switch silently swallowing the transition that is still current.
    pending = [
        alert for alert in pending
        if _status_monitor_alert_setting(
            alert_settings, alert.get("system", ""), "emails")
    ]
    if not pending:
        return transitioned
    recipients = await _repository_monitor_admin_emails(env)
    if not recipients:
        return transitioned
    attention_log_tail = None
    if any(not alert["is_up"] for alert in pending):
        attention_log_tail = await _cloudflare_attention_log_tail(env, now)
    for alert in pending:
        system_id = alert["system"]
        label = alert["label"]
        if system_id.startswith(STATUS_MIRROR_PREFIX):
            mirror_name = system_id[len(STATUS_MIRROR_PREFIX):]
            where, fix = (
                "the signed direct-HTTPS health record for " + mirror_name,
                "Check the node process and tunnel, then confirm its signed "
                "health proof is fresh, integrity is ok, and forkmesh/forkmesh "
                "matches an accepted source revision.",
            )
        elif system_id.startswith(STATUS_CORE_PREFIX):
            core_name = system_id[len(STATUS_CORE_PREFIX):]
            where, fix = (
                "cluster_members, dns_health_votes and ids_heartbeats for "
                + core_name + ", and its journal",
                "Run tools/next_ids.py status, then check forkmesh-app, "
                "rqlite and /readyz on " + core_name + "; unseal it if it is "
                "sealed.",
            )
        elif system_id.startswith(STATUS_RUNNER_PREFIX):
            runner_name = system_id[len(STATUS_RUNNER_PREFIX):]
            where, fix = (
                "runner_nodes and runner_job_leases for " + runner_name
                + ", and its forkmesh-runner journal",
                "Check that " + runner_name + " is up and its runner daemon "
                "is running and can reach the app; revoke it if it was "
                "scaled down.",
            )
        elif system_id == "email" and _mail_over_smtp(env):
            where, fix = STATUS_EMAIL_SMTP_GUIDANCE
        else:
            where, fix = STATUS_MONITOR_GUIDANCE.get(
                system_id,
                ("Cloudflare Worker logs", "Inspect the failing check."))
        if alert["is_up"]:
            duration = _flagship_monitor_duration(
                int(now) - (
                    alert["outage_started_at"] or
                    alert["previous_changed_at"]))
            subject = "[ForkMesh recovered] " + label + " is green"
            lead = label + " is passing again after " + duration + "."
            text = (
                lead + "\n\nThe current /status probe is green.\n\n"
                "Status: https://forkmesh.com/status")
            heading = label + " recovered"
            body = (
                "<div class=\"fm-item\" style=\"border:1px solid #d4d4d8;"
                "border-radius:8px;background:#fafafa;padding:14px;"
                "margin:0 0 18px\"><p class=\"fm-item-title\" style=\"margin:0;"
                "color:#18181b;font-size:14px;font-weight:800\">Outage "
                "duration</p><p class=\"fm-item-body\" style=\"margin:6px 0 0;"
                "color:#3f3f46;font-size:14px\">" +
                _html_escape(duration) + "</p></div>")
        else:
            observed = alert["reason"] or "The current health check failed."
            subject = "[ForkMesh outage] " + label + " is not green"
            lead = label + " failed its /status health check."
            text = (
                lead + "\n\nObserved: " + observed + "\n\nLook here: " +
                where + "\n\nSuggested first step: " + fix +
                "\n\nA recovery email will be sent when this check is green "
                "again.\n\nStatus: https://forkmesh.com/status")
            heading = label + " needs attention"
            body = (
                "<div class=\"fm-item\" style=\"border:1px solid #d4d4d8;"
                "border-radius:8px;background:#fafafa;padding:14px;"
                "margin:0 0 12px\"><p class=\"fm-item-title\" style=\"margin:0;"
                "color:#18181b;font-size:14px;font-weight:800\">Observed</p>"
                "<p class=\"fm-item-body\" style=\"margin:6px 0 0;color:#3f3f46;"
                "font-size:14px\">" + _html_escape(observed) + "</p></div>"
                "<p class=\"fm-text\" style=\"margin:0 0 8px;color:#3f3f46;"
                "font-size:14px\"><strong>Look here:</strong> " +
                _html_escape(where) + "</p><p class=\"fm-text\" style=\"margin:"
                "0 0 18px;color:#3f3f46;font-size:14px\"><strong>Suggested "
                "first step:</strong> " + _html_escape(fix) + "</p>")
        html = _forkmesh_email_card_html(
            _html_escape(heading), _html_escape(lead), body,
            "<p class=\"fm-muted\" style=\"margin:20px 0 0;color:#71717a;"
            "font-size:12px\"><a class=\"fm-link\" style=\"color:#15803d\" "
            "href=\"https://forkmesh.com/status\">Open ForkMesh status</a>"
            "</p>")
        if not alert["is_up"]:
            text, html = _attention_email_with_logs(
                text, html, attention_log_tail)
        text, html = _email_with_status_alert_manage_link(
            env, text, html, system_id)
        delivered = True
        for email in recipients:
            delivered = bool(await _send_email(
                env, email, subject, text, html,
                email_kind="status_alert")) and delivered
        if delivered:
            await d1_run(
                env,
                "UPDATE repository_monitor_state SET notified_state=? "
                "WHERE monitor_id=? AND is_up=?",
                alert["state"], "status:" + system_id,
                1 if alert["is_up"] else 0,
            )
    return transitioned


# Must match api_metrics.FLAGSHIP_ROUTE_GROUP: a failed mirror-routed
# forkmesh/forkmesh content read (/api/repo/forkmesh/forkmesh/
# tree|blob|blobs|raw|history|commit|compare|branches|search|stats|sizes and
# release blobs) under this exact label is counted in
# flagship_read_failure_minute.
FLAGSHIP_PUBLIC_READ_ROUTE_GROUP = "repo/forkmesh/forkmesh/content"
# Failed content reads in one minute before the visitor evidence alone calls
# the repository down. One stray timeout is not "the repository is not
# accessible"; a page load during an outage fails several reads at once.
# api_metrics writes this many flagship failures per isolate-minute
# (FLAGSHIP_FAILURE_WRITES), so the threshold is observable at the next tick.
FLAGSHIP_PUBLIC_READ_FAILURE_THRESHOLD = 2
# Event buckets describe one minute's errors ("something failed during this
# minute"); they hold no state to carry across a deployment window. Every other
# row is a probe/state reading that stays true until it is re-checked.
STATUS_EVENT_SYSTEMS = frozenset((
    "api", "errors", "realtime", "durable_objects"))


async def _flagship_public_read_failures(env, now):
    """Visitor-side evidence that forkmesh/forkmesh is not loading right now.

    Returns ``(failures, reason)``: how many HTTP 5xx responses the Worker
    served to forkmesh/forkmesh content reads (the mirror-routed tree, blob,
    raw, history, ... and release-blob routes the repository page renders
    from) during the previous sample minute, read from
    flagship_read_failure_minute (api_metrics writes a failure as it happens,
    so this reader sees it at the next tick). A 503 there is the mirror router
    answering that no eligible mirror could serve the read — exactly what a
    person on the repository page sees as an error — so it is an outage
    signal in its own right, independent of when the deep probe last
    ran. Served (non-5xx) responses are not counted at all: an edge-cached
    tree listing still answers while every mirror is down, so they prove
    nothing about accessibility. The count is a floor: each isolate writes at
    most FLAGSHIP_FAILURE_WRITES failures a minute. Fewer than
    FLAGSHIP_PUBLIC_READ_FAILURE_THRESHOLD failures answer ``(0, "")``.
    """
    minute_ts = (int(now) // STATUS_SAMPLE_WINDOW_MS) * STATUS_SAMPLE_WINDOW_MS
    row = await d1_first(
        env,
        "SELECT failures FROM flagship_read_failure_minute WHERE minute_ts=?",
        minute_ts - STATUS_SAMPLE_WINDOW_MS,
    )
    failures = max(0, int((row or {}).get("failures") or 0))
    if failures < FLAGSHIP_PUBLIC_READ_FAILURE_THRESHOLD:
        return 0, ""
    return failures, (
        "At least %d visitor read%s of forkmesh/forkmesh repository content "
        "failed with HTTP 5xx in the last minute" % (
            failures, "" if failures == 1 else "s"))


async def _status_carried_failures(env, minute_ts):
    """State rows that were failing in the minute before ``minute_ts``.

    Read from the one physical minute batch; decoded with the same poisoned-row
    guard the public projection uses, so a bad row carries nothing rather than
    something invented. Event buckets are excluded (see STATUS_EVENT_SYSTEMS).
    """
    row = await d1_first(
        env,
        "SELECT data FROM system_status_minute_batch WHERE minute_ts=?",
        int(minute_ts) - STATUS_SAMPLE_WINDOW_MS,
    )
    decoded = (
        _status_batch_decode((row or {}).get("data"), "minute")
        if row else {}
    )
    return {
        system_id: (item.get("reason") or None)
        for system_id, item in decoded.items()
        if not item.get("ok") and system_id not in STATUS_EVENT_SYSTEMS
    }


async def _status_deploy_recovered_systems(env, now):
    """Read durable recoveries without running probes during deploy grace.

    A rollout carries existing failures so deployment work cannot paint a real
    outage green. The inverse also matters: a separately scheduled deep probe
    or signed mirror lease can recover while that grace is active. Those
    durable, fresh success records are stronger than the previous minute and
    must stop being carried immediately.
    """
    recovered = set()
    try:
        rows = await d1_all(
            env,
            "SELECT monitor_id,is_up,checked_at FROM repository_monitor_state "
            "WHERE monitor_id IN (?,?,?,?,?)",
            FLAGSHIP_DEEP_MONITOR_ID,
            *(monitor[0] for monitor in RELEASE_PLATFORM_MONITORS.values()),
        )
        policies = {
            FLAGSHIP_DEEP_MONITOR_ID: (
                "flagship_repository", FLAGSHIP_RESULT_STALE_MS),
        }
        for system_id, monitor in RELEASE_PLATFORM_MONITORS.items():
            policies[monitor[0]] = (
                system_id, RELEASE_PLATFORM_RESULT_STALE_MS)
        for row in rows or []:
            policy = policies.get(str(row.get("monitor_id") or ""))
            checked_at = max(0, int(row.get("checked_at") or 0))
            if (
                policy
                and int(row.get("is_up") or 0) == 1
                and checked_at >= int(now) - policy[1]
            ):
                recovered.add(policy[0])
    except Exception:
        pass
    try:
        cutoff = int(now) - HTTPS_MIRROR_STATUS_FRESH_MS
        row = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM mirror_https_endpoints "
            "WHERE checked_at>=? AND forkmesh_verified_at>=? "
            "AND healthy=1 AND forkmesh_active=1 "
            "AND integrity='ok' AND abuse_blocked=0",
            cutoff,
            cutoff,
        )
        if int((row or {}).get("n", 0) or 0) > 0:
            recovered.add("git_hosting")
    except Exception:
        pass
    return recovered


async def _record_status_deploy_sample(env, now, run_compaction=True):
    """Record a maintenance minute without probing or alerting.

    Missing cron samples are deliberately shown as down on /status, so simply
    returning while a deploy semaphore is set would still manufacture a red
    minute. Record one sample for every currently known status row instead,
    without running the probes a rollout handoff can disturb. We intentionally
    do not call the transition mailer: rollout handoffs are neither incidents
    nor recoveries.

    "Neither incidents nor recoveries" cuts both ways. A state row that was
    already failing going into the rollout keeps failing through it, with its
    last reason, until the first real sample afterwards re-checks it. Recording
    every row as healthy here used to paint the whole deploy window green in
    the middle of a genuine outage: on 2026-08-19 the flagship repository was
    unreachable for thirteen minutes while /status stayed "operational",
    because two rollouts landed on top of the failure (adhoc #68). Event
    buckets are not carried — they describe one minute's errors, not a state.
    Visitor-side flagship evidence is still read: people failing to load the
    repository is an outage whatever the deploy clock says.
    """
    hour_ts = (now // STATUS_HOUR_MS) * STATUS_HOUR_MS
    minute_ts = (now // STATUS_SAMPLE_WINDOW_MS) * STATUS_SAMPLE_WINDOW_MS
    try:
        systems = await _status_recent_system_ids(env, now)
    except Exception:
        systems = [system_id for system_id, _label in STATUS_SYSTEMS]
    try:
        carried = await _status_carried_failures(env, minute_ts)
    except Exception:
        # A read hiccup must not invent an outage; it merely cannot preserve
        # one, which the first real sample after the rollout corrects.
        carried = {}
    try:
        recovered = await _status_deploy_recovered_systems(env, now)
    except Exception:
        recovered = set()
    try:
        observed_failures, observed_reason = (
            await _flagship_public_read_failures(env, now))
    except Exception:
        observed_failures, observed_reason = 0, ""
    minute_systems = {}
    for system_id in systems:
        if system_id in carried and system_id not in recovered:
            minute_systems[system_id] = {
                "ok": False,
                "reason": clean_string(carried[system_id] or "", 240) or None,
            }
        else:
            minute_systems[system_id] = {"ok": True, "reason": None}
    if observed_failures and "flagship_repository" in minute_systems:
        minute_systems["flagship_repository"] = {
            "ok": False,
            "reason": clean_string(observed_reason, 240) or None,
        }
    await _write_status_minute_batch(env, minute_ts, minute_systems)
    if run_compaction:
        # Direct callers retain the historical one-call behavior. CronRunner
        # uses the separately budgeted compaction phase.
        await _compact_status_hour(env, hour_ts, now)
        await _compact_status_day(
            env, (now // STATUS_DAY_MS) * STATUS_DAY_MS, now)


async def record_status_sample(
        env, source="runner", allow_deep_probes=True, deliver_alerts=True,
        run_compaction=True):
    await ensure_schema(env)
    now = int(Date.now())
    if not await _claim_status_sample_minute(env, now, source):
        return
    if await _status_deploy_semaphore_active(env, now):
        await _record_status_deploy_sample(
            env, now, run_compaction=run_compaction)
        return
    minute_ts = (now // 60000) * 60000
    ok = {}
    reason = {}

    try:
        website_ok, website_reason = await _cached_synthetic_status_probe(
            env, STATUS_HOMEPAGE_MONITOR_ID, now, _homepage_status_probe,
            "Homepage probe failed: ")
        ok["website"] = website_ok
        if not website_ok:
            reason["website"] = website_reason
    except Exception as exc:
        ok["website"] = False
        reason["website"] = "Homepage probe failed: " + str(exc)[:160]

    try:
        world_ok, world_reason = await _cached_synthetic_status_probe(
            env, STATUS_WORLD_MOBILE_MONITOR_ID, now,
            _mobile_world_status_probe, "Mobile World probe failed: ")
        ok["world_mobile"] = world_ok
        if not world_ok:
            reason["world_mobile"] = world_reason
    except Exception as exc:
        ok["world_mobile"] = False
        reason["world_mobile"] = (
            "Mobile World probe failed: " + str(exc)[:160])

    try:
        status_page_ok, status_page_reason = await _cached_synthetic_status_probe(
            env, STATUS_PAGE_MONITOR_ID, now, _status_page_api_probe,
            "Status API probe failed: ")
        ok["status_page"] = status_page_ok
        if not status_page_ok:
            reason["status_page"] = status_page_reason
    except Exception as exc:
        ok["status_page"] = False
        reason["status_page"] = "Status API probe failed: " + str(exc)[:160]

    try:
        await d1_first(env, "SELECT 1 AS ok")
        ok["database"] = True
    except Exception as exc:
        ok["database"] = False
        reason["database"] = "Database query failed: " + str(exc)[:160]

    try:
        email_ok, email_reason = await _email_delivery_status(env, now)
        ok["email"] = email_ok
        if not email_ok:
            reason["email"] = email_reason
    except Exception as exc:
        ok["email"] = False
        reason["email"] = (
            "Email delivery health query failed: " + str(exc)[:160])

    # Two independent signals feed the flagship row. The deep
    # probe is the authoritative, traffic-independent check. Visitor evidence
    # is what people actually got from the repository's mirror-routed content
    # reads (/api/repo/forkmesh/forkmesh/tree, blob, raw, ...) in the last
    # minute: any HTTP 5xx there means someone could not load the
    # repository page, and it turns the row red in the minute after the first
    # failure instead of waiting for the next scheduled probe. A mirror outage
    # that began right after a probe could otherwise stay green until it ran,
    # and then for as long as deploy handoffs kept landing on top of it
    # (adhoc #68).
    try:
        observed_failures, observed_reason = (
            await _flagship_public_read_failures(env, now))
    except Exception:
        # A diagnostics read hiccup is not evidence either way.
        observed_failures, observed_reason = 0, ""
    try:
        repository_ok, repository_reason = await _flagship_repository_status(
            env, now, allow_probe=allow_deep_probes)
        if (
            not repository_ok
            and not observed_failures
            and _deployment_status_grace_active(env, now)
        ):
            # Worker/static activation can briefly interrupt the probe's own
            # routed reads. That handoff is forgiven only while no visitor is
            # failing: people seeing errors is an outage whatever the deploy
            # clock says.
            repository_ok = True
            repository_reason = ""
    except Exception as exc:
        if not observed_failures and _deployment_status_grace_active(env, now):
            repository_ok, repository_reason = True, ""
        else:
            repository_ok = False
            repository_reason = (
                "Repository availability probe failed: " + str(exc)[:160])
    if observed_failures:
        repository_ok = False
        repository_reason = (
            repository_reason + "; " + observed_reason[:1].lower()
            + observed_reason[1:]
            if repository_reason else observed_reason)
    ok["flagship_repository"] = repository_ok
    if not repository_ok:
        reason["flagship_repository"] = repository_reason

    for system_id, (_monitor_id, label, _names) in (
            RELEASE_PLATFORM_MONITORS.items()):
        try:
            release_ok, release_reason = await _release_platform_status(
                env, now, system_id, allow_probe=allow_deep_probes)
            ok[system_id] = release_ok
            if not release_ok:
                reason[system_id] = release_reason
        except Exception as exc:
            ok[system_id] = False
            reason[system_id] = (
                "%s release probe failed: %s" % (label, str(exc)[:160]))

    try:
        cutoff = now - HTTPS_MIRROR_STATUS_FRESH_MS
        row = await d1_first(
            env,
            "SELECT COUNT(*) AS n FROM mirror_https_endpoints "
            "WHERE checked_at>=? AND forkmesh_verified_at>=? "
            "AND healthy=1 AND forkmesh_active=1 "
            "AND integrity='ok' AND abuse_blocked=0",
            cutoff, cutoff,
        )
        ok["git_hosting"] = int((row or {}).get("n", 0) or 0) > 0
        if not ok["git_hosting"]:
            reason["git_hosting"] = (
                "No healthy direct HTTPS mirror has supplied a valid ForkMesh "
                "repository proof within the last 10 minutes")
    except Exception as exc:
        ok["git_hosting"] = False
        reason["git_hosting"] = (
            "Direct HTTPS mirror health query failed: " + str(exc)[:160])

    try:
        rows = await d1_all(
            env,
            "SELECT method, path, status, message FROM error_log "
            "WHERE ts >= ?",
            now - STATUS_SAMPLE_WINDOW_MS,
        )
        buckets = (
            "website", "api", "errors", "realtime", "durable_objects")
        failed = {bucket: False for bucket in buckets}
        first_hit = {bucket: None for bucket in buckets}
        hit_count = {bucket: 0 for bucket in buckets}
        for row in rows:
            path = str(row.get("path") or "")
            method = str(row.get("method") or "").strip().upper()
            message = str(row.get("message") or "")
            # error_log is shared by three sources, classified the same way the
            # admin console does it (_admin_error_source): Worker faults, browser
            # reports posted to /client-error/* as method JS/520, and Qt desktop
            # reports posted to /desktop-error/* as method APP/521. Every bucket
            # below is a Worker-side signal — "Website" covers Worker page-route
            # errors and "errors" is literally labelled "Worker errors" — so a
            # client reporting its own trouble must never redden them. One
            # desktop node showing an ordinary informational notice used to paint
            # both rows red on /status, because /desktop-error/<surface> is not
            # under /api/ and so landed in the website bucket (adhoc #1733).
            if (
                method in ("APP", "JS")
                or path.startswith("/desktop-error/")
                or path.startswith("/client-error/")
            ):
                continue
            try:
                row_status = int(row.get("status") or 0)
            except (TypeError, ValueError):
                row_status = 0
            # A DO duration-cap abort is a real room failure, so it still
            # counts toward "realtime" below — but it also gets its own
            # bucket so free-tier plan-limit aborts have a dedicated, visible
            # history instead of being buried among other realtime incidents.
            # Matches legacy tagged 503 rows and raw AbortError tracebacks from
            # before expected aborts moved to their minute aggregate (same
            # substring the doDurationAborts24h snapshot below keys on).
            if "Exceeded allowed duration" in message:
                failed["durable_objects"] = True
                hit_count["durable_objects"] += 1
                if first_hit["durable_objects"] is None:
                    first_hit["durable_objects"] = (row.get("status"), path, message.strip())
            # 502/503/504 on a direct-mirror content path (release blob, repo
            # browse, git clone) means the selected endpoint is unreachable —
            # upstream availability, which the signed HTTPS health check
            # (the git_hosting signal) already tracks. It is not evidence the
            # Worker/API is down, and a single offline node's blob being
            # re-requested every few minutes used to paint the whole "api"
            # system red on /status. A 500 on the same path still counts.
            if row_status in (502, 503, 504) and (
                    RELEASE_BLOB_RE.match(path) or REPO_HOST_RE.match(path) or
                    GIT_INFO_RE.match(path) or GIT_PACK_RE.match(path)):
                continue
            failed["errors"] = True
            hit_count["errors"] += 1
            if first_hit["errors"] is None:
                first_hit["errors"] = (
                    row.get("status"), path, message.strip())
            if (path.rstrip("/") == "/api/world/ws" or
                    ROOM_RE.match(path) or REPO_ROOM_RE.match(path) or
                    GIT_INFO_RE.match(path) or GIT_PACK_RE.match(path)):
                bucket = "realtime"
            elif path.startswith("/api/"):
                bucket = "api"
            else:
                bucket = "website"
            failed[bucket] = True
            hit_count[bucket] += 1
            if first_hit[bucket] is None:
                first_hit[bucket] = (row.get("status"), path, message.strip())

        # Expected platform aborts are intentionally kept out of error_log:
        # every reconnecting room client can see the same DO failure, and one
        # actionable error row per client obscures real Worker bugs. Preserve
        # the operational signal with one content-free aggregate row per
        # minute. Sample the completed preceding minute so an abort that lands
        # just before this cron tick is neither missed nor counted again by the
        # next tick.
        abort_rows = await d1_all(
            env,
            "SELECT aborts,duration_aborts "
            "FROM durable_object_abort_minute WHERE minute_ts=?",
            minute_ts - STATUS_SAMPLE_WINDOW_MS,
        )
        abort_count = sum(
            max(0, int(row.get("aborts") or 0)) for row in abort_rows)
        duration_abort_count = sum(
            max(0, int(row.get("duration_aborts") or 0))
            for row in abort_rows)
        if abort_count:
            summary = "%d Durable Object request%s aborted" % (
                abort_count, "" if abort_count == 1 else "s")
            if duration_abort_count:
                summary += " (%d exceeded allowed duration)" % (
                    duration_abort_count)
            for bucket in ("durable_objects", "realtime"):
                failed[bucket] = True
                # The summary already carries the aggregate event count; this
                # is one status signal, not N separate error rows.
                hit_count[bucket] += 1
                if first_hit[bucket] is None:
                    first_hit[bucket] = (
                        503, "/durable-object-rooms", summary)
        ok["website"] = bool(ok.get("website", False)) and not failed["website"]
        ok["api"] = not failed["api"]
        ok["errors"] = not failed["errors"]
        ok["realtime"] = not failed["realtime"]
        ok["durable_objects"] = not failed["durable_objects"]
        for bucket in buckets:
            if failed[bucket] and first_hit[bucket]:
                status_code, path, message = first_hit[bucket]
                text = (str(status_code) + " on " + path) if status_code else path
                if message:
                    text += ": " + message[:120]
                if hit_count[bucket] > 1:
                    text += " (+%d more)" % (hit_count[bucket] - 1)
                if bucket != "website" or ok["website"] or not reason.get("website"):
                    reason[bucket] = text
    except Exception:
        # A query hiccup here is not itself evidence of an outage — don't
        # fabricate a false incident from it.
        ok["api"] = ok["errors"] = True
        ok["realtime"] = ok["durable_objects"] = True

    # Only mirrors currently present in the account-bound Hosts roster get a
    # /status row. Historical registrations and samples must not keep removed
    # hosts visible. A still-active host can remain visible as red when its most
    # recent endpoint check is unhealthy or otherwise invalid.
    status_systems = list(STATUS_SYSTEMS)
    try:
        mirror_rows = await _status_active_mirror_rows(env, now)
        try:
            expected_artifacts, mirror_artifact_counts = (
                await _status_flagship_artifact_counts(env, now))
        except Exception:
            # Endpoint health remains independently meaningful during a
            # catalog read hiccup. The Database row reports that failure; do
            # not fabricate an artifact mismatch without a source count.
            expected_artifacts, mirror_artifact_counts = 0, {}
        seen_mirrors = set()
        for row in mirror_rows:
            mirror_name = _status_mirror_name(row)
            if not mirror_name or mirror_name in seen_mirrors:
                continue
            seen_mirrors.add(mirror_name)
            system_id = STATUS_MIRROR_PREFIX + mirror_name
            status_systems.append(
                (system_id, "Mirror node — " + mirror_name))
            checked_at = int(row.get("checked_at") or 0)
            verified_at = int(row.get("forkmesh_verified_at") or 0)
            is_fresh = (
                checked_at >= now - HTTPS_MIRROR_STATUS_FRESH_MS
                and verified_at >= now - HTTPS_MIRROR_STATUS_FRESH_MS)
            is_healthy = int(row.get("healthy") or 0) == 1
            integrity_ok = str(row.get("integrity") or "") == "ok"
            is_active = int(row.get("forkmesh_active") or 0) == 1
            is_allowed = int(row.get("abuse_blocked") or 0) == 0
            reported_artifacts = mirror_artifact_counts.get(mirror_name)
            artifacts_ok = (
                expected_artifacts <= 0
                or (
                    reported_artifacts is not None
                    and reported_artifacts >= expected_artifacts))
            mirror_ok = (
                is_fresh and is_healthy and integrity_ok and is_active
                and is_allowed and artifacts_ok)
            ok[system_id] = mirror_ok
            if not mirror_ok:
                if not is_fresh:
                    reason[system_id] = (
                        mirror_name + " has not supplied a fresh signed "
                        "ForkMesh repository proof within the last 10 minutes")
                elif not is_healthy:
                    reason[system_id] = (
                        mirror_name + " failed its signed HTTPS health check")
                elif not integrity_ok:
                    reason[system_id] = (
                        mirror_name + " reported repository integrity " +
                        (str(row.get("integrity") or "unknown")))
                elif not is_allowed:
                    reason[system_id] = (
                        mirror_name + " is blocked from serving traffic")
                elif not artifacts_ok:
                    reason[system_id] = (
                        mirror_name + " serves only "
                        + str(reported_artifacts or 0) + " of "
                        + str(expected_artifacts) + " required "
                        + "forkmesh/forkmesh release artifacts")
                else:
                    reason[system_id] = (
                        mirror_name + " is reachable but is not serving an "
                        "accepted forkmesh/forkmesh source revision")
    except Exception as exc:
        # The aggregate Git-hosting row already reports a registry query
        # failure. Do not fabricate per-node identities when the authoritative
        # registry itself could not be read. Workers Logs stays off, so the
        # failure goes straight to Sentry (best-effort — never let telemetry
        # sink the sample; an error_log row here would also turn a registry
        # hiccup into a red "errors" minute).
        try:
            await capture_sentry_error(
                env, 500, "scheduled", "/cron/record-status-sample",
                "status mirror registry query failed: "
                + _safe_error_text(exc), error=exc)
        except BaseException:
            pass
    try:
        for core_name, (core_ok, core_reason) in sorted(
                (await _status_core_health(env, now)).items()):
            system_id = STATUS_CORE_PREFIX + core_name
            status_systems.append((system_id, "Core node — " + core_name))
            ok[system_id] = core_ok
            if not core_ok:
                reason[system_id] = core_reason
    except Exception as exc:
        try:
            await capture_sentry_error(
                env, 500, "scheduled", "/cron/record-status-sample",
                "status core roster query failed: "
                + _safe_error_text(exc), error=exc)
        except BaseException:
            pass
    try:
        for runner_name, (runner_ok, runner_reason) in sorted(
                (await _status_runner_health(env, now)).items()):
            system_id = STATUS_RUNNER_PREFIX + runner_name
            status_systems.append((system_id, "Runner — " + runner_name))
            ok[system_id] = runner_ok
            if not runner_ok:
                reason[system_id] = runner_reason
    except Exception as exc:
        try:
            await capture_sentry_error(
                env, 500, "scheduled", "/cron/record-status-sample",
                "status runner roster query failed: "
                + _safe_error_text(exc), error=exc)
        except BaseException:
            pass

    # One bounded JSON row owns the entire minute. Per-system rows made the D1
    # write bill grow with every public mirror (and their time indexes doubled
    # it); completed hours/days are compacted absolutely below.
    minute_systems = {}
    for system_id, _label in status_systems:
        failure = 0 if ok.get(system_id, True) else 1
        minute_systems[system_id] = {
            "ok": not bool(failure),
            "reason": clean_string(reason.get(system_id, "") or "", 240)
            or None,
        }
    await _write_status_minute_batch(env, minute_ts, minute_systems)
    # Alerts are optional follow-up work. Persist the minute first so a slow
    # mail provider or notification failure cannot erase public status data.
    if deliver_alerts:
        try:
            await _record_status_monitor_transitions(
                env, ok, reason, now, status_systems)
        except Exception as exc:
            # Same reasoning as the registry read above: Sentry-only visibility,
            # never an error_log row and never a raised sample.
            try:
                await capture_sentry_error(
                    env, 500, "scheduled", "/cron/record-status-sample",
                    "record_status_monitor_transitions failed: "
                    + _safe_error_text(exc), error=exc)
            except BaseException:
                pass
    if run_compaction:
        await record_status_compaction(env, now=now)


async def record_status_compaction(env, now=None):
    """Compact completed status buckets and apply bounded retention.

    CronRunner invokes this after the minute sampler in a distinct Worker
    invocation. Keeping the top-of-hour dual reads and retention statements
    out of the sampler leaves both phases comfortably below Free D1's
    50-query ceiling. Direct/test callers retain the historical one-call
    behavior through ``record_status_sample(run_compaction=True)``.
    """
    await ensure_schema(env)
    now = int(Date.now() if now is None else now)
    day_ts = (now // STATUS_DAY_MS) * STATUS_DAY_MS
    hour_ts = (now // STATUS_HOUR_MS) * STATUS_HOUR_MS
    minute_ts = (now // STATUS_SAMPLE_WINDOW_MS) * STATUS_SAMPLE_WINDOW_MS
    repaired_hour = await _compact_status_hour(env, hour_ts, now)
    repaired_day = None
    if repaired_hour is not None:
        candidate_day = (
            int(repaired_hour) // STATUS_DAY_MS) * STATUS_DAY_MS
        if candidate_day < day_ts:
            repaired_day = candidate_day
    await _compact_status_day(
        env, day_ts, now, force_target=repaired_day)
    # Retention prunes only need to run occasionally, not 60x/hour: sweep on
    # the first sample of each hour.
    if now - hour_ts < STATUS_SAMPLE_WINDOW_MS:
        await d1_run(
            env, "DELETE FROM system_status_daily WHERE day_ts < ?",
            day_ts - STATUS_HISTORY_RETAIN_MS,
        )
        await d1_run(
            env, "DELETE FROM system_status_hourly WHERE hour_ts < ?",
            # Legacy rows are no longer queried outside the bounded 24-hour
            # handoff window, but deleting the existing 30-day history in one
            # rollout-hour would spend thousands of indexed D1 writes. Retain
            # the table's established 30-day envelope so the cutoff advances
            # one day at a time and drains the old rows gradually.
            day_ts - STATUS_HISTORY_RETAIN_MS,
        )
        await d1_run(
            env, "DELETE FROM system_status_minute WHERE minute_ts < ?",
            minute_ts - STATUS_MINUTE_RETAIN_MS,
        )
        await d1_run(
            env, "DELETE FROM system_status_daily_batch WHERE day_ts < ?",
            day_ts - STATUS_HISTORY_RETAIN_MS,
        )
        await d1_run(
            env, "DELETE FROM system_status_hourly_batch WHERE hour_ts < ?",
            day_ts - STATUS_HISTORY_RETAIN_MS,
        )
        await d1_run(
            env, "DELETE FROM system_status_minute_batch WHERE minute_ts < ?",
            minute_ts - STATUS_MINUTE_RETAIN_MS,
        )
        await d1_run(
            env,
            "DELETE FROM system_status_sample_claim WHERE minute_ts < ?",
            minute_ts - STATUS_MINUTE_RETAIN_MS,
        )
        await d1_run(
            env,
            "DELETE FROM durable_object_abort_minute WHERE minute_ts < ?",
            minute_ts - STATUS_HISTORY_RETAIN_MS,
        )
    return True


async def record_status_alerts(env, delivery_channel="both"):
    """Deliver a bounded alert slice from the already-persisted minute row.

    Sampling and alert fan-out are separate CronRunner invocations.  This keeps
    a fleet-wide outage (and its optional notification/email writes) from
    sharing the sampler's 50-query Free-plan ceiling while preserving retry:
    undelivered rows keep blank notification markers and are selected again on
    the next minute.
    """
    await ensure_schema(env)
    now = int(Date.now())
    minute_ts = (now // STATUS_SAMPLE_WINDOW_MS) * STATUS_SAMPLE_WINDOW_MS
    rows = await _status_minute_records(env, minute_ts, minute_ts)
    if not rows:
        return False
    ok = {}
    reason = {}
    labels = dict(STATUS_SYSTEMS)
    status_systems = []
    for row in sorted(rows, key=lambda item: str(item.get("system") or "")):
        system_id = _status_batch_system_id(row.get("system"))
        if not system_id:
            continue
        is_up = int(row.get("ok") or 0) == 1
        ok[system_id] = is_up
        if not is_up:
            reason[system_id] = clean_string(
                row.get("reason") or "", 240)
        label = labels.get(system_id)
        if label is None and system_id.startswith(STATUS_MIRROR_PREFIX):
            mirror_name = system_id[len(STATUS_MIRROR_PREFIX):]
            label = "Mirror node — " + mirror_name
        if label is None and system_id.startswith(STATUS_CORE_PREFIX):
            label = "Core node — " + system_id[len(STATUS_CORE_PREFIX):]
        if label is None and system_id.startswith(STATUS_RUNNER_PREFIX):
            label = "Runner — " + system_id[len(STATUS_RUNNER_PREFIX):]
        status_systems.append((system_id, label or system_id))
    if not status_systems:
        return False
    transitioned = await _record_status_monitor_transitions(
        env, ok, reason, now, status_systems,
        delivery_channel=delivery_channel)
    if transitioned:
        await schedule_public_snapshot_republish(
            env, public_snapshots.SNAPSHOT_STATUS_PATHS)
    return True


async def status_history(env, view="full"):
    await ensure_schema(env)
    now = int(Date.now())
    cur_day = (now // 86400000) * 86400000
    start = cur_day - (STATUS_HISTORY_DAYS - 1) * 86400000
    historical_end = cur_day - STATUS_DAY_MS
    day_rows = (
        await _status_day_records(env, start, historical_end)
        if historical_end >= start else []
    )
    by_system_day = {}
    for row in day_rows:
        system_id = str(row.get("system") or "")
        by_system_day.setdefault(system_id, {})[int(row["day_ts"])] = (
            int(row.get("checks") or 0), int(row.get("failures") or 0),
        )

    current_hour = (now // STATUS_HOUR_MS) * STATUS_HOUR_MS
    uptime_24h_start = current_hour - 23 * STATUS_HOUR_MS
    # Only completed hours needed by the current UTC day or rolling 24h bar.
    # Historical day cells/totals come from the one-row daily buckets above.
    hour_start = min(cur_day, uptime_24h_start)
    # Include a legacy current-hour row during the bounded migration handoff;
    # raw batch minutes below overwrite it as soon as the new sampler lands.
    completed_hour_end = current_hour
    hour_rows = (
        await _status_hour_records(env, hour_start, completed_hour_end)
        if completed_hour_end >= hour_start else []
    )
    by_system_hour = {}
    for row in hour_rows:
        system_id = str(row.get("system") or "")
        by_system_hour.setdefault(system_id, {})[int(row["hour_ts"])] = (
            int(row.get("checks") or 0), int(row.get("failures") or 0),
            row.get("reason") or None,
        )

    current_minute = (now // 60000) * 60000
    minute_start = current_minute - (STATUS_MINUTES_SHOWN - 1) * 60000
    upcoming_slots, previous_slots = _status_phase_slots(current_minute)
    minute_rows = await _status_minute_records(
        env, minute_start, current_minute)
    active_mirror_rows = []
    try:
        active_mirror_rows = await _status_active_mirror_rows(env, now)
    except Exception:
        active_mirror_rows = []
    active_mirrors = {
        mirror_name for mirror_name in (
            _status_mirror_name(row) for row in active_mirror_rows)
        if mirror_name
    }
    try:
        active_cores = {
            row["node_id"] for row in await _status_core_rows(env, now)}
    except Exception:
        active_cores = set()
    try:
        active_runners = {
            row["name"] for row in await _status_runner_rows(env)}
    except Exception:
        active_runners = set()

    by_system_minute = {}
    last_sample_ts = 0
    current_hour_samples = {}
    for row in sorted(minute_rows, key=lambda item: int(item["minute_ts"])):
        system_id = str(row.get("system") or "")
        by_system_minute.setdefault(system_id, {})[int(row["minute_ts"])] = row
        last_sample_ts = max(last_sample_ts, int(row["minute_ts"]))
        if int(row["minute_ts"]) >= current_hour:
            aggregate = current_hour_samples.setdefault(
                system_id, [0, 0, None])
            aggregate[0] += 1
            if int(row.get("ok") or 0) != 1:
                aggregate[1] += 1
                aggregate[2] = row.get("reason") or None
    for system_id, aggregate in current_hour_samples.items():
        by_system_hour.setdefault(system_id, {})[current_hour] = tuple(aggregate)

    # Historical status is retained in D1, but the public roster is the current
    # active Hosts set. Union it so a newly activated host appears immediately;
    # filter old samples below so a removed host cannot resurrect itself.
    recorded_system_ids = (
        set(by_system_day) | set(by_system_hour) | set(by_system_minute))
    recorded_system_ids.update(
        STATUS_MIRROR_PREFIX + mirror_name for mirror_name in active_mirrors)
    mirror_systems = []
    for system_id in recorded_system_ids:
        if not system_id.startswith(STATUS_MIRROR_PREFIX):
            continue
        mirror_name = system_id[len(STATUS_MIRROR_PREFIX):]
        if mirror_name not in active_mirrors:
            continue
        mirror_systems.append(
            (system_id, "Mirror node — " + mirror_name))
    mirror_systems.sort(key=lambda item: item[1].lower())
    core_systems = [
        (STATUS_CORE_PREFIX + core_name, "Core node — " + core_name)
        for core_name in sorted(active_cores)]
    runner_systems = [
        (STATUS_RUNNER_PREFIX + runner_name, "Runner — " + runner_name)
        for runner_name in sorted(active_runners)]
    status_systems = (
        list(STATUS_SYSTEMS) + core_systems + runner_systems + mirror_systems)

    systems = []
    for system_id, label in status_systems:
        # Displayed uptime is the fraction of expected samples that passed.
        # Missing samples remain separate monitoring gaps rather than service
        # failures, but they must keep a history containing non-green buckets
        # from displaying 100%. See _status_effective_hour.
        days = []
        total_checks = total_failures = total_expected = 0
        checks_24h = failures_24h = expected_24h = 0
        for i in range(STATUS_HISTORY_DAYS):
            this_day = start + i * STATUS_DAY_MS
            historical = this_day < cur_day
            hours = []
            if historical:
                day_checks, day_failures = by_system_day.get(
                    system_id, {}).get(this_day, (0, 0))
                elapsed_hours = 24
                day_expected = 24 * 60
            else:
                day_checks = day_failures = day_expected = 0
                elapsed_hours = int((current_hour - this_day) // STATUS_HOUR_MS) + 1
                for h in range(elapsed_hours):
                    hour_ts = this_day + h * STATUS_HOUR_MS
                    hour = _status_effective_hour(
                        hour_ts, now,
                        by_system_hour.get(system_id, {}).get(hour_ts),
                    )
                    day_expected += hour["expectedChecks"]
                    day_checks += hour["checks"]
                    day_failures += hour["failures"]
            # Serialize only the rolling latest 24 hourly cells. A historical
            # day still gets its exact total from the daily batch above.
            first_visible_hour = max(this_day, uptime_24h_start)
            last_visible_hour = min(
                this_day + 23 * STATUS_HOUR_MS, current_hour)
            if first_visible_hour <= last_visible_hour:
                hour_ts = first_visible_hour
                while hour_ts <= last_visible_hour:
                    hour = _status_effective_hour(
                        hour_ts, now,
                        by_system_hour.get(system_id, {}).get(hour_ts),
                    )
                    hours.append(hour)
                    expected_24h += hour["expectedChecks"]
                    checks_24h += hour["checks"]
                    failures_24h += hour["failures"]
                    hour_ts += STATUS_HOUR_MS
            total_expected += day_expected
            total_checks += day_checks
            total_failures += day_failures
            uptime = (
                round(
                    (max(0, min(day_checks - day_failures, day_expected)) /
                     day_expected) * 100,
                    2,
                )
                if day_checks else None
            )
            coverage = (
                round((min(day_checks, day_expected) / day_expected) * 100, 2)
                if day_expected else None
            )
            days.append({
                "dayTs": this_day, "checks": day_checks,
                "failures": day_failures,
                "expectedChecks": day_expected,
                "missingChecks": max(0, day_expected - day_checks),
                "uptimePct": uptime, "coveragePct": coverage,
                "hours": hours, "hoursElapsed": elapsed_hours,
                "hoursCompacted": elapsed_hours > len(hours),
            })
        # Current service state comes from the newest RAW MINUTE probe. An
        # hourly aggregate answers "did anything fail during this hour?", not
        # "is it reachable now"; using it for the badge left recovered systems
        # yellow and displayed an hour-old reason as though it were current.
        # A missed shared sampler window must not overwrite that evidence:
        # retain each latest observed result and report sampler freshness once
        # in current.monitoringState below. Otherwise one delayed Cron event
        # makes every unrelated service and mirror look simultaneously down.
        latest_minute_row = None
        minute_records = by_system_minute.get(system_id, {})
        if minute_records:
            latest_minute_ts = max(minute_records)
            latest_minute_row = minute_records[latest_minute_ts]
        reason_text = reason_ts = monitoring_state = observed_at = None
        if latest_minute_row is not None:
            observed_at = latest_minute_ts
            if int(latest_minute_row.get("ok") or 0) == 1:
                status = "operational"
            else:
                status = "down"
                reason_text = (
                    latest_minute_row.get("reason")
                    or "The latest reachability check failed")
                reason_ts = latest_minute_ts
        else:
            # Backward-compatible fallback for an existing deployment while
            # minute sampling is first being populated.
            status = None

        latest_hour = None
        for d in reversed(days):
            for h in reversed(d["hours"]):
                if h["checks"]:
                    latest_hour = h
                    break
            if latest_hour:
                break
        if status is None:
            if latest_hour is not None and latest_hour["checks"]:
                status = latest_hour["status"]
                observed_at = latest_hour.get("hourTs")
                if status in ("down", "degraded"):
                    reason_text = latest_hour.get("reason")
                    reason_ts = latest_hour.get("hourTs")
            else:
                status = "unknown"
        overall_uptime = (
            round(
                (max(0, min(total_checks - total_failures, total_expected)) /
                 total_expected) * 100,
                2,
            )
            if total_checks else None
        )
        uptime_24h = (
            round(
                (max(0, min(checks_24h - failures_24h, expected_24h)) /
                 expected_24h) * 100,
                2,
            )
            if checks_24h else None
        )
        coverage_30d = (
            round((min(total_checks, total_expected) / total_expected) * 100, 2)
            if total_expected else None
        )
        coverage_24h = (
            round((min(checks_24h, expected_24h) / expected_24h) * 100, 2)
            if expected_24h else None
        )
        minutes = [
            _status_minute(
                minute_start + i * 60000, current_minute,
                by_system_minute.get(system_id, {}).get(minute_start + i * 60000),
            )
            for i in range(STATUS_MINUTES_SHOWN)
        ]

        check_description = STATUS_SYSTEM_CHECKS.get(system_id, "")
        check_steps = STATUS_SYSTEM_CHECK_STEPS.get(system_id, ())
        if system_id == "email" and _mail_over_smtp(env):
            check_description = STATUS_EMAIL_SMTP_CHECK
            check_steps = STATUS_EMAIL_SMTP_CHECK_STEPS
        elif system_id.startswith(STATUS_MIRROR_PREFIX):
            check_steps = STATUS_MIRROR_CHECK_STEPS
            mirror_name = system_id[len(STATUS_MIRROR_PREFIX):]
            check_description = (
                "Samples " + mirror_name + " independently once a minute "
                "and refreshes its account-bound direct HTTPS endpoint proof "
                "every minute. Passes only "
                "when its signed health and forkmesh/forkmesh repository proof "
                "are fresh, endpoint health is good, integrity is ok, and the "
                "served refs match an accepted source revision. Its signed "
                "physical release-artifact count must also match the canonical "
                "forkmesh/forkmesh source; the release deep probe separately "
                "fetches every artifact in the latest signed manifest.")
        elif system_id.startswith(STATUS_CORE_PREFIX):
            check_steps = STATUS_CORE_CHECK_STEPS
            core_name = system_id[len(STATUS_CORE_PREFIX):]
            check_description = (
                "Checks core node " + core_name + " every minute. Passes "
                "only when its cluster heartbeat is fresh, it is active "
                "(unsealed, not quarantined, not leaving), every peer's "
                "HTTPS readiness probe of it passes, and its intrusion "
                "detector is fresh and out of lockdown. While it fails, "
                "administrators are pinged every five minutes.")
        elif system_id.startswith(STATUS_RUNNER_PREFIX):
            check_steps = STATUS_RUNNER_CHECK_STEPS
            runner_name = system_id[len(STATUS_RUNNER_PREFIX):]
            check_description = (
                "Checks runner " + runner_name + " every minute. Passes "
                "while it is an active pool runner that has claimed or "
                "polled for agent jobs within the last three minutes and "
                "none of its provider logins has expired. A runner that is "
                "scaled down and revoked leaves this list.")
        systems.append({
            "id": system_id, "label": label, "status": status,
            "checkDescription": check_description,
            "checks": [
                {"name": name, "detail": detail}
                for name, detail in check_steps
            ],
            "uptimePct": overall_uptime, "uptime24hPct": uptime_24h,
            "coveragePct": coverage_30d, "coverage24hPct": coverage_24h,
            "checks24h": checks_24h, "failures24h": failures_24h,
            "missing24h": max(0, expected_24h - checks_24h),
            "days": days, "minutes": minutes,
            "reason": reason_text,
            "reasonTs": reason_ts,
            "monitoringState": monitoring_state,
            "observedAt": observed_at,
            "nextCheck": _status_next_check(
                system_id, status != "operational",
                upcoming_slots, previous_slots),
        })

    # Current-state snapshot (issue #356): the headline health metrics rendered
    # at the top of the page — flagship repository directly reachable, the
    # distinct online node count (same signal as the /network/ headline),
    # catalog size, and errors logged in the last 24h. Each
    # read is best-effort so one failing query can't blank the summary, and it
    # all rides on the single /api/status fetch a page view already makes.
    current = {}
    # Sampling freshness is one shared reporting signal, not a service state.
    # Keep it at the payload level so one delayed Cron event produces one
    # honest warning instead of turning every component row red. 0 = no sample
    # in the visible window at all.
    current["lastCronSampleTs"] = last_sample_ts or None
    if not last_sample_ts:
        current["monitoringState"] = "empty"
    elif last_sample_ts < (
            current_minute - 2 * STATUS_SAMPLE_WINDOW_MS):
        current["monitoringState"] = "stale"
    else:
        current["monitoringState"] = None
    try:
        current["catalogRepos"] = await _repositories_count(env, now)
    except Exception:
        current["catalogRepos"] = None
    try:
        online = {
            label for label in (await _live_online_nodes(env, now)).values() if label
        }
        current["onlineNodes"] = len(online)
    except Exception:
        current["onlineNodes"] = None
    try:
        # Dedicated counter for the platform killing a Durable Object request
        # mid-flight ("Exceeded allowed duration in Durable Objects free
        # tier."). Count generic errors and legacy duration-abort rows in the
        # same indexed 24-hour pass, then add the new minute aggregates. This
        # preserves a continuous total while avoiding a second scan of the
        # error window on every scheduled status projection.
        error_row = await d1_first(
            env,
            "SELECT COUNT(*) AS errors24h, "
            "COALESCE(SUM(CASE WHEN message LIKE ? THEN 1 ELSE 0 END),0) + "
            "(SELECT COALESCE(SUM(duration_aborts),0) "
            " FROM durable_object_abort_minute WHERE minute_ts>=?) "
            "AS duration_aborts24h FROM error_log WHERE ts>=?",
            "%Exceeded allowed duration%",
            now - 24 * 60 * 60 * 1000,
            now - 24 * 60 * 60 * 1000,
        )
        current["errors24h"] = int(
            (error_row or {}).get("errors24h", 0) or 0)
        current["doDurationAborts24h"] = int(
            (error_row or {}).get("duration_aborts24h", 0) or 0)
    except Exception:
        current["errors24h"] = None
        current["doDurationAborts24h"] = None
    try:
        # forkmesh_verified_at is written only after an account-bound endpoint
        # signs a fresh challenge whose refs hash matches the owner-attested
        # public forkmesh/forkmesh catalog state. This avoids treating a
        # control WebSocket heartbeat as proof that Git bytes are available.
        endpoint_row = await d1_first(
            env,
            "SELECT MAX(CASE WHEN healthy=1 AND forkmesh_active=1 "
            "AND integrity='ok' AND abuse_blocked=0 "
            "THEN forkmesh_verified_at ELSE 0 END) AS ts, "
            "COALESCE(SUM(CASE WHEN checked_at>=? AND healthy=1 "
            "AND integrity='ok' AND abuse_blocked=0 "
            "AND forkmesh_active=0 AND forkmesh_refs_sha256!='' "
            "THEN 1 ELSE 0 END),0) AS unaccepted "
            "FROM mirror_https_endpoints",
            now - HTTPS_MIRROR_STATUS_FRESH_MS,
        )
        last_ts = int((endpoint_row or {}).get("ts") or 0) or None
        current["mainnodeUnacceptedMirrors"] = int(
            (endpoint_row or {}).get("unaccepted") or 0)
        current["mainnodeLastSeenTs"] = last_ts
        current["mainnodeOnline"] = (
            last_ts is not None
            and now - last_ts < HTTPS_MIRROR_STATUS_FRESH_MS
        )
        current["mainnodeStaleMs"] = HTTPS_MIRROR_STATUS_FRESH_MS
    except Exception:
        current["mainnodeOnline"] = None
        current["mainnodeLastSeenTs"] = None
        current["mainnodeStaleMs"] = HTTPS_MIRROR_STATUS_FRESH_MS
        current["mainnodeUnacceptedMirrors"] = None

    payload = {"ok": True, "now": now, "systems": systems, "current": current}
    if view == "world":
        # Compatibility for direct internal callers. Public World requests use
        # cached_status_history(), which projects the already cached/built full
        # response and therefore cannot launch another D1 history build.
        payload = public_snapshots.project_world_status_payload(payload)
    return json_response(payload, cache_seconds=STATUS_EDGE_CACHE_TTL)
