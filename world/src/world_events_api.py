"""D1-backed, audited UTC event-announcement API for ForkMesh World."""

import json

import world_events as policy


EVENTS_PREFIX = "/api/world/events"
PUBLIC_EVENTS_CACHE_MS = 30 * 1000

# Browser clients deliberately bypass their HTTP cache when checking the live
# bulletin. Keep the identical public list briefly inside a warm Worker isolate
# so a burst of World tabs shares one D1 read. The scope is the environment
# object for that isolate; test/runtime adapters without one simply skip this
# optional cache.
_PUBLIC_EVENTS_CACHE = {
    "scope": 0,
    "expires_at": 0,
    "payload": None,
}


def _cache_scope(runtime):
    env = getattr(runtime, "env", None)
    return id(env) if env is not None else 0


def _cached_public_events(runtime, now):
    scope = _cache_scope(runtime)
    if (
        scope
        and scope == _PUBLIC_EVENTS_CACHE["scope"]
        and int(now) < int(_PUBLIC_EVENTS_CACHE["expires_at"])
        and isinstance(_PUBLIC_EVENTS_CACHE["payload"], dict)
    ):
        return _PUBLIC_EVENTS_CACHE["payload"]
    return None


def _remember_public_events(runtime, now, payload):
    scope = _cache_scope(runtime)
    if not scope or not isinstance(payload, dict):
        return
    _PUBLIC_EVENTS_CACHE.update({
        "scope": scope,
        "expires_at": int(now) + PUBLIC_EVENTS_CACHE_MS,
        "payload": payload,
    })


def _invalidate_public_events():
    _PUBLIC_EVENTS_CACHE.update({
        "scope": 0,
        "expires_at": 0,
        "payload": None,
    })


