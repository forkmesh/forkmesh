from __future__ import annotations

import json
import re

import apps_core as core


DRAIN_IMMEDIATE = 4
CRON_BATCH = 25
MAX_INSTALLS_PER_EVENT = 200
MAX_EVENT_DATA_BYTES = 16 * 1024
VERIFY_TIMEOUT_SECONDS = 8
DELIVERY_TIMEOUT_SECONDS = 10


def _scopes(text):
    return core.parse_scopes(str(text or "")) or ()


def mentions(text, slug):
    if not slug:
        return False
    return re.search(r"(?<![\w@/-])@%s(?![\w-])" % re.escape(slug),
                     str(text or ""), re.IGNORECASE) is not None


async def _app(runtime, app_id, cache):
    if app_id in cache:
        return cache[app_id]
    row = await runtime.d1_first("SELECT * FROM apps WHERE app_id=?", app_id)
    record = await runtime.open(row.get("data")) if row else None
    cache[app_id] = (row, record) if isinstance(record, dict) else (None, None)
    return cache[app_id]


def _deliverable(row):
    return bool(row) and str(row.get("listing") or "") != "suspended" and \
        int(row.get("event_url_verified_at") or 0) > 0 and \
        not int(row.get("events_disabled_at") or 0)


def _trim(data):
    data = dict(data or {})
    text = json.dumps(data, separators=(",", ":"))
    if len(text.encode()) <= MAX_EVENT_DATA_BYTES:
        return data
    if isinstance(data.get("body"), str):
        data["body"] = data["body"][:2000]
        data["truncated"] = True
    return data


async def queue(runtime, row, record, install, event_type, data, target):
    now = runtime.now()
    event_id = runtime.new_hex(16)
    payload = {
        "type": "event_callback",
        "eventId": event_id,
        "appId": row["app_id"],
        "installId": install["install_id"],
        "target": target,
        "ts": now,
        "event": dict(_trim(data), type=event_type),
    }
    await runtime.d1_run(
        "INSERT INTO app_event_outbox (app_id, install_id, event_id, "
        "event_type, attempts, next_ts, created_at, data) "
        "VALUES (?,?,?,?,0,?,?,?)",
        row["app_id"], install["install_id"], event_id, event_type, now, now,
        await runtime.seal(payload))
    return event_id


async def _target(runtime, install):
    opened = await runtime.open(install.get("data")) or {}
    return {"kind": str(install.get("target_kind") or ""),
            "name": str(opened.get("targetName") or "")}


async def _fan_out(runtime, installs, event_type, data, source_actor, text):
    cache = {}
    queued = 0
    for install in installs[:MAX_INSTALLS_PER_EVENT]:
        row, record = await _app(runtime, install["app_id"], cache)
        if not _deliverable(row):
            continue
        if source_actor and source_actor == core.bot_author(row.get("slug")):
            continue
        scopes = _scopes(install.get("scopes"))
        subscribed = set(record.get("events") or ())
        types = []
        if event_type in subscribed and core.event_allowed(event_type,
                                                           scopes):
            types.append(event_type)
        if "app.mention" in subscribed and mentions(text, row.get("slug")) \
                and core.event_allowed("app.mention", scopes):
            needed = {"issue": "issues:read", "pull": "pulls:read",
                      "chat": "chat:read"}[event_type.split(".")[0]]
            if needed in scopes:
                types.append("app.mention")
        if not types:
            continue
        target = await _target(runtime, install)
        for kind in types:
            payload = dict(data)
            if kind == "app.mention":
                payload["source"] = event_type
            await queue(runtime, row, record, install, kind, payload, target)
            queued += 1
    if queued:
        await drain(runtime, min(queued, DRAIN_IMMEDIATE))
    return queued


async def installs_for_repo(runtime, owner):
    owner = str(owner or "").strip().lower()
    installs = []
    org_bi = await runtime.org_lookup(owner)
    if org_bi:
        installs += await runtime.d1_all(
            "SELECT * FROM app_installs WHERE target_bi=? "
            "AND target_kind='org' AND status='active' LIMIT ?",
            org_bi, MAX_INSTALLS_PER_EVENT)
    for account in await runtime.repo_accounts(owner):
        installs += await runtime.d1_all(
            "SELECT * FROM app_installs WHERE target_bi=? "
            "AND target_kind='user' AND status='active' LIMIT ?",
            await runtime.blind("app-target:user:" + account),
            MAX_INSTALLS_PER_EVENT)
    return installs


