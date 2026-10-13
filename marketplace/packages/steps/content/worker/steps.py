import re


PREFIX = "/api/steps"
PLUGIN_ID = "steps"
REQUEST_PROOF = "forkmesh-steps-request-v1"
MAX_BODY_BYTES = 64 * 1024
MAX_SYNC_DAYS = 62
MAX_RANGE_DAYS = 400
OVERVIEW_DAYS = 30
KEY_VERSION = 1
DEFAULT_GOAL = 10000
MAX_STEPS = 200000
MAX_DISTANCE_M = 300000.0
MAX_FLOORS = 2000
MAX_ACTIVE_KCAL = 20000.0
FIRST_DAY = "0000-01-01"
LAST_DAY = "9999-12-31"
SOURCES = ("health", "manual")
SOURCE_LABELS = {"health": "Apple Health", "manual": "Added by hand"}
DEVICE_RE = re.compile(r"^[a-z][a-z0-9-]{0,31}$")
DAY_RE = re.compile(r"^\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])$")
IMPERIAL_COUNTRIES = frozenset({"US", "LR", "MM", "GB"})
UNIT_CHOICES = ("auto", "metric", "imperial")

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS steps_keys (
        account_bi TEXT PRIMARY KEY,
        key_version INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        wrapped TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS steps_profiles (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        installed_at INTEGER NOT NULL CHECK(installed_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS steps_days (
        account_bi TEXT NOT NULL,
        day TEXT NOT NULL,
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL,
        PRIMARY KEY(account_bi,day))""",
]

DATA_SCHEMA_DOWN = [
    "DROP TABLE IF EXISTS steps_days",
    "DROP TABLE IF EXISTS steps_profiles",
    "DROP TABLE IF EXISTS steps_keys",
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
        return int(number)


def _number(value, default=0.0, low=0.0, high=100000.0):
    if isinstance(value, bool):
        return default
    try:
        result = float(value)
    except (TypeError, ValueError):
        return default
    if result != result or result in (float("inf"), float("-inf")):
        return default
    return round(min(max(result, low), high), 1)


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


def _day_from_ms(ms):
    return _ordinal_day(int(ms) // 86_400_000)


def _shift_day(day, delta):
    return _ordinal_day(_day_ordinal(day) + int(delta))


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
                "INSERT INTO steps_keys(account_bi,key_version,created_at,wrapped) "
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
            "SELECT wrapped FROM steps_keys WHERE account_bi=?", account_bi)
        if not row:
            return ""
        opened = await self._runtime.open(row.get("wrapped"))
        return str(opened.get("key") or "") if isinstance(opened, dict) else ""

    async def seal_for(self, account_bi, value):
        material = await self.key(account_bi, create=True)
        if not material:
            raise RuntimeError("steps: no key for this account")
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


def _country(runtime):
    reader = getattr(runtime, "country", None)
    if not callable(reader):
        return ""
    try:
        code = str(reader() or "").strip().upper()
    except Exception:
        return ""
    return code if len(code) == 2 and code.isalpha() else ""


def _profile_data(data, existing=None, country=""):
    result = {"goal": DEFAULT_GOAL, "unitsPreference": "auto", "tzOffsetMinutes": 0}
    result.update(existing or {})
    if "goal" in data:
        result["goal"] = max(0, min(MAX_STEPS, _integer(data.get("goal"), result["goal"])))
    if "unitsPreference" in data:
        chosen = _clean(data.get("unitsPreference"), 16).lower()
        if chosen in UNIT_CHOICES:
            result["unitsPreference"] = chosen
    if "tzOffsetMinutes" in data:
        result["tzOffsetMinutes"] = max(-840, min(840, _integer(data.get("tzOffsetMinutes"), 0)))
    preference = result.get("unitsPreference")
    if preference not in UNIT_CHOICES:
        preference = result["unitsPreference"] = "auto"
    if preference == "auto":
        result["units"] = "imperial" if country in IMPERIAL_COUNTRIES else "metric"
    else:
        result["units"] = preference
    return result


async def _profile_row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_name,installed_at,updated_at,data FROM steps_profiles "
        "WHERE account_bi=?", account_bi)


async def _profile(runtime, account_bi, row=None):
    row = row or await _profile_row(runtime, account_bi)
    if not row:
        return _profile_data({}, None, _country(runtime))
    stored = await runtime.open_for(account_bi, row.get("data"))
    return _profile_data({}, stored, _country(runtime))


async def _install(runtime, account_bi, actor, data):
    profile = _profile_data(data if isinstance(data, dict) else {}, None, _country(runtime))
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO steps_profiles(account_bi,account_name,installed_at,updated_at,data) "
        "VALUES(?,?,?,?,?) ON CONFLICT(account_bi) DO UPDATE SET "
        "account_name=excluded.account_name",
        account_bi, actor, now, now, await runtime.seal_for(account_bi, profile),
    )
    await runtime.audit(actor, "plugin.install", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {"ok": True, "installed": True, "plugin": PLUGIN_ID})


async def _save_profile(runtime, row, account_bi, actor, data):
    stored = await runtime.open_for(account_bi, row.get("data"))
    profile = _profile_data(data, stored, _country(runtime))
    await runtime.d1_run(
        "UPDATE steps_profiles SET account_name=?,updated_at=?,data=? WHERE account_bi=?",
        actor, runtime.now(), await runtime.seal_for(account_bi, profile), account_bi,
    )
    await runtime.audit(actor, "steps.profile.save", "steps_profile", actor)
    return profile


def _reading(value, updated_at=0, device=""):
    value = value if isinstance(value, dict) else {}
    reading = {
        "steps": max(0, min(MAX_STEPS, _integer(value.get("steps")))),
        "distanceM": _number(value.get("distanceM"), 0.0, 0.0, MAX_DISTANCE_M),
        "floors": max(0, min(MAX_FLOORS, _integer(value.get("floors")))),
        "activeKcal": _number(value.get("activeKcal"), 0.0, 0.0, MAX_ACTIVE_KCAL),
        "updatedAt": max(0, _integer(value.get("updatedAt"), updated_at)),
    }
    device = _clean(value.get("device") or device, 32).lower()
    if DEVICE_RE.fullmatch(device):
        reading["device"] = device
    return reading


def _project_day(day, sources):
    clean = {}
    for source in SOURCES:
        if isinstance(sources.get(source), dict):
            clean[source] = _reading(sources[source])
    return {
        "day": day,
        "steps": min(MAX_STEPS, sum(item["steps"] for item in clean.values())),
        "distanceM": round(sum(item["distanceM"] for item in clean.values()), 1),
        "floors": sum(item["floors"] for item in clean.values()),
        "activeKcal": round(sum(item["activeKcal"] for item in clean.values()), 1),
        "sources": clean,
    }


async def _day_sources(runtime, account_bi, day):
    row = await runtime.d1_first(
        "SELECT data FROM steps_days WHERE account_bi=? AND day=?", account_bi, day)
    if not row:
        return {}
    opened = await runtime.open_for(account_bi, row.get("data"))
    return opened.get("sources") if isinstance(opened.get("sources"), dict) else {}


async def _store_day(runtime, account_bi, day, sources):
    if not sources:
        await runtime.d1_run(
            "DELETE FROM steps_days WHERE account_bi=? AND day=?", account_bi, day)
        return _project_day(day, {})
    await runtime.d1_run(
        "INSERT INTO steps_days(account_bi,day,updated_at,data) VALUES(?,?,?,?) "
        "ON CONFLICT(account_bi,day) DO UPDATE SET updated_at=excluded.updated_at,"
        "data=excluded.data",
        account_bi, day, runtime.now(),
        await runtime.seal_for(account_bi, {"sources": sources}),
    )
    return _project_day(day, sources)


async def _days(runtime, account_bi, start, end):
    rows = await runtime.d1_all(
        "SELECT day,data FROM steps_days WHERE account_bi=? AND day>=? AND day<=? "
        "ORDER BY day", account_bi, start, end)
    result = {}
    for row in rows or []:
        day = str(row.get("day") or "")
        opened = await runtime.open_for(account_bi, row.get("data"))
        sources = opened.get("sources") if isinstance(opened.get("sources"), dict) else {}
        result[day] = _project_day(day, sources)
    return result


def _series(days, start, end):
    first, last = _day_ordinal(start), _day_ordinal(end)
    series = []
    for ordinal in range(first, last + 1):
        day = _ordinal_day(ordinal)
        series.append(days.get(day) or _project_day(day, {}))
    return series


def _streak(series, goal):
    if goal <= 0:
        return 0
    streak = 0
    for index, item in enumerate(reversed(series)):
        if item["steps"] >= goal:
            streak += 1
        elif index == 0:
            continue
        else:
            break
    return streak


async def _overview(runtime, row, account_bi, actor, today):
    profile = await _profile(runtime, account_bi, row)
    start = _shift_day(today, -(OVERVIEW_DAYS - 1))
    series = _series(await _days(runtime, account_bi, start, today), start, today)
    week = series[-7:]
    logged = [item for item in series if item["steps"] > 0]
    best = max(series, key=lambda item: item["steps"]) if logged else None
    goal = profile["goal"]
    return _response(runtime, {
        "ok": True,
        "installed": True,
        "actor": actor,
        "day": today,
        "profile": profile,
        "today": series[-1],
        "days": series,
        "summary": {
            "weekSteps": sum(item["steps"] for item in week),
            "weekDistanceM": round(sum(item["distanceM"] for item in week), 1),
            "averageSteps": (sum(item["steps"] for item in logged) // len(logged)) if logged else 0,
            "bestDay": best["day"] if best else "",
            "bestSteps": best["steps"] if best else 0,
            "goalDays": sum(1 for item in series if goal > 0 and item["steps"] >= goal),
            "streak": _streak(series, goal),
        },
        "sourceLabels": SOURCE_LABELS,
    })


async def _sync(runtime, account_bi, actor, data):
    source = _clean(data.get("source"), 16).lower() or "health"
    if source != "health":
        return _response(runtime, {"error": "invalid_source"}, 400)
    items = data.get("days")
    if not isinstance(items, list) or not items:
        return _response(runtime, {"error": "no_days"}, 400)
    if len(items) > MAX_SYNC_DAYS:
        return _response(runtime, {"error": "too_many_days", "max": MAX_SYNC_DAYS}, 400)
    device = _clean(data.get("device"), 32).lower()
    now = runtime.now()
    latest = {}
    for item in items:
        if not isinstance(item, dict):
            continue
        day = _day(item.get("day"))
        if day:
            latest[day] = item
    if not latest:
        return _response(runtime, {"error": "no_days"}, 400)
    synced = []
    for day in sorted(latest):
        sources = await _day_sources(runtime, account_bi, day)
        sources = dict(sources)
        sources[source] = _reading(dict(latest[day], updatedAt=now), now, device)
        if sources[source]["steps"] == 0 and sources[source]["distanceM"] == 0:
            sources.pop(source)
        synced.append(await _store_day(runtime, account_bi, day, sources))
    await runtime.audit(actor, "steps.sync", "steps_day", actor)
    return _response(runtime, {"ok": True, "synced": len(synced), "days": synced})


async def _set_manual(runtime, account_bi, actor, day, data):
    sources = dict(await _day_sources(runtime, account_bi, day))
    reading = _reading(dict(data, updatedAt=runtime.now()), runtime.now())
    reading.pop("device", None)
    if reading["steps"] == 0 and reading["distanceM"] == 0:
        sources.pop("manual", None)
    else:
        sources["manual"] = reading
    projected = await _store_day(runtime, account_bi, day, sources)
    await runtime.audit(actor, "steps.day.save", "steps_day", actor)
    return _response(runtime, {"ok": True, "day": projected})


async def _delete_day(runtime, account_bi, actor, day, source):
    sources = dict(await _day_sources(runtime, account_bi, day))
    if source in SOURCES:
        sources.pop(source, None)
    elif source in ("", "all"):
        sources = {}
    else:
        return _response(runtime, {"error": "invalid_source"}, 400)
    projected = await _store_day(runtime, account_bi, day, sources)
    await runtime.audit(actor, "steps.day.delete", "steps_day", actor)
    return _response(runtime, {"ok": True, "deleted": True, "day": projected})


async def account_installed(runtime, account_bi):
    return bool(await _profile_row(runtime, account_bi))


async def account_has_data(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT 1 AS one FROM steps_days WHERE account_bi=? LIMIT 1", account_bi)
    return bool(row)


async def export_account_data(runtime, account_bi, actor):
    runtime = _vault(runtime)
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-steps-export-v1",
        "account": actor,
        "exportedAt": runtime.now(),
        "profile": await _profile(runtime, account_bi),
        "days": list((await _days(runtime, account_bi, FIRST_DAY, LAST_DAY)).values()),
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    for table in ("steps_days", "steps_profiles", "steps_keys"):
        await runtime.d1_run("DELETE FROM " + table + " WHERE account_bi=?", account_bi)
    if isinstance(runtime, _Vault):
        runtime.forget(account_bi)


def _export_filename(actor):
    return "forkmesh-steps-" + actor + ".json"


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
    if head in ("install", "uninstall", "sync") and len(parts) == 1:
        return ("POST",)
    if head == "profile" and len(parts) == 1:
        return ("GET", "PUT")
    if head == "export" and len(parts) == 1:
        return ("GET",)
    if head == "days":
        if len(parts) == 1:
            return ("GET",)
        if len(parts) == 2:
            return ("PUT", "DELETE")
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
    await runtime.ensure_schema()
    account_bi, actor = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    await _ensure_tables(runtime)
    params = runtime.query_params() if hasattr(runtime, "query_params") else {}
    today = _day(params.get("day")) or _day(data.get("day")) or _day_from_ms(runtime.now())
    head = parts[0] if parts else ""
    if head == "install":
        return await _install(runtime, account_bi, actor, data)
    row = await _profile_row(runtime, account_bi)
    if not row:
        if not parts:
            return _response(runtime, {"ok": True, "installed": False, "actor": actor})
        return _response(runtime, {"error": "plugin_not_installed", "canInstall": True}, 409)
    if not parts:
        return await _overview(runtime, row, account_bi, actor, today)
    if head == "uninstall":
        return await _uninstall(runtime, account_bi, actor)
    if head == "profile":
        if method == "GET":
            return _response(runtime, {"ok": True, "profile": await _profile(runtime, account_bi, row)})
        profile = await _save_profile(runtime, row, account_bi, actor, data)
        return _response(runtime, {"ok": True, "profile": profile})
    if head == "sync":
        return await _sync(runtime, account_bi, actor, data)
    if head == "export":
        return _response(runtime, {
            "ok": True, "filename": _export_filename(actor),
            "export": await export_account_data(runtime, account_bi, actor),
        })
    if len(parts) == 1:
        end = _day(params.get("to")) or today
        start = _day(params.get("from")) or _shift_day(end, -(OVERVIEW_DAYS - 1))
        if start > end:
            start, end = end, start
        if _day_ordinal(end) - _day_ordinal(start) > MAX_RANGE_DAYS:
            start = _shift_day(end, -MAX_RANGE_DAYS)
        return _response(runtime, {
            "ok": True, "from": start, "to": end,
            "days": _series(await _days(runtime, account_bi, start, end), start, end),
        })
    day = _day(parts[1])
    if not day:
        return _response(runtime, {"error": "invalid_day"}, 400)
    if method == "DELETE":
        return await _delete_day(runtime, account_bi, actor, day,
                                 _clean(params.get("source"), 16).lower() or "manual")
    return await _set_manual(runtime, account_bi, actor, day, data)
