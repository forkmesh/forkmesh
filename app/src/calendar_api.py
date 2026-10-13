"""Private personal and organization calendars shared by every ForkMesh client.

The runtime supplied by ``entry.py`` owns sessions, D1, encryption, and the
notification inbox.  This module keeps the calendar contract and its access
rules small enough to test without a Workers runtime.
"""

import calendar as month_calendar
import datetime
import re
from zoneinfo import ZoneInfo

from organization_rbac import ORG_INSIDER_ROLE_SQL, ORG_INSIDER_ROLES


PREFIX = "/api/calendar"
EVENT_ID_RE = re.compile(r"^[0-9a-f]{32}$")
NAME_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
TIMEZONE_RE = re.compile(r"^[A-Za-z0-9_+.-]+(?:/[A-Za-z0-9_+.-]+)*$")
DEFAULT_REMINDERS = (1440, 60, 10)
ALLOWED_REMINDERS = frozenset({0, 5, 10, 15, 30, 60, 120, 1440, 2880, 10080})
RECURRENCES = frozenset({"none", "daily", "weekly", "monthly", "yearly"})
MAX_EVENTS = 500
MAX_ATTENDEES = 100
MAX_RANGE_MS = 3 * 366 * 24 * 60 * 60 * 1000


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload, status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def _text(value, limit):
    return str(value or "").replace("\x00", "")[:limit]


def _integer(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return "collection", "", ""
    match = re.fullmatch(
        r"/api/calendar/([0-9a-f]{32})(?:/(rsvp))?", clean)
    if not match:
        return None
    return "event", match.group(1), match.group(2) or ""


def _reminders(value):
    if value is None:
        return list(DEFAULT_REMINDERS)
    values = value if isinstance(value, list) else []
    result = []
    for item in values:
        offset = _integer(item, -1)
        if offset in ALLOWED_REMINDERS and offset not in result:
            result.append(offset)
    return sorted(result, reverse=True)[:5]


def _event_data(data, existing=None):
    base = dict(existing or {})
    for key, limit, cleaner in (
        ("title", 200, _clean),
        ("description", 10000, _text),
        ("location", 500, _clean),
        ("meetingUrl", 1000, _clean),
        ("color", 20, _clean),
    ):
        if key in data:
            base[key] = cleaner(data.get(key), limit)
    if "reminders" in data or not existing:
        base["reminders"] = _reminders(data.get("reminders"))
    recurrence = str(data.get("recurrence", base.get("recurrence", "none"))).lower()
    base["recurrence"] = recurrence if recurrence in RECURRENCES else "none"
    if "recurrenceUntil" in data or not existing:
        base["recurrenceUntil"] = max(0, _integer(data.get("recurrenceUntil")))
    return base


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), str((record or {}).get("name") or "").strip().lower()


async def _row(runtime, event_id):
    return await runtime.d1_first(
        "SELECT event_id,owner_bi,owner_name,org_bi,org_name,start_at,end_at,"
        "all_day,timezone,recurring,created_at,updated_at,data "
        "FROM calendar_events WHERE event_id=?", event_id)


async def _org_insider(runtime, org_bi, account_bi):
    member = await runtime.d1_first(
        "SELECT 1 AS one FROM org_members WHERE org_bi=? AND member_bi=? "
        "AND role IN " + ORG_INSIDER_ROLE_SQL, org_bi, account_bi)
    return bool(member)


async def _can_read(runtime, row, account_bi):
    if not row or not account_bi:
        return False
    org_bi = str(row.get("org_bi") or "")
    if str(row.get("owner_bi") or "") == account_bi and not org_bi:
        return True
    attendee = await runtime.d1_first(
        "SELECT 1 AS one FROM calendar_attendees WHERE event_id=? "
        "AND attendee_bi=?", row["event_id"], account_bi)
    if attendee:
        return True
    if not org_bi:
        return False
    return await _org_insider(runtime, org_bi, account_bi)


async def _can_manage(runtime, row, account_bi, actor):
    if not row or not account_bi:
        return False
    org_name = str(row.get("org_name") or "")
    if not org_name:
        return str(row.get("owner_bi") or "") == account_bi
    role = await runtime.org_role(org_name, actor)
    if role in ("owner", "admin"):
        return True
    return (str(row.get("owner_bi") or "") == account_bi
            and role in ORG_INSIDER_ROLES and role != "auditor")