async def emit_repo_event(runtime, owner, repo, event_type, data,
                          source_actor=""):
    if event_type not in core.EVENT_SCOPES:
        return 0
    installs = await installs_for_repo(runtime, owner)
    if not installs:
        return 0
    payload = dict(data or {})
    payload["repository"] = str(owner).lower() + "/" + str(repo)
    text = " ".join(str(payload.get(key) or "") for key in ("title", "body"))
    return await _fan_out(runtime, installs, event_type, payload,
                          source_actor, text)


async def emit_ping(runtime, recipient, title, href):
    recipient = str(recipient or "").strip().lower()
    if not recipient:
        return 0
    installs = await runtime.d1_all(
        "SELECT * FROM app_installs WHERE target_bi=? AND target_kind='user' "
        "AND status='active' AND scopes LIKE '%pings:read%' LIMIT ?",
        await runtime.blind("app-target:user:" + recipient),
        MAX_INSTALLS_PER_EVENT)
    if not installs:
        return 0
    return await _fan_out(runtime, installs, "ping.created", {
        "recipient": recipient, "title": core.clean_line(title, 200),
        "href": str(href or "")[:300]}, "", "")


async def emit_chat(runtime, channel_id, channel_name, message,
                    source_actor=""):
    installs = await runtime.d1_all(
        "SELECT * FROM app_installs WHERE status='active' "
        "AND scopes LIKE '%chat:read%' ORDER BY last_used_at DESC LIMIT ?",
        MAX_INSTALLS_PER_EVENT)
    if not installs:
        return 0
    text = str(message.get("text") or "")
    return await _fan_out(runtime, installs, "chat.message", {
        "channel": {"id": channel_id, "name": channel_name},
        "messageId": str(message.get("id") or ""),
        "sender": {"id": str(message.get("senderId") or ""),
                   "name": str(message.get("sender") or "")},
        "text": text[:core.MAX_MESSAGE_TEXT],
    }, source_actor, text)


async def emit_uninstalled(runtime, install, reason):
    row, record = await _app(runtime, install["app_id"], {})
    if not row or not int(row.get("event_url_verified_at") or 0) or \
            not record.get("eventUrl"):
        return 0
    await queue(runtime, row, record, install, "app.uninstalled",
                {"reason": reason}, await _target(runtime, install))
    await drain(runtime, 1)
    return 1


async def record_delivery(runtime, app_id, kind, event_type, status,
                          attempts, outcome, detail=""):
    await runtime.d1_run(
        "INSERT INTO app_deliveries (delivery_id, app_id, kind, event_type, "
        "status, attempts, outcome, detail, ts) VALUES (?,?,?,?,?,?,?,?,?)",
        runtime.new_hex(12), app_id, kind, event_type, int(status),
        int(attempts), outcome, str(detail or "")[:120], runtime.now())
    await runtime.d1_run(
        "DELETE FROM app_deliveries WHERE app_id=? AND delivery_id NOT IN "
        "(SELECT delivery_id FROM app_deliveries WHERE app_id=? "
        "ORDER BY ts DESC, rowid DESC LIMIT ?)", app_id, app_id,
        core.DELIVERY_HISTORY_PER_APP)


async def signed_post(runtime, record, url, payload, request_type, attempt,
                      timeout):
    body = core.canonical_body(payload)
    request_id = str(payload.get("eventId") or payload.get("requestId")
                     or runtime.new_hex(16))
    timestamp = runtime.now() // 1000
    signature = core.sign(str(record.get("signingSecret") or ""), request_id,
                          timestamp, body)
    headers = core.request_headers(signature, request_id, timestamp,
                                   request_type, attempt)
    return await runtime.http_post(url, headers, body.decode("utf-8"),
                                   timeout)


async def _disable(runtime, row, record):
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE apps SET events_disabled_at=? WHERE app_id=? "
        "AND events_disabled_at=0", now, row["app_id"])
    await runtime.d1_run(
        "DELETE FROM app_event_outbox WHERE app_id=?", row["app_id"])
    owner = str(record.get("ownerName") or "")
    if owner:
        await runtime.notify(
            owner, "Events for %s were switched off" % record.get("name"),
            "%d deliveries in a row failed. Fix the endpoint, then verify "
            "the event URL again." % core.DISABLE_AFTER_FAILURES,
            "/dashboard/apps/build", "app-events-off:" + row["app_id"])
    await runtime.audit("app:" + str(row.get("slug")),
                        "apps.events.disabled", "app", row["app_id"],
                        outcome="failed")


