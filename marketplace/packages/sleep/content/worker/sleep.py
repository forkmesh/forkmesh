import math
import re


PREFIX = "/api/sleep"
PLUGIN_ID = "sleep"
REQUEST_PROOF = "forkmesh-sleep-request-v1"
MAX_BODY_BYTES = 64 * 1024
MAX_SYNC_NIGHTS = 62
MAX_RANGE_DAYS = 400
OVERVIEW_DAYS = 30
KEY_VERSION = 1
DEFAULT_GOAL_MINUTES = 480
MAX_GOAL_MINUTES = 960
MAX_SPAN_MS = 20 * 3_600_000
MAX_INTERRUPTIONS = 500
DEFAULT_BEDTIME = "23:00"
DEFAULT_WAKE_TIME = "07:00"
FIRST_DAY = "0000-01-01"
LAST_DAY = "9999-12-31"
SOURCES = ("manual", "health", "phone")
SYNC_SOURCES = ("health", "phone")
SOURCE_LABELS = {
    "manual": "Added by hand",
    "health": "Apple Health",
    "phone": "Phone at rest",
}
CONFIDENCE = ("high", "medium", "low")
STAGES = ("deepMinutes", "remMinutes", "coreMinutes")
DEVICE_RE = re.compile(r"^[a-z][a-z0-9-]{0,31}$")
DAY_RE = re.compile(r"^\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])$")
CLOCK_RE = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$")

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS sleep_keys (
        account_bi TEXT PRIMARY KEY,
        key_version INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        wrapped TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS sleep_profiles (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        installed_at INTEGER NOT NULL CHECK(installed_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS sleep_nights (
        account_bi TEXT NOT NULL,
        night TEXT NOT NULL,
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL,
        PRIMARY KEY(account_bi,night))""",
]

DATA_SCHEMA_DOWN = [
    "DROP TABLE IF EXISTS sleep_nights",
    "DROP TABLE IF EXISTS sleep_profiles",
    "DROP TABLE IF EXISTS sleep_keys",
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


def _day(value, default=""):
    value = _clean(value, 10)
    return value if DAY_RE.fullmatch(value) else default


def _clock(value, default):
    value = _clean(value, 5)
    return value if CLOCK_RE.fullmatch(value) else default


def _clock_minutes(value):
    hours, minutes = value.split(":")
    return int(hours) * 60 + int(minutes)


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


def _local_day(ms, tz_offset):
    return _ordinal_day((int(ms) // 60_000 - int(tz_offset)) // 1440)


def _local_minutes(ms, tz_offset):
    return (int(ms) // 60_000 - int(tz_offset)) % 1440


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
                "INSERT INTO sleep_keys(account_bi,key_version,created_at,wrapped) "
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
            "SELECT wrapped FROM sleep_keys WHERE account_bi=?", account_bi)
        if not row:
            return ""
        opened = await self._runtime.open(row.get("wrapped"))
        return str(opened.get("key") or "") if isinstance(opened, dict) else ""

    async def seal_for(self, account_bi, value):
        material = await self.key(account_bi, create=True)
        if not material:
            raise RuntimeError("sleep: no key for this account")
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
    result = {
        "goalMinutes": DEFAULT_GOAL_MINUTES,
        "bedtime": DEFAULT_BEDTIME,
        "wakeTime": DEFAULT_WAKE_TIME,
        "tzOffsetMinutes": 0,
    }
    result.update(existing or {})
    if "goalMinutes" in data:
        result["goalMinutes"] = max(0, min(MAX_GOAL_MINUTES,
                                           _integer(data.get("goalMinutes"), result["goalMinutes"])))
    if "bedtime" in data:
        result["bedtime"] = _clock(data.get("bedtime"), result["bedtime"])
    if "wakeTime" in data:
        result["wakeTime"] = _clock(data.get("wakeTime"), result["wakeTime"])
    if "tzOffsetMinutes" in data:
        result["tzOffsetMinutes"] = max(-840, min(840, _integer(data.get("tzOffsetMinutes"), 0)))
    result["bedtime"] = _clock(result.get("bedtime"), DEFAULT_BEDTIME)
    result["wakeTime"] = _clock(result.get("wakeTime"), DEFAULT_WAKE_TIME)
    window = (_clock_minutes(result["wakeTime"]) - _clock_minutes(result["bedtime"])) % 1440
    result["scheduleMinutes"] = window
    return result


async def _profile_row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_name,installed_at,updated_at,data FROM sleep_profiles "
        "WHERE account_bi=?", account_bi)


async def _profile(runtime, account_bi, row=None):
    row = row or await _profile_row(runtime, account_bi)
    if not row:
        return _profile_data({})
    stored = await runtime.open_for(account_bi, row.get("data"))
    return _profile_data({}, stored)


async def _install(runtime, account_bi, actor, data):
    profile = _profile_data(data if isinstance(data, dict) else {})
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO sleep_profiles(account_bi,account_name,installed_at,updated_at,data) "
        "VALUES(?,?,?,?,?) ON CONFLICT(account_bi) DO UPDATE SET "
        "account_name=excluded.account_name",
        account_bi, actor, now, now, await runtime.seal_for(account_bi, profile),
    )
    await runtime.audit(actor, "plugin.install", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {"ok": True, "installed": True, "plugin": PLUGIN_ID})


async def _save_profile(runtime, row, account_bi, actor, data):
    stored = await runtime.open_for(account_bi, row.get("data"))
    profile = _profile_data(data, stored)
    await runtime.d1_run(
        "UPDATE sleep_profiles SET account_name=?,updated_at=?,data=? WHERE account_bi=?",
        actor, runtime.now(), await runtime.seal_for(account_bi, profile), account_bi,
    )
    await runtime.audit(actor, "sleep.profile.save", "sleep_profile", actor)
    return profile


def _reading(value, updated_at=0, device=""):
    value = value if isinstance(value, dict) else {}
    bed_at = _integer(value.get("bedAt"))
    wake_at = _integer(value.get("wakeAt"))
    if bed_at <= 0 or wake_at <= bed_at or wake_at - bed_at > MAX_SPAN_MS:
        return None
    span = (wake_at - bed_at) // 60_000
    awake = max(0, min(span, _integer(value.get("awakeMinutes"))))
    if value.get("asleepMinutes") is None:
        asleep = span - awake
    else:
        asleep = max(0, min(span, _integer(value.get("asleepMinutes"))))
    reading = {
        "bedAt": bed_at,
        "wakeAt": wake_at,
        "asleepMinutes": asleep,
        "awakeMinutes": awake,
        "interruptions": max(0, min(MAX_INTERRUPTIONS, _integer(value.get("interruptions")))),
        "updatedAt": max(0, _integer(value.get("updatedAt"), updated_at)),
    }
    for stage in STAGES:
        minutes = max(0, min(span, _integer(value.get(stage))))
        if minutes:
            reading[stage] = minutes
    confidence = _clean(value.get("confidence"), 8).lower()
    if confidence in CONFIDENCE:
        reading["confidence"] = confidence
    device = _clean(value.get("device") or device, 32).lower()
    if DEVICE_RE.fullmatch(device):
        reading["device"] = device
    return reading


def _empty_night(night):
    return {
        "night": night, "source": "", "bedAt": 0, "wakeAt": 0, "asleepMinutes": 0,
        "awakeMinutes": 0, "interruptions": 0, "sources": {},
    }


def _project_night(night, sources):
    clean = {}
    for source in SOURCES:
        reading = _reading(sources.get(source)) if isinstance(sources.get(source), dict) else None
        if reading:
            clean[source] = reading
    result = _empty_night(night)
    result["sources"] = clean
    for source in SOURCES:
        if source in clean:
            chosen = clean[source]
            result.update({key: value for key, value in chosen.items()
                           if key not in ("updatedAt", "device")})
            result["source"] = source
            break
    return result


async def _night_sources(runtime, account_bi, night):
    row = await runtime.d1_first(
        "SELECT data FROM sleep_nights WHERE account_bi=? AND night=?", account_bi, night)
    if not row:
        return {}
    opened = await runtime.open_for(account_bi, row.get("data"))
    return opened.get("sources") if isinstance(opened.get("sources"), dict) else {}


async def _store_night(runtime, account_bi, night, sources):
    if not sources:
        await runtime.d1_run(
            "DELETE FROM sleep_nights WHERE account_bi=? AND night=?", account_bi, night)
        return _project_night(night, {})
    await runtime.d1_run(
        "INSERT INTO sleep_nights(account_bi,night,updated_at,data) VALUES(?,?,?,?) "
        "ON CONFLICT(account_bi,night) DO UPDATE SET updated_at=excluded.updated_at,"
        "data=excluded.data",
        account_bi, night, runtime.now(),
        await runtime.seal_for(account_bi, {"sources": sources}),
    )
    return _project_night(night, sources)


async def _nights(runtime, account_bi, start, end):
    rows = await runtime.d1_all(
        "SELECT night,data FROM sleep_nights WHERE account_bi=? AND night>=? AND night<=? "
        "ORDER BY night", account_bi, start, end)
    result = {}
    for row in rows or []:
        night = str(row.get("night") or "")
        opened = await runtime.open_for(account_bi, row.get("data"))
        sources = opened.get("sources") if isinstance(opened.get("sources"), dict) else {}
        result[night] = _project_night(night, sources)
    return result


def _series(nights, start, end):
    first, last = _day_ordinal(start), _day_ordinal(end)
    return [nights.get(_ordinal_day(ordinal)) or _empty_night(_ordinal_day(ordinal))
            for ordinal in range(first, last + 1)]


def _streak(series, goal):
    if goal <= 0:
        return 0
    streak = 0
    for index, item in enumerate(reversed(series)):
        if item["asleepMinutes"] >= goal:
            streak += 1
        elif index == 0 and not item["asleepMinutes"]:
            continue
        else:
            break
    return streak


def _clock_mean(minutes):
    if not minutes:
        return -1
    x = sum(math.cos(value * 2 * math.pi / 1440) for value in minutes)
    y = sum(math.sin(value * 2 * math.pi / 1440) for value in minutes)
    if abs(x) < 1e-9 and abs(y) < 1e-9:
        return -1
    return int(round(math.atan2(y, x) * 1440 / (2 * math.pi))) % 1440


def _clock_spread(minutes, mean):
    if len(minutes) < 2 or mean < 0:
        return 0
    deltas = [min((value - mean) % 1440, (mean - value) % 1440) for value in minutes]
    return int(round(sum(deltas) / len(deltas)))


def _average(values):
    return int(round(sum(values) / len(values))) if values else 0


def _summary(series, profile):
    goal = profile["goalMinutes"]
    tz = profile["tzOffsetMinutes"]
    logged = [item for item in series if item["asleepMinutes"] > 0]
    week = [item for item in series[-7:] if item["asleepMinutes"] > 0]
    best = max(logged, key=lambda item: item["asleepMinutes"]) if logged else None
    bedtimes = [_local_minutes(item["bedAt"], tz) for item in logged]
    wakes = [_local_minutes(item["wakeAt"], tz) for item in logged]
    bedtime = _clock_mean(bedtimes)
    return {
        "loggedNights": len(logged),
        "averageMinutes": _average([item["asleepMinutes"] for item in logged]),
        "weekAverageMinutes": _average([item["asleepMinutes"] for item in week]),
        "weekNights": len(week),
        "debtMinutes": sum(max(0, goal - item["asleepMinutes"]) for item in week) if goal > 0 else 0,
        "goalNights": sum(1 for item in logged if goal > 0 and item["asleepMinutes"] >= goal),
        "streak": _streak(series, goal),
        "bestNight": best["night"] if best else "",
        "bestMinutes": best["asleepMinutes"] if best else 0,
        "averageBedtimeMinutes": bedtime,
        "averageWakeMinutes": _clock_mean(wakes),
        "bedtimeSpreadMinutes": _clock_spread(bedtimes, bedtime),
    }


async def _overview(runtime, row, account_bi, actor, today):
    profile = await _profile(runtime, account_bi, row)
    start = _shift_day(today, -(OVERVIEW_DAYS - 1))
    series = _series(await _nights(runtime, account_bi, start, today), start, today)
    return _response(runtime, {
        "ok": True,
        "installed": True,
        "actor": actor,
        "night": today,
        "profile": profile,
        "lastNight": series[-1],
        "nights": series,
        "summary": _summary(series, profile),
        "sourceLabels": SOURCE_LABELS,
    })


async def _sync(runtime, row, account_bi, actor, data):
    source = _clean(data.get("source"), 16).lower()
    if source not in SYNC_SOURCES:
        return _response(runtime, {"error": "invalid_source"}, 400)
    items = data.get("nights")
    if not isinstance(items, list) or not items:
        return _response(runtime, {"error": "no_nights"}, 400)
    if len(items) > MAX_SYNC_NIGHTS:
        return _response(runtime, {"error": "too_many_nights", "max": MAX_SYNC_NIGHTS}, 400)
    device = _clean(data.get("device"), 32).lower()
    now = runtime.now()
    latest = {}
    for item in items:
        if not isinstance(item, dict):
            continue
        night = _day(item.get("night"))
        if night:
            latest[night] = item
    if not latest:
        return _response(runtime, {"error": "no_nights"}, 400)
    if "tzOffsetMinutes" in data:
        stored = await runtime.open_for(account_bi, row.get("data"))
        offset = max(-840, min(840, _integer(data.get("tzOffsetMinutes"), 0)))
        if _profile_data({}, stored)["tzOffsetMinutes"] != offset:
            await _save_profile(runtime, row, account_bi, actor, {"tzOffsetMinutes": offset})
    synced = []
    for night in sorted(latest):
        sources = dict(await _night_sources(runtime, account_bi, night))
        reading = _reading(dict(latest[night], updatedAt=now), now, device)
        if reading and reading["asleepMinutes"] > 0:
            sources[source] = reading
        else:
            sources.pop(source, None)
        synced.append(await _store_night(runtime, account_bi, night, sources))
    await runtime.audit(actor, "sleep.sync", "sleep_night", actor)
    return _response(runtime, {"ok": True, "synced": len(synced), "nights": synced})


async def _set_manual(runtime, account_bi, actor, night, data):
    reading = _reading(dict(data, updatedAt=runtime.now()), runtime.now())
    if not reading or reading["asleepMinutes"] <= 0:
        return _response(runtime, {"error": "invalid_night"}, 400)
    reading.pop("device", None)
    reading.pop("confidence", None)
    sources = dict(await _night_sources(runtime, account_bi, night))
    sources["manual"] = reading
    projected = await _store_night(runtime, account_bi, night, sources)
    await runtime.audit(actor, "sleep.night.save", "sleep_night", actor)
    return _response(runtime, {"ok": True, "night": projected})


async def _delete_night(runtime, account_bi, actor, night, source):
    sources = dict(await _night_sources(runtime, account_bi, night))
    if source in SOURCES:
        sources.pop(source, None)
    elif source in ("", "all"):
        sources = {}
    else:
        return _response(runtime, {"error": "invalid_source"}, 400)
    projected = await _store_night(runtime, account_bi, night, sources)
    await runtime.audit(actor, "sleep.night.delete", "sleep_night", actor)
    return _response(runtime, {"ok": True, "deleted": True, "night": projected})


async def account_installed(runtime, account_bi):
    return bool(await _profile_row(runtime, account_bi))


async def account_has_data(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT 1 AS one FROM sleep_nights WHERE account_bi=? LIMIT 1", account_bi)
    return bool(row)


async def export_account_data(runtime, account_bi, actor):
    runtime = _vault(runtime)
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-sleep-export-v1",
        "account": actor,
        "exportedAt": runtime.now(),
        "profile": await _profile(runtime, account_bi),
        "nights": list((await _nights(runtime, account_bi, FIRST_DAY, LAST_DAY)).values()),
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    for table in ("sleep_nights", "sleep_profiles", "sleep_keys"):
        await runtime.d1_run("DELETE FROM " + table + " WHERE account_bi=?", account_bi)
    if isinstance(runtime, _Vault):
        runtime.forget(account_bi)


def _export_filename(actor):
    return "forkmesh-sleep-" + actor + ".json"


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
    if head == "nights":
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
    head = parts[0] if parts else ""
    if head == "install":
        return await _install(runtime, account_bi, actor, data)
    row = await _profile_row(runtime, account_bi)
    if not row:
        if not parts:
            return _response(runtime, {"ok": True, "installed": False, "actor": actor})
        return _response(runtime, {"error": "plugin_not_installed", "canInstall": True}, 409)
    today = _day(params.get("night")) or _day(params.get("day")) or _day(data.get("night"))
    if not today:
        profile = await _profile(runtime, account_bi, row)
        today = _local_day(runtime.now(), profile["tzOffsetMinutes"])
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
        return await _sync(runtime, row, account_bi, actor, data)
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
            "nights": _series(await _nights(runtime, account_bi, start, end), start, end),
        })
    night = _day(parts[1])
    if not night:
        return _response(runtime, {"error": "invalid_night"}, 400)
    if method == "DELETE":
        return await _delete_night(runtime, account_bi, actor, night,
                                   _clean(params.get("source"), 16).lower() or "manual")
    return await _set_manual(runtime, account_bi, actor, night, data)