async def _attendees_by_event(runtime, event_ids):
    grouped = {}
    for start in range(0, len(event_ids), 80):
        chunk = event_ids[start:start + 80]
        if not chunk:
            continue
        rows = await runtime.d1_all(
            "SELECT event_id,attendee_name,status,created_at,responded_at "
            "FROM calendar_attendees WHERE event_id IN (" +
            ",".join(["?"] * len(chunk)) + ") ORDER BY attendee_name",
            *chunk)
        for row in rows or []:
            grouped.setdefault(str(row.get("event_id") or ""), []).append({
                "name": str(row.get("attendee_name") or ""),
                "status": str(row.get("status") or "pending"),
                "invitedAt": _integer(row.get("created_at")),
                "respondedAt": _integer(row.get("responded_at")),
            })
    return grouped


async def _project(runtime, row, attendees=None):
    data = await runtime.open(row.get("data"))
    data = data if isinstance(data, dict) else {}
    result = {
        "id": str(row.get("event_id") or ""),
        "owner": str(row.get("owner_name") or ""),
        "calendar": str(row.get("org_name") or "personal"),
        "scope": "organization" if row.get("org_bi") else "personal",
        "startAt": _integer(row.get("start_at")),
        "endAt": _integer(row.get("end_at")),
        "allDay": bool(row.get("all_day")),
        "timezone": str(row.get("timezone") or "UTC"),
        "createdAt": _integer(row.get("created_at")),
        "updatedAt": _integer(row.get("updated_at")),
        "title": str(data.get("title") or "Untitled event"),
        "description": str(data.get("description") or ""),
        "location": str(data.get("location") or ""),
        "meetingUrl": str(data.get("meetingUrl") or ""),
        "color": str(data.get("color") or ""),
        "reminders": _reminders(data.get("reminders")),
        "recurrence": str(data.get("recurrence") or "none"),
        "recurrenceUntil": _integer(data.get("recurrenceUntil")),
        "attendees": attendees or [],
    }
    return result


async def _list(runtime, account_bi, actor):
    now = runtime.now()
    start_at = _integer(runtime.query("start"), now - 90 * 86400000)
    end_at = _integer(runtime.query("end"), now + 400 * 86400000)
    if end_at <= start_at or end_at - start_at > MAX_RANGE_MS:
        return None, "invalid_range"
    rows = await runtime.d1_all(
        "SELECT DISTINCT e.event_id,e.owner_bi,e.owner_name,e.org_bi,e.org_name,"
        "e.start_at,e.end_at,e.all_day,e.timezone,e.recurring,e.created_at,"
        "e.updated_at,e.data FROM calendar_events e "
        "LEFT JOIN calendar_attendees a ON a.event_id=e.event_id "
        "LEFT JOIN org_members m ON m.org_bi=e.org_bi AND m.member_bi=? "
        "AND m.role IN " + ORG_INSIDER_ROLE_SQL + " "
        "WHERE ((e.owner_bi=? AND COALESCE(e.org_bi,'')='') "
        "OR a.attendee_bi=? OR m.member_bi=?) "
        "AND ((e.start_at<? AND e.end_at>?) OR "
        "(e.recurring=1 AND e.start_at<?)) "
        "ORDER BY e.start_at,e.event_id LIMIT ?",
        account_bi, account_bi, account_bi, account_bi,
        end_at, start_at, end_at, MAX_EVENTS)
    rows = list(rows or [])
    attendees = await _attendees_by_event(
        runtime, [str(row.get("event_id") or "") for row in rows])
    events = [await _project(
        runtime, row, attendees.get(str(row.get("event_id") or ""), []))
        for row in rows]
    calendars = [{"id": "personal", "name": "My calendar", "scope": "personal", "role": "owner"}]
    memberships = await runtime.d1_all(
        "SELECT o.org_bi,o.name,m.role FROM org_members m JOIN orgs o ON o.org_bi=m.org_bi "
        "WHERE m.member_bi=? AND m.role IN " + ORG_INSIDER_ROLE_SQL +
        " ORDER BY o.name", account_bi)
    org_ids = [str(row.get("org_bi") or "") for row in memberships or []]
    members_by_org = {}
    if org_ids:
        member_rows = await runtime.d1_all(
            "SELECT org_bi,name FROM org_members WHERE org_bi IN (" +
            ",".join(["?"] * len(org_ids)) + ") ORDER BY created_at,name",
            *org_ids)
        for row in member_rows or []:
            members_by_org.setdefault(str(row.get("org_bi") or ""), []).append(
                str(row.get("name") or ""))
    calendars.extend({
        "id": str(row.get("name") or ""),
        "name": str(row.get("name") or ""),
        "scope": "organization",
        "role": str(row.get("role") or "member"),
        "members": members_by_org.get(str(row.get("org_bi") or ""), []),
    } for row in memberships or [])
    return {"ok": True, "actor": actor, "calendars": calendars, "events": events}, ""