def _response(runtime, data, status=200, cache_control=None, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        data,
        status=status,
        cache_control=cache_control or
        "no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _target(path):
    clean = str(path or "").rstrip("/")
    if clean == EVENTS_PREFIX:
        return ""
    prefix = EVENTS_PREFIX + "/"
    candidate = clean[len(prefix):] if clean.startswith(prefix) else ""
    return candidate if policy.valid_event_id(candidate) else None


async def _body(runtime):
    data, error = await runtime.json_body(policy.WORLD_EVENT_BODY_MAX_BYTES)
    if error:
        return None, _response(
            runtime,
            {"error": error},
            status=413 if error == "payload_too_large" else 400,
        )
    return data, None


async def _actor(runtime, data):
    account_bi, account = await runtime.session(data or {})
    actor = str((account or {}).get("name") or "").strip().lower()
    return account_bi, actor


async def _authorized(runtime, data, action, target):
    account_bi, actor = await _actor(runtime, data)
    if not actor:
        return "", "", _response(
            runtime, {"error": "invalid_session"}, status=401)
    if not await runtime.is_admin(actor):
        await runtime.audit(
            actor,
            "world.event." + action,
            "world_event",
            target or "new",
            outcome="denied",
            details={"reason": "platform_administrator_required"},
        )
        return "", "", _response(
            runtime, {"error": "forbidden"}, status=403)
    return account_bi, actor, None


async def handle(runtime, path):
    """Serve public future events and admin-only create/update/cancel."""
    target = _target(path)
    if target is None:
        return _response(runtime, {"error": "not_found"}, status=404)
    method = runtime.method()
    now = runtime.now()

    if method == "GET":
        if not target:
            cached = _cached_public_events(runtime, now)
            if cached is not None:
                return _response(
                    runtime,
                    cached,
                    cache_control=(
                        "public, max-age=30, stale-while-revalidate=120"),
                )
        await runtime.ensure_schema()
        if target:
            row = await runtime.d1_first(
                "SELECT event_id,status,starts_at,ends_at,data,updated_at "
                "FROM world_events WHERE event_id=? AND status='scheduled' "
                "AND ends_at>?",
                target, now,
            )
            event = policy.public_event(row)
            if not event:
                return _response(
                    runtime, {"error": "not_found"}, status=404)
            return _response(
                runtime,
                {"ok": True, "event": event, "timeStandard": "UTC"},
                cache_control="public, max-age=30, stale-while-revalidate=120",
            )
        rows = await runtime.d1_all(
            "SELECT event_id,status,starts_at,ends_at,data,updated_at "
            "FROM world_events WHERE status='scheduled' AND ends_at>? "
            "ORDER BY starts_at ASC LIMIT ?",
            now, policy.MAX_EVENTS_RESPONSE,
        )
        payload = policy.event_list_payload(rows, now)
        _remember_public_events(runtime, now, payload)
        return _response(
            runtime,
            payload,
            cache_control="public, max-age=30, stale-while-revalidate=120",
        )

    await runtime.ensure_schema()
    if method not in ("POST", "PATCH", "DELETE"):
        return _response(
            runtime,
            {"error": "method_not_allowed"},
            status=405,
            allow="GET, POST, PATCH, DELETE",
        )
    if (method == "POST" and target) or (
            method in ("PATCH", "DELETE") and not target):
        return _response(runtime, {"error": "not_found"}, status=404)

    data = {}
    if method in ("POST", "PATCH"):
        data, error_response = await _body(runtime)
        if error_response is not None:
            return error_response
    account_bi, actor, denial = await _authorized(
        runtime, data, method.lower(), target)
    if denial is not None:
        return denial

    # Retention is enforced during mutations as well as deployment cleanup.
    await runtime.d1_run(
        "DELETE FROM world_events WHERE "
        "(status='cancelled' AND cancelled_at<?) OR "
        "(status='scheduled' AND ends_at<?)",
        now - policy.EVENT_RETENTION_MS,
        now - policy.EVENT_RETENTION_MS,
    )

    if method == "POST":
        normalized, error = policy.normalize_event(data, now)
        if error:
            return _response(runtime, {"error": error}, status=400)
        count = await runtime.d1_first(
            "SELECT COUNT(*) AS n FROM world_events")
        if int((count or {}).get("n") or 0) >= policy.MAX_EVENT_RECORDS:
            return _response(runtime, {"error": "event_catalog_full"}, status=409)
        event_id = runtime.new_id()
        public_data = {
            key: normalized[key]
            for key in ("type", "title", "description", "destination")
        }
        await runtime.d1_run(
            "INSERT INTO world_events "
            "(event_id,status,starts_at,ends_at,data,created_by_bi,"
            "created_at,updated_at,cancelled_at) "
            "VALUES (?,'scheduled',?,?,?,?,?,?,0)",
            event_id,
            normalized["startsAtMs"],
            normalized["endsAtMs"],
            json.dumps(public_data, sort_keys=True, separators=(",", ":")),
            account_bi,
            now,
            now,
        )
        _invalidate_public_events()
        await runtime.audit(
            actor,
            "world.event.create",
            "world_event",
            event_id,
            details={"type": normalized["type"]},
        )
        row = await runtime.d1_first(
            "SELECT event_id,status,starts_at,ends_at,data,updated_at "
            "FROM world_events WHERE event_id=?",
            event_id,
        )
        return _response(
            runtime,
            {"ok": True, "event": policy.public_event(row)},
            status=201,
        )

    existing = await runtime.d1_first(
        "SELECT event_id,status,starts_at,ends_at,data,updated_at "
        "FROM world_events WHERE event_id=?",
        target,
    )
    if not existing:
        return _response(runtime, {"error": "not_found"}, status=404)
    if method == "DELETE":
        if str(existing.get("status") or "") == "cancelled":
            return _response(runtime, {"error": "not_found"}, status=404)
        await runtime.d1_run(
            "UPDATE world_events SET status='cancelled',cancelled_at=?,"
            "updated_at=? WHERE event_id=? AND status='scheduled'",
            now, now, target,
        )
        _invalidate_public_events()
        await runtime.audit(
            actor, "world.event.cancel", "world_event", target)
        return _response(runtime, {"ok": True, "cancelled": target})

    current = policy.public_event(existing)
    if not current:
        return _response(runtime, {"error": "not_found"}, status=404)
    current.pop("id", None)
    current.pop("updatedAt", None)
    normalized, error = policy.normalize_event(data, now, current)
    if error:
        return _response(runtime, {"error": error}, status=400)
    public_data = {
        key: normalized[key]
        for key in ("type", "title", "description", "destination")
    }
    await runtime.d1_run(
        "UPDATE world_events SET starts_at=?,ends_at=?,data=?,updated_at=? "
        "WHERE event_id=? AND status='scheduled'",
        normalized["startsAtMs"],
        normalized["endsAtMs"],
        json.dumps(public_data, sort_keys=True, separators=(",", ":")),
        now,
        target,
    )
    _invalidate_public_events()
    await runtime.audit(
        actor,
        "world.event.update",
        "world_event",
        target,
        details={"type": normalized["type"]},
    )
    row = await runtime.d1_first(
        "SELECT event_id,status,starts_at,ends_at,data,updated_at "
        "FROM world_events WHERE event_id=?",
        target,
    )
    return _response(
        runtime, {"ok": True, "event": policy.public_event(row)})


async def cleanup_records(runtime):
    """Purge retained cancelled/expired announcements on the hourly cron."""
    await runtime.ensure_schema()
    now = runtime.now()
    await runtime.d1_run(
        "DELETE FROM world_events WHERE "
        "(status='cancelled' AND cancelled_at<?) OR "
        "(status='scheduled' AND ends_at<?)",
        now - policy.EVENT_RETENTION_MS,
        now - policy.EVENT_RETENTION_MS,
    )
    _invalidate_public_events()
    return {"ok": True, "cleanedAt": now}
