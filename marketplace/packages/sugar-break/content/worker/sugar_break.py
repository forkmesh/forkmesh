import re


PREFIX = "/api/sugar-break"
PLUGIN_ID = "sugar-break"
REQUEST_PROOF = "forkmesh-sugar-break-request-v1"
MAX_BODY_BYTES = 32 * 1024
MAX_EVENTS_PER_DAY = 200
MAX_HISTORY = 20
KEY_VERSION = 1
DEFAULT_LENGTH_DAYS = 30
MIN_LENGTH_DAYS = 1
MAX_LENGTH_DAYS = 365
LENGTH_CHOICES = (7, 14, 21, 30, 60, 90)
KINDS = ("skip", "slip")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
DAY_RE = re.compile(r"^\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])$")

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS sugar_break_keys (
        account_bi TEXT PRIMARY KEY,
        key_version INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        wrapped TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS sugar_break_profiles (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        installed_at INTEGER NOT NULL CHECK(installed_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS sugar_break_breaks (
        break_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        started_at INTEGER NOT NULL CHECK(started_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS sugar_break_events (
        event_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        break_id TEXT NOT NULL,
        day TEXT NOT NULL,
        logged_at INTEGER NOT NULL CHECK(logged_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_sugar_break_breaks_account "
    "ON sugar_break_breaks(account_bi,started_at)",
    "CREATE INDEX IF NOT EXISTS idx_sugar_break_events_break "
    "ON sugar_break_events(account_bi,break_id,day,logged_at)",
]

DATA_SCHEMA_DOWN = [
    "DROP INDEX IF EXISTS idx_sugar_break_events_break",
    "DROP INDEX IF EXISTS idx_sugar_break_breaks_account",
    "DROP TABLE IF EXISTS sugar_break_events",
    "DROP TABLE IF EXISTS sugar_break_breaks",
    "DROP TABLE IF EXISTS sugar_break_profiles",
    "DROP TABLE IF EXISTS sugar_break_keys",
]

_SCHEMA_READY = False


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    return runtime.response(
        payload,
        status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers,
    )


def _clean(value, limit):
    return " ".join(str(value or "").replace("\x00", "").split())[:limit]


def _integer(value, default=0):
    if isinstance(value, bool):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        try:
            number = float(value)
        except (TypeError, ValueError):
            return default
        if number != number or number in (float("inf"), float("-inf")):
            return default
        return int(round(number))


def _day(value, default=""):
    value = _clean(value, 10)
    return value if DAY_RE.fullmatch(value) else default


def _day_ordinal(day):
    year, month, date = (int(part) for part in day.split("-"))
    if month < 3:
        year -= 1
        month += 12
    era = year // 400
    year_of_era = year - era * 400
    day_of_year = (153 * (month - 3) + 2) // 5 + date - 1
    day_of_era = year_of_era * 365 + year_of_era // 4 - year_of_era // 100 + day_of_year
    return era * 146097 + day_of_era - 719468


def _ordinal_day(ordinal):
    value = int(ordinal) + 719468
    era = (value if value >= 0 else value - 146096) // 146097
    day_of_era = value - era * 146097
    year_of_era = (day_of_era - day_of_era // 1460 + day_of_era // 36524
                   - day_of_era // 146096) // 365
    year = year_of_era + era * 400
    day_of_year = day_of_era - (365 * year_of_era + year_of_era // 4 - year_of_era // 100)
    month_index = (5 * day_of_year + 2) // 153
    date = day_of_year - (153 * month_index + 2) // 5 + 1
    month = month_index + 3 if month_index < 10 else month_index - 9
    if month <= 2:
        year += 1
    return "%04d-%02d-%02d" % (year, month, date)


def _day_from_ms(ms, offset_minutes=0):
    return _ordinal_day((int(ms) - int(offset_minutes) * 60_000) // 86_400_000)


def _shift_day(day, delta):
    return _ordinal_day(_day_ordinal(day) + int(delta))


def _length(value, default=DEFAULT_LENGTH_DAYS):
    return max(MIN_LENGTH_DAYS, min(MAX_LENGTH_DAYS, _integer(value, default) or default))


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ()
    if not clean.startswith(PREFIX + "/"):
        return None
    parts = tuple(part for part in clean[len(PREFIX) + 1:].split("/") if part)
    if any(len(part) > 64 for part in parts):
        return None
    return parts


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    return str(account_bi or ""), _clean((record or {}).get("name"), 64).lower()


async def _ensure_tables(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in DATA_SCHEMA_UP:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


class _Vault:
    def __init__(self, runtime):
        self._runtime = runtime
        self._keys = {}

    def __getattr__(self, name):
        return getattr(self._runtime, name)

    async def key(self, account_bi, create=False):
        account_bi = str(account_bi or "")
        if not account_bi:
            return ""
        material = self._keys.get(account_bi)
        if material:
            return material
        material = await self._stored_key(account_bi)
        if not material and create:
            await self._runtime.d1_run(
                "INSERT INTO sugar_break_keys(account_bi,key_version,created_at,wrapped) "
                "VALUES(?,?,?,?) ON CONFLICT(account_bi) DO NOTHING",
                account_bi, KEY_VERSION, self._runtime.now(),
                await self._runtime.seal(
                    {"key": self._runtime.new_secret(), "version": KEY_VERSION}),
            )
            material = await self._stored_key(account_bi)
        if material:
            self._keys[account_bi] = material
        return material

    async def _stored_key(self, account_bi):
        row = await self._runtime.d1_first(
            "SELECT wrapped FROM sugar_break_keys WHERE account_bi=?", account_bi)
        if not row:
            return ""
        opened = await self._runtime.open(row.get("wrapped"))
        return str(opened.get("key") or "") if isinstance(opened, dict) else ""

    async def seal_for(self, account_bi, value):
        material = await self.key(account_bi, create=True)
        if not material:
            raise RuntimeError("sugar-break: no key for this account")
        return await self._runtime.seal_with(material, value)

    async def open_for(self, account_bi, stored):
        if not stored:
            return {}
        material = await self.key(account_bi)
        if not material:
            return {}
        opened = await self._runtime.open_with(material, stored)
        return opened if isinstance(opened, dict) else {}

    def forget(self, account_bi):
        self._keys.pop(str(account_bi or ""), None)


def _vault(runtime):
    return runtime if isinstance(runtime, _Vault) else _Vault(runtime)


def _profile_data(data, existing=None):
    result = {"defaultLengthDays": DEFAULT_LENGTH_DAYS, "tzOffsetMinutes": 0}
    result.update({key: value for key, value in (existing or {}).items() if key in result})
    if "defaultLengthDays" in data:
        result["defaultLengthDays"] = _length(data.get("defaultLengthDays"), result["defaultLengthDays"])
    if "tzOffsetMinutes" in data:
        result["tzOffsetMinutes"] = max(-840, min(840, _integer(data.get("tzOffsetMinutes"), 0)))
    result["defaultLengthDays"] = _length(result["defaultLengthDays"])
    return result


async def _profile_row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_name,installed_at,updated_at,data FROM sugar_break_profiles "
        "WHERE account_bi=?", account_bi)


async def _profile(runtime, account_bi, row=None):
    row = row or await _profile_row(runtime, account_bi)
    if not row:
        return _profile_data({})
    return _profile_data({}, await runtime.open_for(account_bi, row.get("data")))


async def _install(runtime, account_bi, actor, data):
    profile = _profile_data(data if isinstance(data, dict) else {})
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO sugar_break_profiles(account_bi,account_name,installed_at,updated_at,data) "
        "VALUES(?,?,?,?,?) ON CONFLICT(account_bi) DO UPDATE SET "
        "account_name=excluded.account_name",
        account_bi, actor, now, now, await runtime.seal_for(account_bi, profile),
    )
    await runtime.audit(actor, "plugin.install", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {"ok": True, "installed": True, "plugin": PLUGIN_ID})


async def _save_profile(runtime, row, account_bi, actor, data):
    profile = _profile_data(data, await runtime.open_for(account_bi, row.get("data")))
    await runtime.d1_run(
        "UPDATE sugar_break_profiles SET account_name=?,updated_at=?,data=? WHERE account_bi=?",
        actor, runtime.now(), await runtime.seal_for(account_bi, profile), account_bi,
    )
    await runtime.audit(actor, "sugarbreak.profile.save", "sugar_break_profile", actor)
    return profile


def _project_break(row, data, today):
    start = _day(data.get("startDay"), today)
    length = _length(data.get("lengthDays"))
    end = _shift_day(start, length - 1)
    ended_at = max(0, _integer(data.get("endedAt")))
    ended_day = _day(data.get("endedDay")) if ended_at else ""
    if ended_at:
        status = "ended"
    elif today > end:
        status = "completed"
    else:
        status = "active"
    return {
        "id": str(row.get("break_id") or ""),
        "startDay": start,
        "endDay": end,
        "lengthDays": length,
        "startedAt": int(row.get("started_at") or 0),
        "endedAt": ended_at,
        "endedDay": ended_day,
        "status": status,
    }


async def _break_rows(runtime, account_bi, limit=MAX_HISTORY + 1):
    return await runtime.d1_all(
        "SELECT break_id,started_at,data FROM sugar_break_breaks WHERE account_bi=? "
        "ORDER BY started_at DESC LIMIT ?", account_bi, int(limit))


async def _breaks(runtime, account_bi, today, limit=MAX_HISTORY + 1):
    result = []
    for row in await _break_rows(runtime, account_bi, limit) or []:
        result.append(_project_break(row, await runtime.open_for(account_bi, row.get("data")), today))
    return result


async def _break_row(runtime, account_bi, break_id):
    return await runtime.d1_first(
        "SELECT break_id,started_at,data FROM sugar_break_breaks WHERE break_id=? AND account_bi=?",
        break_id, account_bi)


def _project_event(row, data):
    return {
        "id": str(row.get("event_id") or ""),
        "breakId": str(row.get("break_id") or ""),
        "day": str(row.get("day") or ""),
        "loggedAt": int(row.get("logged_at") or 0),
        "kind": str(data.get("kind") or "skip"),
        "note": str(data.get("note") or ""),
        "at": _integer(data.get("at")),
        "source": str(data.get("source") or "manual"),
    }


async def _events(runtime, account_bi, break_id, limit=20000):
    rows = await runtime.d1_all(
        "SELECT event_id,break_id,day,logged_at,data FROM sugar_break_events "
        "WHERE account_bi=? AND break_id=? ORDER BY day,logged_at LIMIT ?",
        account_bi, break_id, int(limit))
    result = []
    for row in rows or []:
        result.append(_project_event(row, await runtime.open_for(account_bi, row.get("data"))))
    return result


def _break_days(item, events, today):
    counts = {}
    for event in events:
        slot = counts.setdefault(event["day"], {"skips": 0, "slips": 0})
        slot["slips" if event["kind"] == "slip" else "skips"] += 1
    last = item["endDay"]
    if item["status"] == "ended" and item["endedDay"]:
        last = min(last, max(item["startDay"], item["endedDay"]))
    days = []
    for ordinal in range(_day_ordinal(item["startDay"]), _day_ordinal(item["endDay"]) + 1):
        day = _ordinal_day(ordinal)
        slot = counts.get(day, {"skips": 0, "slips": 0})
        if slot["slips"]:
            status = "slip"
        elif slot["skips"]:
            status = "sugar-free"
        elif day > today or day > last:
            status = "upcoming"
        else:
            status = "open"
        days.append({"day": day, "status": status, "skips": slot["skips"], "slips": slot["slips"]})
    return days


def _empty_summary(length=0):
    return {"skips": 0, "slips": 0, "sugarFreeDays": 0, "slipDays": 0, "openDays": 0,
            "streak": 0, "bestStreak": 0, "dayNumber": 0, "lengthDays": length,
            "remainingDays": 0, "percent": 0}


def _summary(item, days, today):
    summary = _empty_summary(item["lengthDays"])
    best, run = 0, 0
    for day in days:
        summary["skips"] += day["skips"]
        summary["slips"] += day["slips"]
        if day["status"] == "sugar-free":
            summary["sugarFreeDays"] += 1
            run += 1
            best = max(best, run)
        else:
            if day["status"] == "slip":
                summary["slipDays"] += 1
            elif day["status"] == "open":
                summary["openDays"] += 1
            if day["status"] != "upcoming":
                run = 0
    summary["bestStreak"] = best
    streak = 0
    for index, day in enumerate(reversed([day for day in days if day["day"] <= today])):
        if day["status"] == "sugar-free":
            streak += 1
        elif index == 0 and day["day"] == today and day["status"] == "open":
            continue
        else:
            break
    summary["streak"] = streak
    start, end = _day_ordinal(item["startDay"]), _day_ordinal(item["endDay"])
    now = _day_ordinal(today)
    summary["dayNumber"] = max(0, min(now, end) - start + 1)
    summary["remainingDays"] = max(0, end - max(now, start - 1)) if item["status"] == "active" else 0
    summary["percent"] = summary["sugarFreeDays"] * 100 // max(1, item["lengthDays"])
    return summary


async def _overview(runtime, row, account_bi, actor, today):
    profile = await _profile(runtime, account_bi, row)
    breaks = await _breaks(runtime, account_bi, today)
    current = breaks[0] if breaks else None
    days, events, summary = [], [], _empty_summary()
    today_card = {"day": today, "skips": 0, "slips": 0, "status": "open", "inBreak": False}
    if current:
        all_events = await _events(runtime, account_bi, current["id"])
        days = _break_days(current, all_events, today)
        summary = _summary(current, days, today)
        events = [event for event in all_events if event["day"] == today]
        match = next((day for day in days if day["day"] == today), None)
        if match and current["status"] == "active":
            today_card = dict(match, inBreak=True)
        elif match:
            today_card.update({key: match[key] for key in ("skips", "slips", "status")})
    history = []
    for item in breaks[1:MAX_HISTORY + 1]:
        past_days = _break_days(item, await _events(runtime, account_bi, item["id"]), today)
        history.append(dict(item, summary=_summary(item, past_days, today)))
    return _response(runtime, {
        "ok": True,
        "installed": True,
        "actor": actor,
        "day": today,
        "profile": profile,
        "lengths": list(LENGTH_CHOICES),
        "current": current,
        "days": days,
        "today": today_card,
        "events": events,
        "summary": summary,
        "history": history,
    })


async def _seal_break(runtime, account_bi, item):
    stored = {"startDay": item["startDay"], "lengthDays": item["lengthDays"],
              "endedAt": item["endedAt"], "endedDay": item["endedDay"]}
    return await runtime.seal_for(account_bi, stored)


async def _end_break(runtime, account_bi, item, today):
    ended = dict(item, endedAt=runtime.now(), endedDay=max(item["startDay"], min(today, item["endDay"])),
                 status="ended")
    await runtime.d1_run(
        "UPDATE sugar_break_breaks SET data=? WHERE break_id=? AND account_bi=?",
        await _seal_break(runtime, account_bi, ended), item["id"], account_bi)
    return ended


async def _start_break(runtime, row, account_bi, actor, data, today):
    profile = await _profile(runtime, account_bi, row)
    length = _length(data.get("lengthDays"), profile["defaultLengthDays"])
    start = _day(data.get("startDay")) or today
    if _day_ordinal(start) > _day_ordinal(today) + MAX_LENGTH_DAYS or \
            _day_ordinal(start) < _day_ordinal(today) - MAX_LENGTH_DAYS:
        return _response(runtime, {"error": "invalid_start_day"}, 400)
    for item in await _breaks(runtime, account_bi, today, 5):
        if item["status"] == "active":
            await _end_break(runtime, account_bi, item, today)
    now = runtime.now()
    break_id = runtime.new_id()
    item = _project_break({"break_id": break_id, "started_at": now},
                          {"startDay": start, "lengthDays": length}, today)
    await runtime.d1_run(
        "INSERT INTO sugar_break_breaks(break_id,account_bi,started_at,data) VALUES(?,?,?,?)",
        break_id, account_bi, now, await _seal_break(runtime, account_bi, item))
    await runtime.audit(actor, "sugarbreak.break.start", "sugar_break", actor)
    return _response(runtime, {"ok": True, "break": item}, 201)


async def _update_break(runtime, break_row, account_bi, actor, data, today):
    item = _project_break(break_row, await runtime.open_for(account_bi, break_row.get("data")), today)
    if "lengthDays" in data:
        item = _project_break(break_row, {"startDay": item["startDay"],
                                          "lengthDays": _length(data.get("lengthDays"), item["lengthDays"]),
                                          "endedAt": item["endedAt"], "endedDay": item["endedDay"]}, today)
        await runtime.d1_run(
            "UPDATE sugar_break_breaks SET data=? WHERE break_id=? AND account_bi=?",
            await _seal_break(runtime, account_bi, item), item["id"], account_bi)
    if data.get("end") is True and item["status"] == "active":
        item = await _end_break(runtime, account_bi, item, today)
    await runtime.audit(actor, "sugarbreak.break.update", "sugar_break", actor)
    return _response(runtime, {"ok": True, "break": item})


async def _delete_break(runtime, break_row, account_bi, actor):
    break_id = break_row.get("break_id")
    await runtime.d1_run(
        "DELETE FROM sugar_break_events WHERE break_id=? AND account_bi=?", break_id, account_bi)
    await runtime.d1_run(
        "DELETE FROM sugar_break_breaks WHERE break_id=? AND account_bi=?", break_id, account_bi)
    await runtime.audit(actor, "sugarbreak.break.delete", "sugar_break", actor)
    return _response(runtime, {"ok": True, "deleted": True})


async def _create_event(runtime, account_bi, actor, data, today):
    kind = _clean(data.get("kind"), 8).lower()
    if kind not in KINDS:
        return _response(runtime, {"error": "invalid_kind"}, 400)
    current = next((item for item in await _breaks(runtime, account_bi, today, 1)
                    if item["status"] == "active"), None)
    if not current:
        return _response(runtime, {"error": "no_active_break"}, 409)
    day = _day(data.get("day")) or today
    if day < current["startDay"] or day > current["endDay"] or day > today:
        return _response(runtime, {"error": "day_outside_break"}, 400)
    count = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM sugar_break_events WHERE account_bi=? AND break_id=? AND day=?",
        account_bi, current["id"], day)
    if int((count or {}).get("n") or 0) >= MAX_EVENTS_PER_DAY:
        return _response(runtime, {"error": "day_full"}, 400)
    now = runtime.now()
    stored = {"kind": kind, "note": _clean(data.get("note"), 200),
              "at": max(0, _integer(data.get("at"), now)) or now, "source": "manual"}
    event_id = runtime.new_id()
    await runtime.d1_run(
        "INSERT INTO sugar_break_events(event_id,account_bi,break_id,day,logged_at,data) "
        "VALUES(?,?,?,?,?,?)",
        event_id, account_bi, current["id"], day, now, await runtime.seal_for(account_bi, stored))
    await runtime.audit(actor, "sugarbreak.event.create", "sugar_break_event", actor)
    row = {"event_id": event_id, "break_id": current["id"], "day": day, "logged_at": now}
    return _response(runtime, {"ok": True, "event": _project_event(row, stored)}, 201)


async def _delete_event(runtime, account_bi, actor, event_id):
    row = await runtime.d1_first(
        "SELECT event_id FROM sugar_break_events WHERE event_id=? AND account_bi=?",
        event_id, account_bi)
    if not row:
        return _response(runtime, {"error": "event_not_found"}, 404)
    await runtime.d1_run(
        "DELETE FROM sugar_break_events WHERE event_id=? AND account_bi=?", event_id, account_bi)
    await runtime.audit(actor, "sugarbreak.event.delete", "sugar_break_event", actor)
    return _response(runtime, {"ok": True, "deleted": True})


async def account_installed(runtime, account_bi):
    return bool(await _profile_row(runtime, account_bi))


async def account_has_data(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT 1 AS one FROM sugar_break_breaks WHERE account_bi=? LIMIT 1", account_bi)
    return bool(row)


async def export_account_data(runtime, account_bi, actor):
    runtime = _vault(runtime)
    today = _day_from_ms(runtime.now())
    breaks = []
    for item in await _breaks(runtime, account_bi, today, 100000):
        breaks.append(dict(item, events=await _events(runtime, account_bi, item["id"])))
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-sugar-break-export-v1",
        "account": actor,
        "exportedAt": runtime.now(),
        "profile": await _profile(runtime, account_bi),
        "breaks": breaks,
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    for table in ("sugar_break_events", "sugar_break_breaks", "sugar_break_profiles", "sugar_break_keys"):
        await runtime.d1_run("DELETE FROM " + table + " WHERE account_bi=?", account_bi)
    if isinstance(runtime, _Vault):
        runtime.forget(account_bi)


def _export_filename(actor):
    return "forkmesh-sugar-break-" + actor + ".json"


async def _uninstall(runtime, account_bi, actor):
    had_data = await account_has_data(runtime, account_bi)
    export = await export_account_data(runtime, account_bi, actor) if had_data else None
    await purge_account_data(runtime, account_bi, actor)
    await runtime.audit(actor, "plugin.uninstall", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {
        "ok": True,
        "uninstalled": True,
        "plugin": PLUGIN_ID,
        "hadData": had_data,
        "filename": _export_filename(actor),
        "export": export,
    })


def _allowed_methods(parts):
    head = parts[0] if parts else ""
    if not parts:
        return ("GET",)
    if head in ("install", "uninstall") and len(parts) == 1:
        return ("POST",)
    if head == "profile" and len(parts) == 1:
        return ("GET", "PUT")
    if head == "export" and len(parts) == 1:
        return ("GET",)
    if head == "breaks":
        if len(parts) == 1:
            return ("POST",)
        if len(parts) == 2:
            return ("PATCH", "DELETE")
    if head == "events":
        if len(parts) == 1:
            return ("POST",)
        if len(parts) == 2:
            return ("DELETE",)
    return ()


async def handle(runtime, path):
    runtime = _vault(runtime)
    parts = _route(path)
    if parts is None:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    allowed = _allowed_methods(parts)
    if not allowed:
        return _response(runtime, {"error": "not_found"}, 404)
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(runtime, {"error": error},
                             413 if error == "payload_too_large" else 400)
        if not isinstance(data, dict):
            data = {}
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    await _ensure_tables(runtime)
    params = runtime.query_params() if hasattr(runtime, "query_params") else {}
    head = parts[0] if parts else ""
    if head == "install":
        return await _install(runtime, account_bi, actor, data)
    row = await _profile_row(runtime, account_bi)
    if not row:
        if not parts:
            return _response(runtime, {"ok": True, "installed": False, "actor": actor})
        return _response(runtime, {"error": "plugin_not_installed", "canInstall": True}, 409)
    today = _day(params.get("day")) or _day(data.get("today"))
    if not today:
        profile = await _profile(runtime, account_bi, row)
        today = _day_from_ms(runtime.now(), profile["tzOffsetMinutes"])
    if not parts:
        return await _overview(runtime, row, account_bi, actor, today)
    if head == "uninstall":
        return await _uninstall(runtime, account_bi, actor)
    if head == "profile":
        if method == "GET":
            return _response(runtime, {"ok": True, "profile": await _profile(runtime, account_bi, row)})
        profile = await _save_profile(runtime, row, account_bi, actor, data)
        return _response(runtime, {"ok": True, "profile": profile})
    if head == "export":
        return _response(runtime, {
            "ok": True, "filename": _export_filename(actor),
            "export": await export_account_data(runtime, account_bi, actor),
        })
    if head == "breaks":
        if len(parts) == 1:
            return await _start_break(runtime, row, account_bi, actor, data, today)
        break_row = await _break_row(runtime, account_bi, parts[1]) if ID_RE.fullmatch(parts[1]) else None
        if not break_row:
            return _response(runtime, {"error": "break_not_found"}, 404)
        if method == "DELETE":
            return await _delete_break(runtime, break_row, account_bi, actor)
        return await _update_break(runtime, break_row, account_bi, actor, data, today)
    if len(parts) == 1:
        return await _create_event(runtime, account_bi, actor, data, today)
    if not ID_RE.fullmatch(parts[1]):
        return _response(runtime, {"error": "event_not_found"}, 404)
    return await _delete_event(runtime, account_bi, actor, parts[1])