async def _replace_attendees(runtime, event_id, names, actor, title, href):
    cleaned = []
    for value in names if isinstance(names, list) else []:
        name = str(value or "").strip().lower()
        if NAME_RE.fullmatch(name) and name != actor and name not in cleaned:
            cleaned.append(name)
    cleaned = cleaned[:MAX_ATTENDEES]
    existing_rows = await runtime.d1_all(
        "SELECT attendee_name,status FROM calendar_attendees WHERE event_id=?",
        event_id)
    existing = {str(row.get("attendee_name") or ""): str(row.get("status") or "pending")
                for row in existing_rows or []}
    await runtime.d1_run("DELETE FROM calendar_attendees WHERE event_id=?", event_id)
    now = runtime.now()
    accepted = []
    for name in cleaned:
        attendee_bi = await runtime.account(name)
        if not attendee_bi:
            continue
        status = existing.get(name, "pending")
        await runtime.d1_run(
            "INSERT INTO calendar_attendees(event_id,attendee_bi,attendee_name,"
            "status,created_at,responded_at) VALUES(?,?,?,?,?,?)",
            event_id, attendee_bi, name, status, now,
            now if status != "pending" else 0)
        accepted.append(name)
        if name not in existing:
            await runtime.notify(
                name, "calendar_invitation", "Calendar invitation: " + title,
                body=actor + " invited you to an event.", href=href,
                actor=actor, source="calendar",
                dedupe="calendar-invite:" + event_id + ":" + name,
                meta={"eventId": event_id})
    return accepted


async def _create(runtime, data, account_bi, actor):
    title = _clean(data.get("title"), 200)
    start_at = _integer(data.get("startAt"))
    end_at = _integer(data.get("endAt"))
    if not title or start_at <= 0 or end_at <= start_at:
        return _response(runtime, {"error": "invalid_event"}, 400)
    timezone = str(data.get("timezone") or "UTC")[:80]
    if not TIMEZONE_RE.fullmatch(timezone):
        return _response(runtime, {"error": "invalid_timezone"}, 400)
    scope = str(data.get("scope") or "personal").lower()
    calendar_name = str(data.get("calendar") or "").strip().lower()
    # Early desktop and web dialogs could be opened before their asynchronous
    # calendar list loaded, producing scope="organization" with no calendar
    # id. Preserve privacy and compatibility by treating that missing choice as
    # the caller's personal calendar rather than attempting an empty org lookup.
    if scope in ("personal", "organization") and calendar_name in ("", "personal"):
        scope = "personal"
    org_name = calendar_name if scope == "organization" else ""
    org_bi = ""
    if org_name:
        role, org_bi = await runtime.org(org_name, actor)
        if role not in ORG_INSIDER_ROLES or role == "auditor":
            return _response(runtime, {"error": "organization_membership_required"}, 403)
    elif scope != "personal":
        return _response(runtime, {"error": "invalid_calendar"}, 400)
    event_id = runtime.new_id()
    now = runtime.now()
    event_data = _event_data(data)
    event_data["title"] = title
    recurring = 0 if event_data["recurrence"] == "none" else 1
    await runtime.d1_run(
        "INSERT INTO calendar_events(event_id,owner_bi,owner_name,org_bi,org_name,"
        "start_at,end_at,all_day,timezone,recurring,created_at,updated_at,data) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        event_id, account_bi, actor, org_bi, org_name, start_at, end_at,
        1 if data.get("allDay") else 0, timezone, recurring, now, now,
        await runtime.seal(event_data))
    attendees = await _replace_attendees(
        runtime, event_id, data.get("attendees"), actor, title,
        "/dashboard/calendar?event=" + event_id)
    await runtime.audit(actor, "calendar.event.create", "calendar_event", event_id)
    row = await _row(runtime, event_id)
    projected = await _project(runtime, row, [{"name": name, "status": "pending",
                                                "invitedAt": now, "respondedAt": 0}
                                               for name in attendees])
    return _response(runtime, {"ok": True, "event": projected}, 201)