async def _attempt(runtime, outbox, cache):
    row, record = await _app(runtime, outbox["app_id"], cache)
    payload = await runtime.open(outbox.get("data"))
    event_type = str(outbox.get("event_type") or "")
    if not row or not isinstance(payload, dict) or \
            int(row.get("events_disabled_at") or 0):
        await runtime.d1_run("DELETE FROM app_event_outbox WHERE id=?",
                             outbox["id"])
        return "dropped"
    if event_type != "app.uninstalled":
        live = await runtime.d1_first(
            "SELECT 1 AS ok FROM app_installs WHERE install_id=? "
            "AND status='active'", outbox["install_id"])
        if not live:
            await runtime.d1_run("DELETE FROM app_event_outbox WHERE id=?",
                                 outbox["id"])
            return "dropped"
    url = core.clean_endpoint_url(
        record.get("eventUrl"),
        core.dev_origins(runtime.config("APPS_DEV_ENDPOINT_ORIGINS")))
    attempts = int(outbox.get("attempts") or 0) + 1
    status = 0
    if url:
        status, _text = await signed_post(runtime, record, url, payload,
                                          event_type, attempts,
                                          DELIVERY_TIMEOUT_SECONDS)
    outcome = core.classify(status)
    if outcome == "retry" and attempts >= core.MAX_EVENT_ATTEMPTS:
        outcome = "dropped"
    if outcome == "retry":
        await runtime.d1_run(
            "UPDATE app_event_outbox SET attempts=?, next_ts=? WHERE id=?",
            attempts, runtime.now() + core.backoff_ms(attempts), outbox["id"])
    else:
        await runtime.d1_run("DELETE FROM app_event_outbox WHERE id=?",
                             outbox["id"])
    if outcome == "delivered":
        await runtime.d1_run(
            "UPDATE apps SET event_failures=0 WHERE app_id=?", row["app_id"])
        row["event_failures"] = 0
    else:
        await runtime.d1_run(
            "UPDATE apps SET event_failures=event_failures+1 WHERE app_id=?",
            row["app_id"])
        row["event_failures"] = int(row.get("event_failures") or 0) + 1
        if row["event_failures"] >= core.DISABLE_AFTER_FAILURES:
            row["events_disabled_at"] = runtime.now()
            await _disable(runtime, row, record)
    if outcome != "retry" or attempts == 1:
        await record_delivery(runtime, row["app_id"], "event", event_type,
                              status, attempts, outcome)
    return outcome


async def drain(runtime, limit):
    rows = await runtime.d1_all(
        "SELECT * FROM app_event_outbox WHERE next_ts<=? "
        "ORDER BY next_ts ASC, id ASC LIMIT ?", runtime.now(), int(limit))
    delivered = 0
    cache = {}
    for outbox in rows or []:
        try:
            if await _attempt(runtime, outbox, cache) == "delivered":
                delivered += 1
        except Exception:
            continue
    return delivered


async def verify_url(runtime, row, record, actor):
    url = core.clean_endpoint_url(
        record.get("eventUrl"),
        core.dev_origins(runtime.config("APPS_DEV_ENDPOINT_ORIGINS")))
    if not url:
        return False, "no_event_url"
    challenge = runtime.new_hex(24)
    payload = {"type": "url_verification", "challenge": challenge,
               "requestId": runtime.new_hex(16)}
    status, text = await signed_post(runtime, record, url, payload,
                                     "url_verification", 1,
                                     VERIFY_TIMEOUT_SECONDS)
    answer = ""
    try:
        answer = str((json.loads(text or "{}") or {}).get("challenge") or "")
    except (ValueError, AttributeError):
        answer = ""
    ok = 200 <= int(status or 0) < 300 and answer == challenge
    await record_delivery(runtime, row["app_id"], "verify",
                          "url_verification", status, 1,
                          "delivered" if ok else "failed",
                          "" if ok else "challenge_mismatch")
    if not ok:
        return False, "verification_failed"
    await runtime.d1_run(
        "UPDATE apps SET event_url_verified_at=?, events_disabled_at=0, "
        "event_failures=0 WHERE app_id=?", runtime.now(), row["app_id"])
    await runtime.audit(actor, "apps.events.verify", "app", row["app_id"])
    return True, ""


async def deliveries(runtime, app_id):
    rows = await runtime.d1_all(
        "SELECT kind, event_type, status, attempts, outcome, detail, ts "
        "FROM app_deliveries WHERE app_id=? ORDER BY ts DESC, rowid DESC "
        "LIMIT ?",
        app_id, core.DELIVERY_HISTORY_PER_APP)
    queued = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM app_event_outbox WHERE app_id=?", app_id)
    return {
        "deliveries": [{
            "kind": str(r.get("kind") or ""),
            "eventType": str(r.get("event_type") or ""),
            "status": int(r.get("status") or 0),
            "attempts": int(r.get("attempts") or 0),
            "outcome": str(r.get("outcome") or ""),
            "detail": str(r.get("detail") or ""),
            "ts": int(r.get("ts") or 0),
        } for r in rows or []],
        "queued": int((queued or {}).get("n") or 0),
    }
