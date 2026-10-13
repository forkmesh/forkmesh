import re


PREFIX = "/api/water"
PLUGIN_ID = "water"
REQUEST_PROOF = "forkmesh-water-request-v1"
MAX_BODY_BYTES = 128 * 1024
MAX_ENTRIES_PER_DAY = 200
MAX_IMPORT = 200
MAX_RANGE_DAYS = 400
OVERVIEW_DAYS = 30
KEY_VERSION = 1
DEFAULT_GOAL_ML = 2000
MAX_GOAL_ML = 10000
MAX_AMOUNT_ML = 5000
ML_PER_FL_OZ = 29.5735295625
FIRST_DAY = "0000-01-01"
LAST_DAY = "9999-12-31"
SOURCES = frozenset({"manual", "quick", "health"})
DRINKS = ("water", "sparkling", "tea", "coffee", "juice", "milk", "other")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
DAY_RE = re.compile(r"^\d{4}-(0[1-9]|1[0-2])-(0[1-9]|[12]\d|3[01])$")
EXTERNAL_RE = re.compile(r"^[A-Za-z0-9._:-]{1,80}$")
IMPERIAL_COUNTRIES = frozenset({"US", "LR", "MM"})
UNIT_CHOICES = ("auto", "metric", "imperial", "cups")
METRIC_PRESETS = (250, 500, 750)
IMPERIAL_PRESETS_OZ = (8, 16, 24)
FL_OZ_PER_CUP = 8
CUPS_PRESETS = (1, 2, 3)

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS water_keys (
        account_bi TEXT PRIMARY KEY,
        key_version INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        wrapped TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS water_profiles (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        installed_at INTEGER NOT NULL CHECK(installed_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS water_entries (
        entry_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        day TEXT NOT NULL,
        logged_at INTEGER NOT NULL CHECK(logged_at >= 0),
        external TEXT NOT NULL DEFAULT '',
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_water_entries_account_day "
    "ON water_entries(account_bi,day,logged_at)",
    "CREATE INDEX IF NOT EXISTS idx_water_entries_external "
    "ON water_entries(account_bi,external)",
]

DATA_SCHEMA_DOWN = [
    "DROP INDEX IF EXISTS idx_water_entries_external",
    "DROP INDEX IF EXISTS idx_water_entries_account_day",
    "DROP TABLE IF EXISTS water_entries",
    "DROP TABLE IF EXISTS water_profiles",
    "DROP TABLE IF EXISTS water_keys",
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
                "INSERT INTO water_keys(account_bi,key_version,created_at,wrapped) "
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
            "SELECT wrapped FROM water_keys WHERE account_bi=?", account_bi)
        if not row:
            return ""
        opened = await self._runtime.open(row.get("wrapped"))
        return str(opened.get("key") or "") if isinstance(opened, dict) else ""

    async def seal_for(self, account_bi, value):
        material = await self.key(account_bi, create=True)
        if not material:
            raise RuntimeError("water: no key for this account")
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
    result = {"goalMl": DEFAULT_GOAL_ML, "unitsPreference": "auto", "tzOffsetMinutes": 0}
    result.update(existing or {})
    if "goalMl" in data:
        result["goalMl"] = max(0, min(MAX_GOAL_ML, _integer(data.get("goalMl"), result["goalMl"])))
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
    if result["units"] == "imperial":
        result["presetsMl"] = [int(round(oz * ML_PER_FL_OZ)) for oz in IMPERIAL_PRESETS_OZ]
    elif result["units"] == "cups":
        result["presetsMl"] = [int(round(cups * FL_OZ_PER_CUP * ML_PER_FL_OZ)) for cups in CUPS_PRESETS]
    else:
        result["presetsMl"] = list(METRIC_PRESETS)
    return result


async def _profile_row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_name,installed_at,updated_at,data FROM water_profiles "
        "WHERE account_bi=?", account_bi)


async def _profile(runtime, account_bi, row=None):
    row = row or await _profile_row(runtime, account_bi)
    if not row:
        return _profile_data({}, None, _country(runtime))
    stored = await runtime.open_for(account_bi, row.get("data"))
    return _profile_data({}, stored, _country(runtime))


async def _install(runtime, account_bi, actor, data):
    profile = _profile_data(data if isinstance(data, dict) else {}, None, _country(runtime))
    stored = {key: profile[key] for key in ("goalMl", "unitsPreference", "tzOffsetMinutes")}
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO water_profiles(account_bi,account_name,installed_at,updated_at,data) "
        "VALUES(?,?,?,?,?) ON CONFLICT(account_bi) DO UPDATE SET "
        "account_name=excluded.account_name",
        account_bi, actor, now, now, await runtime.seal_for(account_bi, stored),
    )
    await runtime.audit(actor, "plugin.install", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {"ok": True, "installed": True, "plugin": PLUGIN_ID})


async def _save_profile(runtime, row, account_bi, actor, data):
    stored = await runtime.open_for(account_bi, row.get("data"))
    profile = _profile_data(data, stored, _country(runtime))
    keep = {key: profile[key] for key in ("goalMl", "unitsPreference", "tzOffsetMinutes")}
    await runtime.d1_run(
        "UPDATE water_profiles SET account_name=?,updated_at=?,data=? WHERE account_bi=?",
        actor, runtime.now(), await runtime.seal_for(account_bi, keep), account_bi,
    )
    await runtime.audit(actor, "water.profile.save", "water_profile", actor)
    return profile


def _entry_data(data, existing=None, now=0):
    result = dict(existing or {})
    if "amountMl" in data or "amountMl" not in result:
        result["amountMl"] = max(0, min(MAX_AMOUNT_ML, _integer(data.get("amountMl"))))
    if "drink" in data or "drink" not in result:
        drink = _clean(data.get("drink"), 16).lower()
        result["drink"] = drink if drink in DRINKS else "water"
    if "note" in data or "note" not in result:
        result["note"] = _clean(data.get("note"), 200)
    if "at" in data or "at" not in result:
        result["at"] = max(0, _integer(data.get("at"), now)) or now
    if not existing:
        source = _clean(data.get("source"), 16).lower()
        result["source"] = source if source in SOURCES else "manual"
        external = _clean(data.get("externalId"), 80)
        if external and EXTERNAL_RE.fullmatch(external):
            result["externalId"] = external
    return result


def _project_entry(row, data):
    entry = {
        "id": str(row.get("entry_id") or ""),
        "day": str(row.get("day") or ""),
        "loggedAt": int(row.get("logged_at") or 0),
        "amountMl": _integer(data.get("amountMl")),
        "drink": str(data.get("drink") or "water"),
        "note": str(data.get("note") or ""),
        "at": _integer(data.get("at")),
        "source": str(data.get("source") or "manual"),
    }
    if data.get("externalId"):
        entry["externalId"] = str(data["externalId"])
    return entry


async def _external_key(runtime, account_bi, external):
    if not external:
        return ""
    digest = await runtime.blind("water-external-v1\n" + str(account_bi) + "\n" + external)
    return str(digest or "")[:64]


async def _entry_row(runtime, account_bi, entry_id):
    return await runtime.d1_first(
        "SELECT entry_id,day,logged_at,data FROM water_entries "
        "WHERE entry_id=? AND account_bi=?", entry_id, account_bi)


async def _entries(runtime, account_bi, start, end, limit=5000):
    rows = await runtime.d1_all(
        "SELECT entry_id,day,logged_at,data FROM water_entries WHERE account_bi=? "
        "AND day>=? AND day<=? ORDER BY day,logged_at LIMIT ?",
        account_bi, start, end, int(limit))
    result = []
    for row in rows or []:
        result.append(_project_entry(row, await runtime.open_for(account_bi, row.get("data"))))
    return result


def _day_totals(entries, start, end):
    totals = {}
    for entry in entries:
        item = totals.setdefault(entry["day"], {"day": entry["day"], "totalMl": 0, "entries": 0})
        item["totalMl"] += entry["amountMl"]
        item["entries"] += 1
    first, last = _day_ordinal(start), _day_ordinal(end)
    return [totals.get(_ordinal_day(ordinal)) or {"day": _ordinal_day(ordinal), "totalMl": 0, "entries": 0}
            for ordinal in range(first, last + 1)]


def _streak(series, goal):
    if goal <= 0:
        return 0
    streak = 0
    for index, item in enumerate(reversed(series)):
        if item["totalMl"] >= goal:
            streak += 1
        elif index == 0:
            continue
        else:
            break
    return streak


async def _overview(runtime, row, account_bi, actor, today):
    profile = await _profile(runtime, account_bi, row)
    start = _shift_day(today, -(OVERVIEW_DAYS - 1))
    entries = await _entries(runtime, account_bi, start, today)
    series = _day_totals(entries, start, today)
    goal = profile["goalMl"]
    logged = [item for item in series if item["entries"]]
    return _response(runtime, {
        "ok": True,
        "installed": True,
        "actor": actor,
        "day": today,
        "profile": profile,
        "today": series[-1],
        "entries": [entry for entry in entries if entry["day"] == today],
        "days": series,
        "summary": {
            "averageMl": (sum(item["totalMl"] for item in logged) // len(logged)) if logged else 0,
            "goalDays": sum(1 for item in series if goal > 0 and item["totalMl"] >= goal),
            "streak": _streak(series, goal),
        },
        "drinks": list(DRINKS),
    })


async def _day_count(runtime, account_bi, day):
    row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM water_entries WHERE account_bi=? AND day=?",
        account_bi, day)
    return int((row or {}).get("n") or 0)


async def _insert_entry(runtime, account_bi, data, today):
    day = _day(data.get("day")) or today
    now = runtime.now()
    entry = _entry_data(data, None, now)
    if entry["amountMl"] <= 0:
        return None, "invalid_amount", None
    external = await _external_key(runtime, account_bi, entry.get("externalId", ""))
    if external:
        existing = await runtime.d1_first(
            "SELECT entry_id,day,logged_at,data FROM water_entries "
            "WHERE account_bi=? AND external=?", account_bi, external)
        if existing:
            opened = await runtime.open_for(account_bi, existing.get("data"))
            return _project_entry(existing, opened), "", True
    if await _day_count(runtime, account_bi, day) >= MAX_ENTRIES_PER_DAY:
        return None, "day_full", None
    entry_id = runtime.new_id()
    row = {"entry_id": entry_id, "day": day, "logged_at": now}
    await runtime.d1_run(
        "INSERT INTO water_entries(entry_id,account_bi,day,logged_at,external,data) "
        "VALUES(?,?,?,?,?,?)",
        entry_id, account_bi, day, now, external, await runtime.seal_for(account_bi, entry))
    return _project_entry(row, entry), "", False


async def _create_entry(runtime, account_bi, actor, data, today):
    entry, error, duplicate = await _insert_entry(runtime, account_bi, data, today)
    if error:
        return _response(runtime, {"error": error}, 400)
    if not duplicate:
        await runtime.audit(actor, "water.entry.create", "water_entry", actor)
    return _response(runtime, {"ok": True, "entry": entry, "duplicate": bool(duplicate)},
                     200 if duplicate else 201)


async def _import(runtime, account_bi, actor, data, today):
    items = data.get("entries")
    if not isinstance(items, list) or not items:
        return _response(runtime, {"error": "no_entries"}, 400)
    if len(items) > MAX_IMPORT:
        return _response(runtime, {"error": "too_many_entries", "max": MAX_IMPORT}, 400)
    imported, skipped, entries = 0, 0, []
    for item in items:
        if not isinstance(item, dict):
            skipped += 1
            continue
        item = dict(item, source="health")
        if not _clean(item.get("externalId"), 80):
            skipped += 1
            continue
        entry, error, duplicate = await _insert_entry(runtime, account_bi, item, today)
        if error or duplicate:
            skipped += 1
            continue
        imported += 1
        entries.append(entry)
    if imported:
        await runtime.audit(actor, "water.import", "water_entry", actor)
    return _response(runtime, {"ok": True, "imported": imported, "skipped": skipped,
                               "entries": entries})


async def _update_entry(runtime, row, account_bi, actor, data):
    existing = await runtime.open_for(account_bi, row.get("data"))
    entry = _entry_data(data, existing, runtime.now())
    if entry["amountMl"] <= 0:
        return _response(runtime, {"error": "invalid_amount"}, 400)
    day = _day(data.get("day")) or str(row.get("day") or "")
    await runtime.d1_run(
        "UPDATE water_entries SET day=?,data=? WHERE entry_id=? AND account_bi=?",
        day, await runtime.seal_for(account_bi, entry), row.get("entry_id"), account_bi)
    await runtime.audit(actor, "water.entry.update", "water_entry", actor)
    return _response(runtime, {"ok": True, "entry": _project_entry(dict(row, day=day), entry)})


async def _delete_entry(runtime, row, account_bi, actor):
    await runtime.d1_run(
        "DELETE FROM water_entries WHERE entry_id=? AND account_bi=?",
        row.get("entry_id"), account_bi)
    await runtime.audit(actor, "water.entry.delete", "water_entry", actor)
    return _response(runtime, {"ok": True, "deleted": True})


async def account_installed(runtime, account_bi):
    return bool(await _profile_row(runtime, account_bi))


async def account_has_data(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT 1 AS one FROM water_entries WHERE account_bi=? LIMIT 1", account_bi)
    return bool(row)


async def export_account_data(runtime, account_bi, actor):
    runtime = _vault(runtime)
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-water-export-v1",
        "account": actor,
        "exportedAt": runtime.now(),
        "profile": await _profile(runtime, account_bi),
        "entries": await _entries(runtime, account_bi, FIRST_DAY, LAST_DAY, 50000),
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    for table in ("water_entries", "water_profiles", "water_keys"):
        await runtime.d1_run("DELETE FROM " + table + " WHERE account_bi=?", account_bi)
    if isinstance(runtime, _Vault):
        runtime.forget(account_bi)


def _export_filename(actor):
    return "forkmesh-water-" + actor + ".json"


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
    if head in ("install", "uninstall", "import") and len(parts) == 1:
        return ("POST",)
    if head == "profile" and len(parts) == 1:
        return ("GET", "PUT")
    if head == "export" and len(parts) == 1:
        return ("GET",)
    if head == "entries":
        if len(parts) == 1:
            return ("GET", "POST")
        if len(parts) == 2:
            return ("GET", "PATCH", "DELETE")
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
    today = _day(params.get("day")) or _day(data.get("today")) or _day_from_ms(runtime.now())
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
    if head == "import":
        return await _import(runtime, account_bi, actor, data, today)
    if head == "export":
        return _response(runtime, {
            "ok": True, "filename": _export_filename(actor),
            "export": await export_account_data(runtime, account_bi, actor),
        })
    if len(parts) == 1:
        if method == "POST":
            return await _create_entry(runtime, account_bi, actor, data, today)
        end = _day(params.get("to")) or today
        start = _day(params.get("from")) or _shift_day(end, -6)
        if start > end:
            start, end = end, start
        if _day_ordinal(end) - _day_ordinal(start) > MAX_RANGE_DAYS:
            start = _shift_day(end, -MAX_RANGE_DAYS)
        entries = await _entries(runtime, account_bi, start, end)
        return _response(runtime, {
            "ok": True, "from": start, "to": end, "entries": entries,
            "days": _day_totals(entries, start, end),
        })
    if not ID_RE.fullmatch(parts[1]):
        return _response(runtime, {"error": "entry_not_found"}, 404)
    row = await _entry_row(runtime, account_bi, parts[1])
    if not row:
        return _response(runtime, {"error": "entry_not_found"}, 404)
    if method == "GET":
        opened = await runtime.open_for(account_bi, row.get("data"))
        return _response(runtime, {"ok": True, "entry": _project_entry(row, opened)})
    if method == "DELETE":
        return await _delete_entry(runtime, row, account_bi, actor)
    return await _update_entry(runtime, row, account_bi, actor, data)