async def _update(runtime, event_id, data, row, actor):
    current = await runtime.open(row.get("data"))
    current = current if isinstance(current, dict) else {}
    event_data = _event_data(data, current)
    if not _clean(event_data.get("title"), 200):
        return _response(runtime, {"error": "invalid_event"}, 400)
    start_at = _integer(data.get("startAt"), _integer(row.get("start_at")))
    end_at = _integer(data.get("endAt"), _integer(row.get("end_at")))
    timezone = str(data.get("timezone", row.get("timezone") or "UTC"))[:80]
    if end_at <= start_at or not TIMEZONE_RE.fullmatch(timezone):
        return _response(runtime, {"error": "invalid_event"}, 400)
    now = runtime.now()
    recurring = 0 if event_data.get("recurrence") == "none" else 1
    await runtime.d1_run(
        "UPDATE calendar_events SET start_at=?,end_at=?,all_day=?,timezone=?,"
        "recurring=?,updated_at=?,data=? WHERE event_id=?",
        start_at, end_at,
        1 if data.get("allDay", bool(row.get("all_day"))) else 0,
        timezone, recurring, now, await runtime.seal(event_data), event_id)
    if "attendees" in data:
        await _replace_attendees(
            runtime, event_id, data.get("attendees"), actor,
            str(event_data.get("title") or "Untitled event"),
            "/dashboard/calendar?event=" + event_id)
    await runtime.d1_run(
        "DELETE FROM calendar_reminder_deliveries WHERE event_id=?", event_id)
    await runtime.audit(actor, "calendar.event.update", "calendar_event", event_id)
    updated = await _row(runtime, event_id)
    attendees = (await _attendees_by_event(runtime, [event_id])).get(event_id, [])
    return _response(runtime, {"ok": True, "event": await _project(runtime, updated, attendees)})


async def handle(runtime, path):
    route = _route(path)
    if not route:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    data = {}
    if method in ("POST", "PATCH", "DELETE"):
        try:
            data = await runtime.json()
        except Exception:
            return _response(runtime, {"error": "invalid_json"}, 400)
    account_bi, actor = await _actor(runtime, data)
    if not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    kind, event_id, action = route
    if kind == "collection":
        if method == "GET":
            payload, error = await _list(runtime, account_bi, actor)
            return _response(runtime, payload if payload else {"error": error}, 200 if payload else 400)
        if method == "POST":
            return await _create(runtime, data, account_bi, actor)
        return _response(runtime, {"error": "method_not_allowed"}, 405, "GET, POST")
    row = await _row(runtime, event_id)
    if not row or not await _can_read(runtime, row, account_bi):
        return _response(runtime, {"error": "not_found"}, 404)
    if action == "rsvp":
        if method != "POST":
            return _response(runtime, {"error": "method_not_allowed"}, 405, "POST")
        status = str(data.get("status") or "").lower()
        if status not in ("accepted", "declined", "tentative"):
            return _response(runtime, {"error": "invalid_rsvp"}, 400)
        result = await runtime.d1_run(
            "UPDATE calendar_attendees SET status=?,responded_at=? "
            "WHERE event_id=? AND attendee_bi=?",
            status, runtime.now(), event_id, account_bi)
        if runtime.changes(result) < 1:
            return _response(runtime, {"error": "invitation_required"}, 403)
        await runtime.notify(
            str(row.get("owner_name") or ""), "calendar_rsvp",
            actor + " responded to your event", body=status,
            href="/dashboard/calendar?event=" + event_id, actor=actor,
            source="calendar", dedupe="calendar-rsvp:" + event_id + ":" + actor + ":" + status,
            meta={"eventId": event_id, "status": status})
        return _response(runtime, {"ok": True, "status": status})
    if method == "GET":
        attendees = (await _attendees_by_event(runtime, [event_id])).get(event_id, [])
        return _response(runtime, {"ok": True, "event": await _project(runtime, row, attendees)})
    if not await _can_manage(runtime, row, account_bi, actor):
        return _response(runtime, {"error": "event_owner_required"}, 403)
    if method == "PATCH":
        return await _update(runtime, event_id, data, row, actor)
    if method == "DELETE":
        await runtime.d1_run("DELETE FROM calendar_attendees WHERE event_id=?", event_id)
        await runtime.d1_run("DELETE FROM calendar_reminder_deliveries WHERE event_id=?", event_id)
        await runtime.d1_run("DELETE FROM calendar_events WHERE event_id=?", event_id)
        await runtime.audit(actor, "calendar.event.delete", "calendar_event", event_id)
        return _response(runtime, {"ok": True, "deleted": True})
    return _response(runtime, {"error": "method_not_allowed"}, 405, "GET, PATCH, DELETE")


def _recurrence_candidate(start_ms, recurrence, target_ms, timezone):
    """Occurrence nearest ``target_ms``, preserving wall time across DST."""
    if recurrence == "none":
        return start_ms
    try:
        zone = ZoneInfo(str(timezone or "UTC"))
    except Exception:
        zone = datetime.timezone.utc
    first = datetime.datetime.fromtimestamp(start_ms / 1000, zone)
    target = datetime.datetime.fromtimestamp(target_ms / 1000, zone)
    if target < first:
        return start_ms
    if recurrence in ("daily", "weekly"):
        step = 1 if recurrence == "daily" else 7
        count = max(0, round((target.date() - first.date()).days / step))
        candidate = first + datetime.timedelta(days=count * step)
    elif recurrence == "monthly":
        count = max(0, (target.year - first.year) * 12 + target.month - first.month)
        year = first.year + (first.month - 1 + count) // 12
        month = (first.month - 1 + count) % 12 + 1
        day = min(first.day, month_calendar.monthrange(year, month)[1])
        candidate = first.replace(year=year, month=month, day=day)
    elif recurrence == "yearly":
        year = max(first.year, target.year)
        day = min(first.day, month_calendar.monthrange(year, first.month)[1])
        candidate = first.replace(year=year, day=day)
    else:
        return start_ms
    return int(candidate.timestamp() * 1000)


async def deliver_reminders(runtime):
    """Enqueue each due event reminder exactly once per recipient and offset."""
    now = runtime.now()
    rows = await runtime.d1_all(
        "SELECT event_id,owner_bi,owner_name,start_at,end_at,timezone,recurring,data FROM "
        "calendar_events WHERE start_at<=? "
        "ORDER BY start_at LIMIT 200",
        now + max(DEFAULT_REMINDERS) * 60000 + 2 * 60000)
    delivered = 0
    for row in rows or []:
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        title = str(data.get("title") or "Untitled event")
        recipients = [(str(row.get("owner_bi") or ""), str(row.get("owner_name") or ""))]
        attendees = await runtime.d1_all(
            "SELECT attendee_bi,attendee_name FROM calendar_attendees "
            "WHERE event_id=? AND status!='declined'", row["event_id"])
        recipients.extend((str(item.get("attendee_bi") or ""),
                           str(item.get("attendee_name") or ""))
                          for item in attendees or [])
        for offset in _reminders(data.get("reminders")):
            target_start = now + offset * 60000
            occurrence_at = _recurrence_candidate(
                _integer(row.get("start_at")),
                str(data.get("recurrence") or "none"), target_start,
                str(row.get("timezone") or "UTC"))
            if (_integer(data.get("recurrenceUntil")) > 0 and
                    occurrence_at > _integer(data.get("recurrenceUntil"))):
                continue
            due = occurrence_at - offset * 60000
            if not (now - 2 * 60000 < due <= now + 60000):
                continue
            for recipient_bi, recipient in recipients:
                if not recipient_bi or not recipient:
                    continue
                prior = await runtime.d1_first(
                    "SELECT 1 AS one FROM calendar_reminder_deliveries WHERE "
                    "event_id=? AND occurrence_at=? AND recipient_bi=? "
                    "AND offset_minutes=?",
                    row["event_id"], occurrence_at, recipient_bi, offset)
                if prior:
                    continue
                await runtime.d1_run(
                    "INSERT INTO calendar_reminder_deliveries(event_id,occurrence_at,"
                    "recipient_bi,offset_minutes,delivered_at) VALUES(?,?,?,?,?)",
                    row["event_id"], occurrence_at, recipient_bi, offset, now)
                label = ("now" if offset == 0 else
                         (str(offset // 1440) + " day" if offset % 1440 == 0 else
                          str(offset // 60) + " hour" if offset % 60 == 0 else
                          str(offset) + " minutes"))
                await runtime.notify(
                    recipient, "calendar_reminder", title,
                    body="Starts " + label + ("" if offset == 0 else " from now"),
                    href="/dashboard/calendar?event=" + str(row["event_id"]),
                    actor=str(row.get("owner_name") or ""), source="calendar",
                    dedupe="calendar-reminder:" + str(row["event_id"]) + ":" +
                    recipient + ":" + str(offset),
                    meta={"eventId": str(row["event_id"]), "offsetMinutes": offset})
                delivered += 1
    return {"ok": True, "delivered": delivered}
