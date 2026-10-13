"""First-party product analytics: capture, insights, replay, flags, surveys.

One module owns the whole server side of the Analytics package.

* Two public, session-less endpoints take traffic from a tracked site:
  ``/api/analytics/_/capture`` (event batches and session-replay chunks) and
  ``/api/analytics/_/decide`` (feature flags, surveys, recorder settings).
  They authenticate with a project's public write key, exactly like any
  product-analytics snippet, and never read a cookie or a session.
* Everything else is an organization member API behind the normal session,
  organization, and install gates.

Events are stored as sealed *batches* (one row per flush, merged into larger
rows by the maintenance hook), not one row per event. A free-plan D1 bills
rows, so an insight reads a few hundred rows instead of tens of thousands,
and the insight maths (trends, funnels, retention, paths, ...) runs here in
plain Python over the opened batches. No IP address is ever stored.
"""

from datetime import datetime, timedelta, timezone
import hashlib
import json
import math
import re


PREFIX = "/api/analytics"
REQUEST_PROOF = "forkmesh-analytics-request-v1"
PLUGIN_ID = "analytics"
SDK_VERSION = "1.0.0"

MAX_CAPTURE_BYTES = 900 * 1024
MAX_BODY_BYTES = 256 * 1024
MAX_BATCH_EVENTS = 100
MAX_BATCH_PERSONS = 20
MAX_EVENT_NAME = 200
MAX_PROPERTIES = 120
MAX_PROPERTY_KEY = 120
MAX_PROPERTY_TEXT = 1000
MAX_LONG_TEXT = 6000
LONG_TEXT_KEYS = frozenset({
    "$elements", "$exception_stack", "$heatmap_points", "$survey_response"})
MAX_REPLAY_CHUNK = 640 * 1024
MAX_PROJECTS = 20
MAX_OBJECTS_PER_KIND = 300
MAX_QUERY_BATCHES = 1500
MAX_QUERY_EVENTS = 60000
MAX_PERSON_SCAN = 3000
MAX_ALIAS_SCAN = 20000
MAX_EXPORT_EVENTS = 20000
DEFAULT_EVENT_QUOTA = 20000
DEFAULT_REPLAY_QUOTA = 40 * 1024 * 1024
HARD_EVENT_QUOTA = 200000
HARD_REPLAY_QUOTA = 400 * 1024 * 1024
DEFAULT_RETENTION_DAYS = 90
DEFAULT_REPLAY_RETENTION_DAYS = 21
COMPACT_TARGET_EVENTS = 1500
COMPACT_MAX_ROWS = 40
DAY_MS = 86400000
DECIDE_CACHE_MS = 30000

MANAGER_ROLES = frozenset({"owner", "admin"})
OBJECT_KINDS = ("insight", "dashboard", "cohort", "action", "annotation",
                "flag", "experiment", "survey")
QUERY_KINDS = ("overview", "trends", "funnel", "retention", "paths",
               "stickiness", "lifecycle", "events", "heatmap", "errors",
               "taxonomy", "propertyValues", "experiment", "survey",
               "sessions", "campaigns")
OPERATORS = ("exact", "is_not", "icontains", "not_icontains", "regex",
             "not_regex", "gt", "gte", "lt", "lte", "is_set", "is_not_set")
MATHS = ("total", "dau", "sessions", "unique_group", "sum", "avg", "min",
         "max", "median", "p90", "p95", "p99")
INTERVALS = ("hour", "day", "week", "month")
SURVEY_QUESTION_TYPES = ("open", "rating", "nps", "single", "multiple", "link")

ORG_RE = re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
KEY_RE = re.compile(r"^(?:site|fma_[0-9a-f]{40})$")
PUBLIC_PAGE_PREFIXES = ("/docs",)
FLAG_KEY_RE = re.compile(r"^[A-Za-z0-9_-]{1,80}$")
SESSION_RE = re.compile(r"^[A-Za-z0-9_-]{8,64}$")
ORIGIN_RE = re.compile(r"^https?://[a-z0-9.-]{1,253}(?::[0-9]{1,5})?$")

DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS analytics_projects (
        project_id TEXT PRIMARY KEY,
        org_bi TEXT NOT NULL,
        org_name TEXT NOT NULL,
        write_key TEXT NOT NULL UNIQUE,
        site_default INTEGER NOT NULL DEFAULT 0,
        created_by_name TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        data TEXT NOT NULL
    )""",
    "CREATE INDEX IF NOT EXISTS idx_analytics_projects_org "
    "ON analytics_projects(org_bi,created_at)",
    """CREATE TABLE IF NOT EXISTS analytics_batches (
        batch_id TEXT PRIMARY KEY,
        project_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        day INTEGER NOT NULL,
        first_ts INTEGER NOT NULL,
        last_ts INTEGER NOT NULL,
        events INTEGER NOT NULL,
        compacted INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL
    )""",
    "CREATE INDEX IF NOT EXISTS idx_analytics_batches_project_day "
    "ON analytics_batches(project_id,day,first_ts)",
    """CREATE TABLE IF NOT EXISTS analytics_persons (
        project_id TEXT NOT NULL,
        person_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        identified INTEGER NOT NULL DEFAULT 0,
        first_seen INTEGER NOT NULL,
        last_seen INTEGER NOT NULL,
        events INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL,
        PRIMARY KEY (project_id, person_id)
    )""",
    "CREATE INDEX IF NOT EXISTS idx_analytics_persons_seen "
    "ON analytics_persons(project_id,last_seen DESC)",
    """CREATE TABLE IF NOT EXISTS analytics_aliases (
        project_id TEXT NOT NULL,
        id_hash TEXT NOT NULL,
        person_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        PRIMARY KEY (project_id, id_hash)
    )""",
    """CREATE TABLE IF NOT EXISTS analytics_groups (
        project_id TEXT NOT NULL,
        group_type TEXT NOT NULL,
        group_hash TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        first_seen INTEGER NOT NULL,
        last_seen INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (project_id, group_type, group_hash)
    )""",
    """CREATE TABLE IF NOT EXISTS analytics_recordings (
        project_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        person_id TEXT NOT NULL,
        started_at INTEGER NOT NULL,
        last_at INTEGER NOT NULL,
        active_ms INTEGER NOT NULL DEFAULT 0,
        chunks INTEGER NOT NULL DEFAULT 0,
        bytes INTEGER NOT NULL DEFAULT 0,
        clicks INTEGER NOT NULL DEFAULT 0,
        keys INTEGER NOT NULL DEFAULT 0,
        errors INTEGER NOT NULL DEFAULT 0,
        data TEXT NOT NULL,
        PRIMARY KEY (project_id, session_id)
    )""",
    "CREATE INDEX IF NOT EXISTS idx_analytics_recordings_started "
    "ON analytics_recordings(project_id,started_at DESC)",
    """CREATE TABLE IF NOT EXISTS analytics_recording_chunks (
        project_id TEXT NOT NULL,
        session_id TEXT NOT NULL,
        seq INTEGER NOT NULL,
        org_bi TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        bytes INTEGER NOT NULL,
        data TEXT NOT NULL,
        PRIMARY KEY (project_id, session_id, seq)
    )""",
    "CREATE INDEX IF NOT EXISTS idx_analytics_recording_chunks_age "
    "ON analytics_recording_chunks(created_at)",
    """CREATE TABLE IF NOT EXISTS analytics_objects (
        object_id TEXT PRIMARY KEY,
        project_id TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        kind TEXT NOT NULL CHECK (kind IN ('insight','dashboard','cohort',
            'action','annotation','flag','experiment','survey')),
        created_by_name TEXT NOT NULL,
        updated_by_name TEXT NOT NULL,
        created_at INTEGER NOT NULL,
        updated_at INTEGER NOT NULL,
        data TEXT NOT NULL
    )""",
    "CREATE INDEX IF NOT EXISTS idx_analytics_objects_project_kind "
    "ON analytics_objects(project_id,kind,updated_at DESC)",
    """CREATE TABLE IF NOT EXISTS analytics_usage (
        project_id TEXT NOT NULL,
        day INTEGER NOT NULL,
        org_bi TEXT NOT NULL,
        events INTEGER NOT NULL DEFAULT 0,
        replay_bytes INTEGER NOT NULL DEFAULT 0,
        requests INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (project_id, day)
    )""",
    """CREATE TABLE IF NOT EXISTS analytics_pages (
        project_id TEXT NOT NULL,
        path TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        views INTEGER NOT NULL DEFAULT 0,
        visitors INTEGER NOT NULL DEFAULT 0,
        updated_at INTEGER NOT NULL,
        PRIMARY KEY (project_id, path)
    )""",
    """CREATE TABLE IF NOT EXISTS analytics_page_visitors (
        project_id TEXT NOT NULL,
        path TEXT NOT NULL,
        visitor TEXT NOT NULL,
        org_bi TEXT NOT NULL,
        first_ts INTEGER NOT NULL,
        PRIMARY KEY (project_id, path, visitor)
    )""",
    "CREATE INDEX IF NOT EXISTS idx_analytics_page_visitors_visitor "
    "ON analytics_page_visitors(project_id,visitor)",
]
PAGE_SCHEMA_UP = DATA_SCHEMA_UP[-3:]
DATA_TABLES = ("analytics_page_visitors", "analytics_pages",
               "analytics_usage", "analytics_objects",
               "analytics_recording_chunks", "analytics_recordings",
               "analytics_groups", "analytics_aliases", "analytics_persons",
               "analytics_batches", "analytics_projects")
DATA_SCHEMA_DOWN = ["DROP TABLE IF EXISTS " + name for name in DATA_TABLES]

# Flags, surveys, and recorder settings are read on every page load of a
# tracked site. One isolate answers many of those, so a short memo keeps the
# public path at one project read instead of three.
_DECIDE_CACHE = {}
_PROJECT_CACHE = {}
_PAGE_SCHEMA = {"ready": False}


# --------------------------------------------------------------------------
# Small helpers


def _response(runtime, payload, status=200, allow="", public=False,
              cache_control="no-store, max-age=0, must-revalidate"):
    headers = {"x-content-type-options": "nosniff"}
    if allow:
        headers["allow"] = allow
    if public:
        # The write key is public by design and no credential rides along, so
        # any origin may call the two ingest endpoints; the project's own
        # allowed-origin list is what actually scopes them.
        headers["access-control-allow-origin"] = "*"
        headers["access-control-max-age"] = "86400"
    return runtime.response(payload, status=status, cache_control=cache_control,
                            extra_headers=headers)


def _text(value, limit):
    return str(value if value is not None else "").replace("\x00", "")[:limit]


def _clean(value, limit):
    return " ".join(_text(value, limit * 2).split())[:limit]


def _integer(value, default=0):
    try:
        if isinstance(value, bool):
            return int(value)
        return int(float(value))
    except (TypeError, ValueError, OverflowError):
        return default


def _number(value):
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value) if math.isfinite(float(value)) else None
    try:
        parsed = float(str(value).strip())
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def _bounded(value, low, high, default):
    parsed = _integer(value, default)
    return max(low, min(high, parsed))


def _hash(*parts):
    return hashlib.sha256(
        "\n".join(str(part) for part in parts).encode("utf-8")).hexdigest()[:32]


def _day(ts):
    moment = datetime.fromtimestamp(max(0, int(ts)) / 1000, tz=timezone.utc)
    return moment.year * 10000 + moment.month * 100 + moment.day


def _day_start(day):
    moment = datetime(day // 10000, day // 100 % 100, day % 100,
                      tzinfo=timezone.utc)
    return int(moment.timestamp() * 1000)


def _json_value(value, depth=0):
    """A bounded, JSON-safe copy of one event property value."""
    if value is None or isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value if abs(value) < 2 ** 53 else float(value)
    if isinstance(value, float):
        return value if math.isfinite(value) else None
    if isinstance(value, str):
        return _text(value, MAX_PROPERTY_TEXT)
    if depth >= 3:
        return _text(json.dumps(value, default=str), MAX_PROPERTY_TEXT)
    if isinstance(value, (list, tuple)):
        return [_json_value(item, depth + 1) for item in list(value)[:60]]
    if isinstance(value, dict):
        return {_text(key, MAX_PROPERTY_KEY): _json_value(item, depth + 1)
                for key, item in list(value.items())[:60]}
    return _text(value, MAX_PROPERTY_TEXT)


def _properties(value):
    result = {}
    if not isinstance(value, dict):
        return result
    for key, item in list(value.items())[:MAX_PROPERTIES]:
        name = _text(key, MAX_PROPERTY_KEY)
        if not name:
            continue
        if name in LONG_TEXT_KEYS:
            if isinstance(item, str):
                result[name] = _text(item, MAX_LONG_TEXT)
            else:
                encoded = json.dumps(_json_value(item), separators=(",", ":"))
                result[name] = (_json_value(item)
                                if len(encoded) <= MAX_LONG_TEXT else None)
            continue
        result[name] = _json_value(item)
    return result


async def _seal(runtime, value):
    return await runtime.seal(value)


async def _open(runtime, value, default):
    try:
        opened = await runtime.open(value)
    except Exception:
        return default
    return opened if isinstance(opened, type(default)) else default


async def _run_batch(runtime, statements):
    batch = getattr(runtime, "batch", None)
    if batch is not None:
        return await batch(statements)
    for sql, args in statements:
        await runtime.d1_run(sql, *args)


def _new_write_key(runtime):
    return "fma_" + _hash(runtime.new_id(), runtime.new_id(), runtime.now()) + \
        _hash(runtime.new_id())[:8]


def _request_origin(runtime):
    reader = getattr(runtime, "request_origin", None)
    try:
        return str(reader() or "").lower().rstrip("/") if reader else ""
    except Exception:
        return ""


# --------------------------------------------------------------------------
# Project settings


def _default_settings():
    return {
        "name": "Default project",
        "allowedOrigins": [],
        "autocapture": True,
        "heatmaps": True,
        "captureErrors": True,
        "capturePerformance": True,
        "replay": {"enabled": False, "sampleRate": 100, "minDurationMs": 2000,
                   "maskAllText": False, "maskAllInputs": True,
                   "recordCanvas": False},
        "eventQuota": DEFAULT_EVENT_QUOTA,
        "replayQuota": DEFAULT_REPLAY_QUOTA,
        "retentionDays": DEFAULT_RETENTION_DAYS,
        "replayRetentionDays": DEFAULT_REPLAY_RETENTION_DAYS,
        "testAccountFilters": [],
        "groupTypes": [],
        "timezoneOffsetMinutes": 0,
    }


def _origins(value):
    result = []
    for item in (value if isinstance(value, list) else [])[:40]:
        origin = _text(item, 300).strip().lower().rstrip("/")
        if ORIGIN_RE.fullmatch(origin) and origin not in result:
            result.append(origin)
    return result


def _settings_data(data, existing=None):
    result = dict(_default_settings())
    result.update(existing or {})
    result["replay"] = dict(_default_settings()["replay"],
                            **((existing or {}).get("replay") or {}))
    if not isinstance(data, dict):
        return result
    if "name" in data:
        result["name"] = _clean(data.get("name"), 80) or result["name"]
    if "allowedOrigins" in data:
        result["allowedOrigins"] = _origins(data.get("allowedOrigins"))
    for key in ("autocapture", "heatmaps", "captureErrors", "capturePerformance"):
        if key in data:
            result[key] = bool(data.get(key))
    replay = data.get("replay")
    if isinstance(replay, dict):
        target = result["replay"]
        for key in ("enabled", "maskAllText", "maskAllInputs", "recordCanvas"):
            if key in replay:
                target[key] = bool(replay.get(key))
        if "sampleRate" in replay:
            target["sampleRate"] = _bounded(replay.get("sampleRate"), 0, 100, 100)
        if "minDurationMs" in replay:
            target["minDurationMs"] = _bounded(
                replay.get("minDurationMs"), 0, 60000, 2000)
    if "eventQuota" in data:
        result["eventQuota"] = _bounded(
            data.get("eventQuota"), 0, HARD_EVENT_QUOTA, DEFAULT_EVENT_QUOTA)
    if "replayQuota" in data:
        result["replayQuota"] = _bounded(
            data.get("replayQuota"), 0, HARD_REPLAY_QUOTA, DEFAULT_REPLAY_QUOTA)
    if "retentionDays" in data:
        result["retentionDays"] = _bounded(data.get("retentionDays"), 1, 400,
                                           DEFAULT_RETENTION_DAYS)
    if "replayRetentionDays" in data:
        result["replayRetentionDays"] = _bounded(
            data.get("replayRetentionDays"), 1, 90, DEFAULT_REPLAY_RETENTION_DAYS)
    if "testAccountFilters" in data:
        result["testAccountFilters"] = _filters(data.get("testAccountFilters"))
    if "groupTypes" in data:
        result["groupTypes"] = [
            name for name in (_clean(item, 40).lower() for item in
                              (data.get("groupTypes") or [])[:5]) if name]
    if "timezoneOffsetMinutes" in data:
        result["timezoneOffsetMinutes"] = _bounded(
            data.get("timezoneOffsetMinutes"), -840, 840, 0)
    return result


async def _project_project(runtime, row, usage=None):
    settings = _settings_data(None, await _open(runtime, row.get("data"), {}))
    return {"id": str(row.get("project_id") or ""),
            "writeKey": str(row.get("write_key") or ""),
            "siteDefault": bool(row.get("site_default")),
            "createdBy": str(row.get("created_by_name") or ""),
            "createdAt": _integer(row.get("created_at")),
            "updatedAt": _integer(row.get("updated_at")),
            "settings": settings,
            "usage": usage or {"events": 0, "replayBytes": 0, "requests": 0}}


async def _project_rows(runtime, org_bi):
    return await runtime.d1_all(
        "SELECT project_id,write_key,site_default,created_by_name,created_at,"
        "updated_at,data FROM analytics_projects WHERE org_bi=? "
        "ORDER BY created_at LIMIT ?", org_bi, MAX_PROJECTS) or []


async def _project_row(runtime, org_bi, project_id):
    return await runtime.d1_first(
        "SELECT project_id,write_key,site_default,created_by_name,created_at,"
        "updated_at,data FROM analytics_projects WHERE org_bi=? AND project_id=?",
        org_bi, project_id)


async def _usage_today(runtime, project_id, now):
    row = await runtime.d1_first(
        "SELECT events,replay_bytes,requests FROM analytics_usage "
        "WHERE project_id=? AND day=?", project_id, _day(now))
    return {"events": _integer((row or {}).get("events")),
            "replayBytes": _integer((row or {}).get("replay_bytes")),
            "requests": _integer((row or {}).get("requests"))}


async def _ingest_project(runtime, key, now):
    """Resolve a public write key to its project, memoized per isolate."""
    cached = _PROJECT_CACHE.get(key)
    if cached and now - cached[0] < DECIDE_CACHE_MS:
        return cached[1]
    try:
        if key == "site":
            row = await runtime.d1_first(
                "SELECT project_id,org_bi,data FROM analytics_projects "
                "WHERE site_default=1 ORDER BY created_at LIMIT 1")
        else:
            row = await runtime.d1_first(
                "SELECT project_id,org_bi,data FROM analytics_projects "
                "WHERE write_key=?", key)
    except Exception as error:
        if "no such table" in str(error).lower():
            return None
        raise
    project = None
    if row:
        project = {"id": str(row["project_id"]), "org_bi": str(row["org_bi"]),
                   "settings": _settings_data(
                       None, await _open(runtime, row.get("data"), {}))}
    if len(_PROJECT_CACHE) > 200:
        _PROJECT_CACHE.clear()
    _PROJECT_CACHE[key] = (now, project)
    return project


def _forget_project(write_key, project_id=""):
    _PROJECT_CACHE.pop(write_key, None)
    _PROJECT_CACHE.pop("site", None)
    if project_id:
        _DECIDE_CACHE.pop(project_id, None)


def _origin_allowed(runtime, settings):
    allowed = settings.get("allowedOrigins") or []
    if not allowed:
        return True
    origin = _request_origin(runtime)
    # Server-side callers send no Origin header; the write key is their only
    # credential, as it is for every analytics ingestion API.
    return not origin or origin in allowed


# --------------------------------------------------------------------------
# Ingest


def _event(raw, now, country):
    if not isinstance(raw, dict):
        return None
    name = _clean(raw.get("event"), MAX_EVENT_NAME)
    distinct = _text(raw.get("distinctId") or raw.get("distinct_id"), 200).strip()
    if not name or not distinct:
        return None
    ts = _integer(raw.get("ts") or raw.get("timestamp"), now)
    # A device clock can be wildly wrong. Keep genuine offline queues (a week)
    # and refuse the future, so one bad clock cannot move an insight's window.
    if ts > now + 5 * 60000 or ts < now - 7 * DAY_MS:
        ts = now
    properties = _properties(raw.get("properties"))
    if country and "$geo_country" not in properties:
        properties["$geo_country"] = country
    event = {"e": name, "d": distinct, "t": ts, "p": properties}
    session = _text(raw.get("sessionId"), 64)
    if SESSION_RE.fullmatch(session):
        event["s"] = session
    uuid = _text(raw.get("uuid"), 64)
    if uuid:
        event["u"] = uuid
    groups = raw.get("groups")
    if isinstance(groups, dict) and groups:
        event["g"] = {_clean(key, 40).lower(): _text(value, 200)
                      for key, value in list(groups.items())[:5]
                      if _clean(key, 40) and _text(value, 200)}
    for source, target in (("set", "$set"), ("setOnce", "$set_once")):
        values = _properties(raw.get(source))
        if values:
            properties[target] = values
    return event


async def _person_row(runtime, project_id, person_id):
    return await runtime.d1_first(
        "SELECT person_id,identified,first_seen,last_seen,events,data "
        "FROM analytics_persons WHERE project_id=? AND person_id=?",
        project_id, person_id)


async def _resolve_person(runtime, project_id, distinct):
    own = _hash(project_id, distinct)
    row = await runtime.d1_first(
        "SELECT person_id FROM analytics_aliases WHERE project_id=? AND id_hash=?",
        project_id, own)
    return str((row or {}).get("person_id") or own)


async def _merge_people(runtime, project, source_distinct, target_distinct, now):
    """Fold the anonymous visitor ``source`` into the person ``target``."""
    project_id = project["id"]
    source_hash = _hash(project_id, source_distinct)
    target_id = await _resolve_person(runtime, project_id, target_distinct)
    source_id = await _resolve_person(runtime, project_id, source_distinct)
    if source_id == target_id:
        return target_id
    source = await _person_row(runtime, project_id, source_id)
    # An already-identified person is never swallowed by another identity:
    # that is how two real users on one shared browser stay two people.
    if source and _integer(source.get("identified")):
        return target_id
    statements = [(
        "INSERT INTO analytics_aliases(project_id,id_hash,person_id,org_bi,"
        "created_at) VALUES(?,?,?,?,?) ON CONFLICT(project_id,id_hash) "
        "DO UPDATE SET person_id=excluded.person_id",
        (project_id, source_hash, target_id, project["org_bi"], now))]
    if source:
        target = await _person_row(runtime, project_id, target_id)
        source_data = await _open(runtime, source.get("data"), {})
        target_data = await _open(runtime, (target or {}).get("data"), {})
        merged = dict(source_data.get("properties") or {})
        merged.update(target_data.get("properties") or {})
        distinct_ids = list(dict.fromkeys(
            (target_data.get("distinctIds") or [target_distinct])
            + (source_data.get("distinctIds") or [source_distinct])))[:20]
        sealed = await _seal(runtime, {"properties": merged,
                                       "distinctIds": distinct_ids})
        statements += [(
            "INSERT INTO analytics_persons(project_id,person_id,org_bi,"
            "identified,first_seen,last_seen,events,data) VALUES(?,?,?,?,?,?,?,?) "
            "ON CONFLICT(project_id,person_id) DO UPDATE SET identified=1,"
            "first_seen=MIN(first_seen,excluded.first_seen),"
            "last_seen=MAX(last_seen,excluded.last_seen),"
            "events=events+excluded.events,data=excluded.data",
            (project_id, target_id, project["org_bi"], 1,
             _integer(source.get("first_seen"), now),
             _integer(source.get("last_seen"), now),
             _integer(source.get("events")), sealed)), (
            "DELETE FROM analytics_persons WHERE project_id=? AND person_id=?",
            (project_id, source_id)), (
            "UPDATE analytics_recordings SET person_id=? "
            "WHERE project_id=? AND person_id=?",
            (target_id, project_id, source_id))]
    await _run_batch(runtime, statements)
    await _merge_page_visitors(runtime, project_id, source_id, target_id)
    return target_id


async def _merge_page_visitors(runtime, project_id, source_id, target_id):
    await _ensure_page_schema(runtime)
    visited = await runtime.d1_all(
        "SELECT path FROM analytics_page_visitors WHERE project_id=? AND visitor=?",
        project_id, source_id) or []
    if not visited:
        return
    await _run_batch(runtime, [(
        "INSERT INTO analytics_page_visitors(project_id,path,visitor,org_bi,"
        "first_ts) SELECT project_id,path,?,org_bi,first_ts FROM "
        "analytics_page_visitors WHERE project_id=? AND visitor=? "
        "ON CONFLICT(project_id,path,visitor) DO NOTHING",
        (target_id, project_id, source_id)), (
        "DELETE FROM analytics_page_visitors WHERE project_id=? AND visitor=?",
        (project_id, source_id))])
    for row in visited:
        await _recount_page(runtime, project_id, str(row["path"]))


async def _touch_person(runtime, project, distinct, events, now):
    """One upsert per flush; property writes only when the flush carries any."""
    project_id = project["id"]
    person_id = await _resolve_person(runtime, project_id, distinct)
    set_values, set_once, identified = {}, {}, False
    for event in events:
        properties = event["p"]
        if event["e"] == "$identify":
            identified = True
        set_values.update(properties.get("$set") or {})
        for key, value in (properties.get("$set_once") or {}).items():
            set_once.setdefault(key, value)
    first = min(event["t"] for event in events)
    last = max(event["t"] for event in events)
    data = None
    if set_values or set_once or identified:
        row = await _person_row(runtime, project_id, person_id)
        current = await _open(runtime, (row or {}).get("data"), {})
        properties = dict(current.get("properties") or {})
        for key, value in set_once.items():
            properties.setdefault(key, value)
        properties.update(set_values)
        distinct_ids = list(dict.fromkeys(
            (current.get("distinctIds") or []) + [distinct]))[:20]
        data = await _seal(runtime, {
            "properties": dict(list(properties.items())[:MAX_PROPERTIES]),
            "distinctIds": distinct_ids})
    if data is None:
        fresh = await _seal(runtime, {"properties": {}, "distinctIds": [distinct]})
        await runtime.d1_run(
            "INSERT INTO analytics_persons(project_id,person_id,org_bi,identified,"
            "first_seen,last_seen,events,data) VALUES(?,?,?,0,?,?,?,?) "
            "ON CONFLICT(project_id,person_id) DO UPDATE SET "
            "last_seen=MAX(last_seen,excluded.last_seen),"
            "events=events+excluded.events",
            project_id, person_id, project["org_bi"], first, last, len(events),
            fresh)
    else:
        await runtime.d1_run(
            "INSERT INTO analytics_persons(project_id,person_id,org_bi,identified,"
            "first_seen,last_seen,events,data) VALUES(?,?,?,?,?,?,?,?) "
            "ON CONFLICT(project_id,person_id) DO UPDATE SET "
            "identified=MAX(identified,excluded.identified),"
            "last_seen=MAX(last_seen,excluded.last_seen),"
            "events=events+excluded.events,data=excluded.data",
            project_id, person_id, project["org_bi"], 1 if identified else 0,
            first, last, len(events), data)
    return person_id


async def _touch_groups(runtime, project, events, now):
    for event in events:
        if event["e"] != "$groupidentify":
            continue
        properties = event["p"]
        group_type = _clean(properties.get("$group_type"), 40).lower()
        group_key = _text(properties.get("$group_key"), 200)
        if not group_type or not group_key:
            continue
        group_hash = _hash(project["id"], group_type, group_key)
        row = await runtime.d1_first(
            "SELECT data FROM analytics_groups WHERE project_id=? AND "
            "group_type=? AND group_hash=?", project["id"], group_type, group_hash)
        current = await _open(runtime, (row or {}).get("data"), {})
        values = dict(current.get("properties") or {})
        values.update(_properties(properties.get("$group_set")))
        sealed = await _seal(runtime, {"key": group_key, "properties": values})
        await runtime.d1_run(
            "INSERT INTO analytics_groups(project_id,group_type,group_hash,org_bi,"
            "first_seen,last_seen,data) VALUES(?,?,?,?,?,?,?) "
            "ON CONFLICT(project_id,group_type,group_hash) DO UPDATE SET "
            "last_seen=excluded.last_seen,data=excluded.data",
            project["id"], group_type, group_hash, project["org_bi"], now, now,
            sealed)


async def _store_replay(runtime, project, replay, person_id, errors, now):
    settings = project["settings"]
    if not (settings.get("replay") or {}).get("enabled"):
        return 0, "replay_disabled"
    session = _text(replay.get("sessionId"), 64)
    payload = replay.get("events")
    if not SESSION_RE.fullmatch(session) or not isinstance(payload, str) or not payload:
        return 0, "invalid_replay"
    size = len(payload)
    if size > MAX_REPLAY_CHUNK:
        return 0, "replay_chunk_too_large"
    seq = _bounded(replay.get("seq"), 0, 100000, 0)
    started = _integer(replay.get("startedAt"), now)
    if started > now + 5 * 60000 or started < now - 2 * DAY_MS:
        started = now
    sealed_chunk = await _seal(runtime, {
        "encoding": "gzip-b64" if replay.get("encoding") == "gzip-b64" else "json",
        "events": payload})
    existing = await runtime.d1_first(
        "SELECT data FROM analytics_recordings WHERE project_id=? AND session_id=?",
        project["id"], session)
    meta = await _open(runtime, (existing or {}).get("data"), {})
    if not meta:
        meta = {"startUrl": _text(replay.get("url"), 500),
                "viewport": _json_value(replay.get("viewport")),
                "device": _json_value(replay.get("device")),
                "distinctId": _text(replay.get("distinctId"), 200)}
    sealed_meta = await _seal(runtime, meta)
    await _run_batch(runtime, [(
        "INSERT INTO analytics_recording_chunks(project_id,session_id,seq,org_bi,"
        "created_at,bytes,data) VALUES(?,?,?,?,?,?,?) "
        "ON CONFLICT(project_id,session_id,seq) DO NOTHING",
        (project["id"], session, seq, project["org_bi"], now, size,
         sealed_chunk)), (
        "INSERT INTO analytics_recordings(project_id,session_id,org_bi,person_id,"
        "started_at,last_at,active_ms,chunks,bytes,clicks,keys,errors,data) "
        "VALUES(?,?,?,?,?,?,?,1,?,?,?,?,?) "
        "ON CONFLICT(project_id,session_id) DO UPDATE SET "
        "person_id=excluded.person_id,last_at=MAX(last_at,excluded.last_at),"
        "active_ms=active_ms+excluded.active_ms,chunks=chunks+1,"
        "bytes=bytes+excluded.bytes,clicks=clicks+excluded.clicks,"
        "keys=keys+excluded.keys,errors=errors+excluded.errors",
        (project["id"], session, project["org_bi"], person_id, started, now,
         _bounded(replay.get("activeMs"), 0, 3600000, 0), size,
         _bounded(replay.get("clicks"), 0, 100000, 0),
         _bounded(replay.get("keys"), 0, 100000, 0), errors, sealed_meta))])
    return size, ""


async def _capture(runtime, data):
    now = runtime.now()
    key = _text(data.get("key"), 64)
    if not KEY_RE.fullmatch(key):
        return _response(runtime, {"error": "invalid_key"}, 401, public=True)
    project = await _ingest_project(runtime, key, now)
    if not project:
        return _response(runtime, {"error": "unknown_project"}, 401, public=True)
    settings = project["settings"]
    if not _origin_allowed(runtime, settings):
        return _response(runtime, {"error": "origin_not_allowed"}, 403, public=True)
    raw_batch = data.get("batch")
    raw_batch = raw_batch if isinstance(raw_batch, list) else []
    country_reader = getattr(runtime, "country", None)
    country = _text(country_reader() if country_reader else "", 2)
    events = [event for event in (
        _event(raw, now, country) for raw in raw_batch[:MAX_BATCH_EVENTS]) if event]
    replay = data.get("replay") if isinstance(data.get("replay"), dict) else None
    if not events and not replay:
        return _response(runtime, {"ok": True, "accepted": 0}, public=True)
    usage = await _usage_today(runtime, project["id"], now)
    event_quota = min(_integer(settings.get("eventQuota"), DEFAULT_EVENT_QUOTA),
                      HARD_EVENT_QUOTA)
    if events and usage["events"] + len(events) > event_quota:
        events = []
    replay_quota = min(_integer(settings.get("replayQuota"), DEFAULT_REPLAY_QUOTA),
                       HARD_REPLAY_QUOTA)
    if replay and usage["replayBytes"] >= replay_quota:
        replay = None
    if not events and not replay:
        return _response(runtime, {"error": "quota_exceeded"}, 429, public=True)

    person_id = ""
    if events:
        # One flush comes from one browser, but an identify inside it changes
        # who that browser is; merge before counting so the events land on the
        # identified person.
        for event in events:
            properties = event["p"]
            if event["e"] == "$identify":
                anonymous = _text(properties.get("$anon_distinct_id"), 200)
                if anonymous and anonymous != event["d"]:
                    await _merge_people(runtime, project, anonymous, event["d"], now)
            elif event["e"] == "$create_alias":
                alias = _text(properties.get("alias"), 200)
                if alias and alias != event["d"]:
                    await _merge_people(runtime, project, alias, event["d"], now)
        by_person = {}
        for event in events:
            by_person.setdefault(event["d"], []).append(event)
        # A browser flush names one visitor; a server-side batch may name
        # several. The cap keeps one request's person upserts bounded.
        visitors = {}
        for distinct, group in list(by_person.items())[:MAX_BATCH_PERSONS]:
            person_id = await _touch_person(runtime, project, distinct, group, now)
            visitors[distinct] = person_id
        await _touch_groups(runtime, project, events, now)
        first = min(event["t"] for event in events)
        last = max(event["t"] for event in events)
        await runtime.d1_run(
            "INSERT INTO analytics_batches(batch_id,project_id,org_bi,day,first_ts,"
            "last_ts,events,compacted,data) VALUES(?,?,?,?,?,?,?,0,?)",
            runtime.new_id(), project["id"], project["org_bi"], _day(first),
            first, last, len(events), await _seal(runtime, events))
        await _count_pages(runtime, project, events, visitors, now)

    replay_bytes, replay_error = 0, ""
    if replay:
        if not person_id:
            person_id = await _resolve_person(
                runtime, project["id"], _text(replay.get("distinctId"), 200))
        errors = sum(1 for event in events if event["e"] == "$exception")
        replay_bytes, replay_error = await _store_replay(
            runtime, project, replay, person_id, errors, now)
    await runtime.d1_run(
        "INSERT INTO analytics_usage(project_id,day,org_bi,events,replay_bytes,"
        "requests) VALUES(?,?,?,?,?,1) ON CONFLICT(project_id,day) DO UPDATE SET "
        "events=events+excluded.events,"
        "replay_bytes=replay_bytes+excluded.replay_bytes,requests=requests+1",
        project["id"], _day(now), project["org_bi"], len(events), replay_bytes)
    payload = {"ok": True, "accepted": len(events), "replayBytes": replay_bytes}
    if replay_error:
        payload["replayError"] = replay_error
    return _response(runtime, payload, public=True)


async def _ensure_page_schema(runtime):
    if _PAGE_SCHEMA["ready"]:
        return
    for statement in PAGE_SCHEMA_UP:
        await runtime.d1_run(statement)
    _PAGE_SCHEMA["ready"] = True


async def _recount_page(runtime, project_id, path):
    await runtime.d1_run(
        "UPDATE analytics_pages SET visitors=(SELECT COUNT(*) FROM "
        "analytics_page_visitors WHERE project_id=? AND path=?) "
        "WHERE project_id=? AND path=?", project_id, path, project_id, path)


async def _count_pages(runtime, project, events, visitors, now):
    pages = {}
    for event in events:
        if event["e"] != "$pageview":
            continue
        page = pages.setdefault(_path_name(event, "$pageview"),
                                {"views": 0, "visitors": set()})
        page["views"] += 1
        page["visitors"].add(
            visitors.get(event["d"]) or _hash(project["id"], event["d"]))
    if not pages:
        return
    await _ensure_page_schema(runtime)
    for path, page in pages.items():
        statements = [(
            "INSERT INTO analytics_page_visitors(project_id,path,visitor,org_bi,"
            "first_ts) VALUES(?,?,?,?,?) ON CONFLICT(project_id,path,visitor) "
            "DO NOTHING", (project["id"], path, visitor, project["org_bi"], now))
            for visitor in sorted(page["visitors"])]
        statements.append((
            "INSERT INTO analytics_pages(project_id,path,org_bi,views,visitors,"
            "updated_at) VALUES(?,?,?,?,0,?) ON CONFLICT(project_id,path) DO "
            "UPDATE SET views=views+excluded.views,updated_at=excluded.updated_at",
            (project["id"], path, project["org_bi"], page["views"], now)))
        await _run_batch(runtime, statements)
        await _recount_page(runtime, project["id"], path)


def _page_path(value):
    path = re.split(r"[?#]", _text(value, 400))[0]
    return _text(path.rstrip("/") or "/", 200)


def _public_page(path):
    return any(path == prefix or path.startswith(prefix + "/")
               for prefix in PUBLIC_PAGE_PREFIXES)


async def _page_count(runtime):
    path = _page_path(runtime.query_params().get("path"))
    if not _public_page(path):
        return _response(runtime, {"error": "page_not_public"}, 404, public=True)
    project = await _ingest_project(runtime, "site", runtime.now())
    if not project:
        return _response(runtime, {"error": "unknown_project"}, 404, public=True)
    await _ensure_page_schema(runtime)
    row = await runtime.d1_first(
        "SELECT views,visitors FROM analytics_pages WHERE project_id=? AND path=?",
        project["id"], path) or {}
    return _response(runtime, {
        "ok": True, "path": path, "views": _integer(row.get("views")),
        "visitors": _integer(row.get("visitors"))}, public=True,
        cache_control="public, max-age=60")


# --------------------------------------------------------------------------
# Property filters (shared by insights, cohorts, flags, surveys)


def _filters(value):
    result = []
    for item in (value if isinstance(value, list) else [])[:20]:
        if not isinstance(item, dict):
            continue
        name = _text(item.get("property"), MAX_PROPERTY_KEY)
        operator = _text(item.get("operator") or "exact", 20)
        kind = _text(item.get("type") or "event", 12)
        if operator not in OPERATORS or kind not in ("event", "person", "cohort",
                                                     "group"):
            continue
        if kind == "cohort":
            cohort = _text(item.get("value"), 32)
            if ID_RE.fullmatch(cohort):
                result.append({"type": "cohort", "property": "id",
                               "operator": "exact" if operator != "is_not"
                               else "is_not", "value": cohort})
            continue
        if not name:
            continue
        raw = item.get("value")
        values = raw if isinstance(raw, list) else [raw]
        entry = {"type": kind, "property": name, "operator": operator,
                 "value": [_json_value(v) for v in values[:30]]}
        if kind == "group":
            entry["groupType"] = _clean(item.get("groupType"), 40).lower()
        result.append(entry)
    return result


def _matches_value(actual, operator, expected):
    if operator == "is_set":
        return actual is not None and actual != ""
    if operator == "is_not_set":
        return actual is None or actual == ""
    values = expected if isinstance(expected, list) else [expected]
    text = "" if actual is None else (
        json.dumps(actual) if isinstance(actual, (dict, list)) else str(actual))
    lowered = text.lower()
    if isinstance(actual, bool):
        lowered = "true" if actual else "false"
    candidates = [("true" if v is True else "false" if v is False
                   else "" if v is None else str(v)).lower() for v in values]
    if operator == "exact":
        return actual is not None and lowered in candidates
    if operator == "is_not":
        return lowered not in candidates
    if operator == "icontains":
        return any(c and c in lowered for c in candidates)
    if operator == "not_icontains":
        return not any(c and c in lowered for c in candidates)
    if operator in ("regex", "not_regex"):
        found = False
        for candidate in values:
            try:
                # Bounded input on both sides keeps a hostile pattern cheap.
                if re.search(str(candidate)[:200], text[:2000]):
                    found = True
                    break
            except re.error:
                continue
        return found if operator == "regex" else not found
    left = _number(actual)
    right = _number(values[0]) if values else None
    if left is None or right is None:
        return False
    return {"gt": left > right, "gte": left >= right,
            "lt": left < right, "lte": left <= right}[operator]


class _Context:
    """Everything an insight needs to decide who an event belongs to."""

    def __init__(self, project_id, aliases=None, persons=None, cohorts=None,
                 actions=None, groups=None):
        self.project_id = project_id
        self.aliases = aliases or {}
        self.persons = persons or {}
        self.cohorts = cohorts or {}
        self.actions = actions or {}
        self.groups = groups or {}
        self._actors = {}

    def actor(self, event):
        distinct = event["d"]
        found = self._actors.get(distinct)
        if found is None:
            own = _hash(self.project_id, distinct)
            found = self._actors[distinct] = self.aliases.get(own, own)
        return found

    def person_properties(self, event):
        return (self.persons.get(self.actor(event)) or {}).get("properties") or {}


def _event_matches(event, filters, context):
    for item in filters or ():
        kind = item.get("type")
        if kind == "cohort":
            members = context.cohorts.get(item.get("value")) or set()
            inside = context.actor(event) in members
            if inside != (item.get("operator") != "is_not"):
                return False
            continue
        if kind == "person":
            actual = context.person_properties(event).get(item["property"])
        elif kind == "group":
            key = (event.get("g") or {}).get(item.get("groupType") or "")
            group = context.groups.get((item.get("groupType") or "", key)) or {}
            actual = group.get(item["property"])
        else:
            actual = event["p"].get(item["property"])
            if actual is None and item["property"] == "event":
                actual = event["e"]
        if not _matches_value(actual, item["operator"], item.get("value")):
            return False
    return True


def _action_data(data):
    steps = []
    for raw in (data.get("steps") if isinstance(data.get("steps"), list) else [])[:10]:
        if not isinstance(raw, dict):
            continue
        steps.append({
            "event": _clean(raw.get("event"), MAX_EVENT_NAME) or "$autocapture",
            "url": _text(raw.get("url"), 400),
            "urlMatching": _text(raw.get("urlMatching") or "contains", 10),
            "selector": _text(raw.get("selector"), 200),
            "text": _text(raw.get("text"), 200),
            "properties": _filters(raw.get("properties"))})
    return {"steps": steps}


def _selector_matches(selector, elements):
    """Match a simple ``tag#id.class[attr="v"]`` selector against a chain."""
    match = re.fullmatch(
        r"\s*([a-zA-Z][a-zA-Z0-9-]*)?((?:[#.][A-Za-z0-9_-]+)*)"
        r"((?:\[[A-Za-z0-9_:-]+(?:=\"[^\"]*\")?\])*)\s*", selector or "")
    if not match:
        return False
    tag, marks, attributes = match.group(1), match.group(2), match.group(3)
    wanted_id = [m[1:] for m in re.findall(r"#[A-Za-z0-9_-]+", marks)]
    wanted_classes = [m[1:] for m in re.findall(r"\.[A-Za-z0-9_-]+", marks)]
    wanted_attributes = re.findall(r"\[([A-Za-z0-9_:-]+)(?:=\"([^\"]*)\")?\]",
                                   attributes)
    for element in elements if isinstance(elements, list) else []:
        if not isinstance(element, dict):
            continue
        if tag and str(element.get("tag") or "").lower() != tag.lower():
            continue
        if wanted_id and str(element.get("id") or "") not in wanted_id:
            continue
        classes = element.get("classes") or []
        if any(name not in classes for name in wanted_classes):
            continue
        attrs = element.get("attrs") or {}
        if any(name not in attrs or (value and str(attrs.get(name)) != value)
               for name, value in wanted_attributes):
            continue
        return True
    return False


def _action_matches(event, action, context):
    for step in action.get("steps") or ():
        if step["event"] != "*" and step["event"] != event["e"]:
            continue
        properties = event["p"]
        url = str(properties.get("$current_url") or "")
        if step["url"]:
            mode = step["urlMatching"]
            if mode == "exact" and url != step["url"]:
                continue
            if mode == "regex":
                try:
                    if not re.search(step["url"][:200], url[:2000]):
                        continue
                except re.error:
                    continue
            elif mode != "exact" and step["url"].lower() not in url.lower():
                continue
        if step["text"] and step["text"].lower() not in str(
                properties.get("$el_text") or "").lower():
            continue
        if step["selector"] and not _selector_matches(
                step["selector"], properties.get("$elements")):
            continue
        if _event_matches(event, step["properties"], context):
            return True
    return False


def _series(value, limit=10):
    result = []
    for raw in (value if isinstance(value, list) else [])[:limit]:
        if not isinstance(raw, dict):
            continue
        name = _clean(raw.get("event"), MAX_EVENT_NAME) or "*"
        math_name = _text(raw.get("math") or "total", 20)
        result.append({
            "event": name,
            "label": _clean(raw.get("label"), 120),
            "math": math_name if math_name in MATHS else "total",
            "mathProperty": _text(raw.get("mathProperty"), MAX_PROPERTY_KEY),
            "groupType": _clean(raw.get("groupType"), 40).lower(),
            "properties": _filters(raw.get("properties"))})
    return result


def _series_label(series, context):
    if series.get("label"):
        return series["label"]
    name = series["event"]
    if name.startswith("action:"):
        return (context.actions.get(name[7:]) or {}).get("name") or "Action"
    return {"*": "All events", "$pageview": "Pageview",
            "$pageleave": "Pageleave", "$autocapture": "Autocapture",
            "$exception": "Exception", "$rageclick": "Rageclick"}.get(name, name)


def _series_matches(event, series, context):
    name = series["event"]
    if name.startswith("action:"):
        action = context.actions.get(name[7:])
        if not action or not _action_matches(event, action, context):
            return False
    elif name != "*" and name != event["e"]:
        return False
    return _event_matches(event, series["properties"], context)


# --------------------------------------------------------------------------
# Insight maths: pure functions over opened events


def _bucket_start(ts, interval, offset_ms):
    local = ts + offset_ms
    if interval == "hour":
        return local - local % 3600000 - offset_ms
    day = local - local % DAY_MS
    if interval == "day":
        return day - offset_ms
    moment = datetime.fromtimestamp(day / 1000, tz=timezone.utc)
    if interval == "week":
        moment -= timedelta(days=moment.weekday())
    else:
        moment = moment.replace(day=1)
    return int(moment.timestamp() * 1000) - offset_ms


def _next_bucket(start, interval, offset_ms):
    if interval == "hour":
        return start + 3600000
    if interval == "day":
        return start + DAY_MS
    if interval == "week":
        return start + 7 * DAY_MS
    moment = datetime.fromtimestamp((start + offset_ms) / 1000, tz=timezone.utc)
    year, month = (moment.year + 1, 1) if moment.month == 12 else (
        moment.year, moment.month + 1)
    return int(datetime(year, month, 1, tzinfo=timezone.utc).timestamp() * 1000) \
        - offset_ms


def _buckets(start, end, interval, offset_ms):
    labels = []
    cursor = _bucket_start(start, interval, offset_ms)
    while cursor <= end and len(labels) < 400:
        labels.append(cursor)
        cursor = _next_bucket(cursor, interval, offset_ms)
    return labels


def _percentile(values, fraction):
    if not values:
        return 0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low, high = int(math.floor(position)), int(math.ceil(position))
    if low == high:
        return ordered[low]
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def _aggregate(math_name, events, series, context):
    if math_name == "total":
        return len(events)
    if math_name == "dau":
        return len({context.actor(event) for event in events})
    if math_name == "sessions":
        return len({event.get("s") for event in events if event.get("s")})
    if math_name == "unique_group":
        group_type = series.get("groupType") or ""
        return len({(event.get("g") or {}).get(group_type) for event in events
                    if (event.get("g") or {}).get(group_type)})
    values = [number for number in (
        _number(event["p"].get(series.get("mathProperty") or ""))
        for event in events) if number is not None]
    if not values:
        return 0
    if math_name == "sum":
        return round(sum(values), 4)
    if math_name == "avg":
        return round(sum(values) / len(values), 4)
    if math_name == "min":
        return min(values)
    if math_name == "max":
        return max(values)
    fraction = {"median": 0.5, "p90": 0.9, "p95": 0.95, "p99": 0.99}[math_name]
    return round(_percentile(values, fraction), 4)


def _breakdown_spec(value):
    if not isinstance(value, dict) or not value.get("property"):
        return None
    kind = _text(value.get("type") or "event", 12)
    return {"property": _text(value.get("property"), MAX_PROPERTY_KEY),
            "type": kind if kind in ("event", "person", "group") else "event",
            "groupType": _clean(value.get("groupType"), 40).lower()}


def _breakdown_value(event, breakdown, context):
    if breakdown["type"] == "person":
        actual = context.person_properties(event).get(breakdown["property"])
    elif breakdown["type"] == "group":
        key = (event.get("g") or {}).get(breakdown["groupType"])
        if breakdown["property"] == "$group_key":
            actual = key
        else:
            actual = (context.groups.get((breakdown["groupType"], key)) or {}).get(
                breakdown["property"])
    else:
        actual = event["p"].get(breakdown["property"])
    if actual is None or actual == "":
        return "(none)"
    if isinstance(actual, (dict, list)):
        return _text(json.dumps(actual), 120)
    return _text(actual, 120)


def trends(events, query, context, start, end):
    interval = query.get("interval") if query.get("interval") in INTERVALS else "day"
    offset_ms = _bounded(query.get("tzOffsetMinutes"), -840, 840, 0) * 60000
    labels = _buckets(start, end, interval, offset_ms)
    index = {label: position for position, label in enumerate(labels)}
    breakdown = _breakdown_spec(query.get("breakdown"))
    filters = _filters(query.get("filters"))
    results = []
    for series in _series(query.get("series")):
        grouped = {}
        for event in events:
            if not (start <= event["t"] <= end):
                continue
            if not _series_matches(event, series, context):
                continue
            if not _event_matches(event, filters, context):
                continue
            key = _breakdown_value(event, breakdown, context) if breakdown else ""
            bucket = _bucket_start(event["t"], interval, offset_ms)
            if bucket in index:
                grouped.setdefault(key, {}).setdefault(bucket, []).append(event)
        if not grouped:
            grouped = {"": {}}
        rows = []
        for key, by_bucket in grouped.items():
            data = [_aggregate(series["math"], by_bucket.get(label, []),
                               series, context) for label in labels]
            everything = [event for items in by_bucket.values() for event in items]
            rows.append({"name": _series_label(series, context),
                         "breakdown": key, "data": data,
                         "total": _aggregate(series["math"], everything, series,
                                             context)})
        rows.sort(key=lambda row: -row["total"])
        results += rows[:25]
    return {"labels": labels, "interval": interval, "series": results}


def funnel(events, query, context, start, end):
    steps = _series(query.get("steps"), 12)
    if len(steps) < 2:
        return {"steps": [], "error": "funnel_needs_two_steps"}
    window = _bounded(query.get("windowSeconds"), 60, 90 * 86400, 14 * 86400) * 1000
    order = query.get("order") if query.get("order") in (
        "ordered", "strict", "unordered") else "ordered"
    filters = _filters(query.get("filters"))
    breakdown = _breakdown_spec(query.get("breakdown"))
    by_actor = {}
    for event in events:
        if start <= event["t"] <= end and _event_matches(event, filters, context):
            by_actor.setdefault(context.actor(event), []).append(event)

    def progress(actor_events):
        """Deepest step reached and the timestamp of each completed step."""
        best, best_times, best_event = 0, [], None
        matches = [[position for position, step in enumerate(steps)
                    if _series_matches(event, step, context)]
                   for event in actor_events]
        for begin, event in enumerate(actor_events):
            if order == "unordered":
                if not matches[begin]:
                    continue
            elif 0 not in matches[begin]:
                continue
            reached, times = 1, [event["t"]]
            seen = {matches[begin][0]} if order == "unordered" else set()
            for later in range(begin + 1, len(actor_events)):
                candidate = actor_events[later]
                if candidate["t"] - event["t"] > window or reached >= len(steps):
                    break
                if order == "unordered":
                    fresh = [p for p in matches[later] if p not in seen]
                    if fresh:
                        seen.add(fresh[0])
                        reached += 1
                        times.append(candidate["t"])
                elif reached in matches[later]:
                    reached += 1
                    times.append(candidate["t"])
                elif order == "strict":
                    break
            if reached > best:
                best, best_times, best_event = reached, times, event
            if best >= len(steps):
                break
        return best, best_times, best_event

    groups = {}
    for actor, actor_events in by_actor.items():
        actor_events.sort(key=lambda item: item["t"])
        reached, times, first = progress(actor_events)
        if not reached:
            continue
        key = _breakdown_value(first, breakdown, context) if breakdown else ""
        bucket = groups.setdefault(key, {"counts": [0] * len(steps),
                                         "durations": [[] for _ in steps]})
        for position in range(reached):
            bucket["counts"][position] += 1
            if position:
                bucket["durations"][position].append(
                    times[position] - times[position - 1])

    def render(bucket):
        rows = []
        first_count = bucket["counts"][0] or 0
        for position, step in enumerate(steps):
            count = bucket["counts"][position]
            previous = bucket["counts"][position - 1] if position else count
            durations = bucket["durations"][position]
            rows.append({
                "name": _series_label(step, context), "count": count,
                "conversion": round(count / first_count, 4) if first_count else 0,
                "stepConversion": round(count / previous, 4) if previous else 0,
                "dropoff": max(0, previous - count) if position else 0,
                "medianTimeMs": int(_percentile(durations, 0.5)) if durations else 0,
                "averageTimeMs": int(sum(durations) / len(durations))
                if durations else 0})
        return rows

    total = {"counts": [0] * len(steps), "durations": [[] for _ in steps]}
    for bucket in groups.values():
        for position in range(len(steps)):
            total["counts"][position] += bucket["counts"][position]
            total["durations"][position] += bucket["durations"][position]
    result = {"steps": render(total), "order": order, "windowSeconds": window // 1000}
    if breakdown:
        ranked = sorted(groups.items(), key=lambda item: -item[1]["counts"][0])[:20]
        result["breakdown"] = [{"value": key, "steps": render(bucket)}
                               for key, bucket in ranked]
    return result


def retention(events, query, context, start, end):
    period = query.get("period") if query.get("period") in (
        "day", "week", "month") else "week"
    periods = _bounded(query.get("periods"), 2, 24, 8)
    offset_ms = _bounded(query.get("tzOffsetMinutes"), -840, 840, 0) * 60000
    target = (_series([query.get("targetEvent") or {"event": "$pageview"}])
              or [{"event": "$pageview", "properties": [], "math": "total"}])[0]
    returning = (_series([query.get("returningEvent") or
                          query.get("targetEvent") or {"event": "$pageview"}])
                 or [target])[0]
    first_time = query.get("retentionType") != "recurring"
    filters = _filters(query.get("filters"))
    labels = _buckets(start, end, period, offset_ms)[-periods:]
    index = {label: position for position, label in enumerate(labels)}
    target_buckets, return_buckets = {}, {}
    for event in events:
        if not _event_matches(event, filters, context):
            continue
        bucket = _bucket_start(event["t"], period, offset_ms)
        actor = context.actor(event)
        if _series_matches(event, target, context):
            target_buckets.setdefault(actor, set()).add(bucket)
        if _series_matches(event, returning, context):
            return_buckets.setdefault(actor, set()).add(bucket)
    rows = [{"start": label, "size": 0,
             "values": [0] * (len(labels) - position)}
            for position, label in enumerate(labels)]
    for actor, buckets in target_buckets.items():
        ordered = sorted(buckets)
        starts = [ordered[0]] if first_time else ordered
        for cohort_start in starts:
            position = index.get(cohort_start)
            if position is None:
                continue
            rows[position]["size"] += 1
            for bucket in return_buckets.get(actor, ()):
                later = index.get(bucket)
                if later is not None and later >= position:
                    rows[position]["values"][later - position] += 1
    for row in rows:
        row["percentages"] = [round(value / row["size"], 4) if row["size"] else 0
                              for value in row["values"]]
    return {"period": period, "rows": rows}


def _path_name(event, path_type):
    if path_type == "$screen":
        return _text(event["p"].get("$screen_name"), 200)
    if path_type == "custom":
        return event["e"]
    value = str(event["p"].get("$pathname") or "")
    if not value:
        match = re.match(r"^[a-z]+://[^/]+([^?#]*)",
                         str(event["p"].get("$current_url") or ""))
        value = match.group(1) if match else ""
    return _text(value.rstrip("/") or "/", 200)


def paths(events, query, context, start, end):
    path_type = query.get("pathType") if query.get("pathType") in (
        "$pageview", "$screen", "custom") else "$pageview"
    step_limit = _bounded(query.get("stepLimit"), 2, 10, 5)
    edge_limit = _bounded(query.get("edgeLimit"), 5, 120, 50)
    start_point = _text(query.get("startPoint"), 200)
    end_point = _text(query.get("endPoint"), 200)
    filters = _filters(query.get("filters"))
    sessions = {}
    for event in events:
        if not (start <= event["t"] <= end):
            continue
        if path_type == "custom":
            if event["e"].startswith("$"):
                continue
        elif event["e"] != path_type:
            continue
        if not _event_matches(event, filters, context):
            continue
        key = event.get("s") or context.actor(event)
        sessions.setdefault(key, []).append(event)
    edges, drop = {}, {}
    for session_events in sessions.values():
        session_events.sort(key=lambda item: item["t"])
        names = []
        for event in session_events:
            name = _path_name(event, path_type)
            if name and (not names or names[-1] != name):
                names.append(name)
        if start_point:
            if start_point not in names:
                continue
            names = names[names.index(start_point):]
        if end_point:
            if end_point not in names:
                continue
            names = names[:names.index(end_point) + 1][-step_limit:]
        names = names[:step_limit]
        for position in range(len(names) - 1):
            edge = ("%d_%s" % (position + 1, names[position]),
                    "%d_%s" % (position + 2, names[position + 1]))
            edges[edge] = edges.get(edge, 0) + 1
        if names:
            last = "%d_%s" % (len(names), names[-1])
            drop[last] = drop.get(last, 0) + 1
    ranked = sorted(edges.items(), key=lambda item: -item[1])[:edge_limit]
    return {"edges": [{"source": source, "target": target, "value": value}
                      for (source, target), value in ranked],
            "dropoffs": drop, "sessions": len(sessions)}


def stickiness(events, query, context, start, end):
    series_list = _series(query.get("series"), 5) or _series([{"event": "$pageview"}])
    interval = query.get("interval") if query.get("interval") in (
        "day", "week", "month") else "day"
    offset_ms = _bounded(query.get("tzOffsetMinutes"), -840, 840, 0) * 60000
    filters = _filters(query.get("filters"))
    size = len(_buckets(start, end, interval, offset_ms))
    results = []
    for series in series_list:
        active = {}
        for event in events:
            if (start <= event["t"] <= end
                    and _series_matches(event, series, context)
                    and _event_matches(event, filters, context)):
                active.setdefault(context.actor(event), set()).add(
                    _bucket_start(event["t"], interval, offset_ms))
        counts = [0] * size
        for buckets in active.values():
            counts[min(size, len(buckets)) - 1] += 1
        results.append({"name": _series_label(series, context), "data": counts,
                        "total": len(active)})
    return {"labels": list(range(1, size + 1)), "interval": interval,
            "series": results}


def lifecycle(events, query, context, start, end):
    series = (_series(query.get("series"), 1)
              or _series([{"event": "$pageview"}]))[0]
    interval = query.get("interval") if query.get("interval") in (
        "day", "week", "month") else "day"
    offset_ms = _bounded(query.get("tzOffsetMinutes"), -840, 840, 0) * 60000
    filters = _filters(query.get("filters"))
    labels = _buckets(start, end, interval, offset_ms)
    active = {}
    for event in events:
        if _series_matches(event, series, context) and _event_matches(
                event, filters, context):
            active.setdefault(context.actor(event), set()).add(
                _bucket_start(event["t"], interval, offset_ms))
    rows = {name: [0] * len(labels)
            for name in ("new", "returning", "resurrecting", "dormant")}
    for buckets in active.values():
        first = min(buckets)
        for position, label in enumerate(labels):
            previous = labels[position - 1] if position else None
            if label in buckets:
                if label == first:
                    rows["new"][position] += 1
                elif previous is not None and previous in buckets:
                    rows["returning"][position] += 1
                else:
                    rows["resurrecting"][position] += 1
            elif previous is not None and previous in buckets:
                rows["dormant"][position] -= 1
    return {"labels": labels, "interval": interval,
            "series": [{"name": name, "data": data} for name, data in rows.items()]}


def _top(counter, limit=12):
    return [{"value": key, "count": value} for key, value in
            sorted(counter.items(), key=lambda item: -item[1])[:limit]]


def _referrer_host(value):
    match = re.match(r"^[a-z]+://([^/:?#]+)", str(value or "").lower())
    return match.group(1) if match else ""


def _sessions(events, context):
    sessions = {}
    for event in events:
        key = event.get("s") or ("actor:" + context.actor(event))
        entry = sessions.get(key)
        if entry is None:
            entry = sessions[key] = {
                "id": event.get("s") or "", "actor": context.actor(event),
                "start": event["t"], "end": event["t"], "pageviews": 0,
                "events": 0, "entry": "", "exit": "", "errors": 0,
                "interactions": 0}
        entry["start"] = min(entry["start"], event["t"])
        entry["end"] = max(entry["end"], event["t"])
        entry["events"] += 1
        if event["e"] == "$pageview":
            entry["pageviews"] += 1
            name = _path_name(event, "$pageview")
            if not entry["entry"] or event["t"] <= entry["start"]:
                entry["entry"] = name
            entry["exit"] = name
        elif event["e"] == "$exception":
            entry["errors"] += 1
        elif event["e"] in ("$autocapture", "$rageclick"):
            entry["interactions"] += 1
    return sessions


def _overview_totals(events, context):
    sessions = _sessions(events, context)
    visitors = {context.actor(event) for event in events}
    pageviews = sum(1 for event in events if event["e"] == "$pageview")
    durations = [entry["end"] - entry["start"] for entry in sessions.values()]
    # A bounce is the usual definition: one pageview, no interaction, and
    # under ten seconds on the site.
    bounces = sum(1 for entry in sessions.values()
                  if entry["pageviews"] <= 1 and not entry["interactions"]
                  and entry["end"] - entry["start"] < 10000)
    return {"visitors": len(visitors), "pageviews": pageviews,
            "sessions": len(sessions),
            "bounceRate": round(bounces / len(sessions), 4) if sessions else 0,
            "averageSessionMs": int(sum(durations) / len(durations))
            if durations else 0,
            "events": len(events)}, sessions


SESSION_DEPTHS = (("1", 1), ("2", 2), ("3", 3), ("4–5", 5), ("6–9", 9),
                  ("10+", None))
SESSION_LENGTHS = (("< 10s", 10000), ("10–30s", 30000), ("30s–1m", 60000),
                   ("1–3m", 180000), ("3–10m", 600000), ("10–30m", 1800000),
                   ("30m+", None))
CHANNEL_MEDIUMS = {
    "organic": "Organic search", "social": "Social", "referral": "Referral",
    "cpc": "Paid", "ppc": "Paid", "paid": "Paid", "paidsearch": "Paid",
    "paid_search": "Paid", "paid-search": "Paid", "paid_social": "Paid",
    "paid-social": "Paid", "display": "Paid", "email": "Email",
    "newsletter": "Email", "e-mail": "Email", "affiliate": "Affiliate",
    "social-media": "Social", "social_media": "Social", "sm": "Social"}


def _bare_host(value):
    host = str(value or "").lower().split(":", 1)[0]
    return host[4:] if host.startswith("www.") else host


def _referrer_url(value):
    text = str(value or "")
    if not re.match(r"^https?://", text, re.IGNORECASE):
        return ""
    return _text(text.split("#", 1)[0], 300)


def _screen(properties):
    width = _integer(properties.get("$screen_width"))
    height = _integer(properties.get("$screen_height"))
    return f"{width}×{height}" if width > 0 and height > 0 else ""


def _channel(first):
    found = _session_channel(first, _bare_host(first["p"].get("$host")))
    if found["source"] == "(direct)":
        return "Direct"
    label = CHANNEL_MEDIUMS.get(found["medium"])
    if label:
        return label
    return "Campaign" if found["tagged"] else "Referral"


def _session_landings(events, context):
    firsts = {}
    for event in events:
        key = event.get("s") or ("actor:" + context.actor(event))
        held = firsts.get(key)
        if held is None or (event["e"] != "$pageview", event["t"]) < (
                held["e"] != "$pageview", held["t"]):
            firsts[key] = event
    return firsts


def _overview_extras(current, previous, sessions, context, offset_ms):
    counters = {name: {} for name in (
        "channels", "referringDomains", "referringUrls")}

    def bump(name, key):
        if key:
            key = _text(key, 300)
            counters[name][key] = counters[name].get(key, 0) + 1

    for first in _session_landings(current, context).values():
        properties = first["p"]
        bump("channels", _channel(first))
        host = _bare_host(_referrer_host(properties.get("$referrer")))
        if host and host != _bare_host(properties.get("$host")):
            bump("referringDomains", host)
            bump("referringUrls", _referrer_url(properties.get("$referrer")))

    seen_before = {context.actor(event) for event in previous}
    visitors = {context.actor(event) for event in current}
    returning = len(visitors & seen_before)

    depth = {label: 0 for label, _ in SESSION_DEPTHS}
    length = {label: 0 for label, _ in SESSION_LENGTHS}
    for entry in sessions.values():
        views = max(1, entry["pageviews"])
        depth[next(label for label, limit in SESSION_DEPTHS
                   if limit is None or views <= limit)] += 1
        span = entry["end"] - entry["start"]
        length[next(label for label, limit in SESSION_LENGTHS
                    if limit is None or span < limit)] += 1

    hours = [[0] * 24 for _ in range(7)]
    page_time, page_scroll = {}, {}
    for event in current:
        if event["e"] == "$pageview":
            local = datetime.fromtimestamp((event["t"] + offset_ms) / 1000,
                                           tz=timezone.utc)
            hours[local.weekday()][local.hour] += 1
        elif event["e"] == "$pageleave":
            page = _path_name(event, "$pageview")
            properties = event["p"]
            duration = _number(properties.get("$duration_ms"))
            if duration is not None and 0 <= duration <= 6 * 3600000:
                page_time.setdefault(page, []).append(duration)
            total = _number(properties.get("$page_height"))
            scrolled = _number(properties.get("$max_scroll_y"))
            if total and total > 0 and scrolled is not None:
                seen = scrolled + _integer(properties.get("$viewport_height"))
                page_scroll.setdefault(page, []).append(
                    max(0.0, min(1.0, seen / total)))
    engagement = []
    for page, durations in page_time.items():
        scrolls = page_scroll.get(page) or []
        engagement.append({
            "value": page, "count": len(durations),
            "medianMs": int(_percentile(durations, 0.5)),
            "scroll": round(sum(scrolls) / len(scrolls), 4) if scrolls else None})
    engagement.sort(key=lambda row: -row["count"])

    breakdowns = {name: _top(counter, 20 if name == "referringUrls" else 12)
                  for name, counter in counters.items()}
    return breakdowns, {
        "visitorKinds": {"new": len(visitors) - returning,
                         "returning": returning},
        "sessionDepth": [{"value": label, "count": count}
                         for label, count in depth.items()],
        "sessionLength": [{"value": label, "count": count}
                          for label, count in length.items()],
        "hours": hours, "engagement": engagement[:12]}


def overview(events, query, context, start, end):
    filters = _filters(query.get("filters"))
    span = max(1, end - start)
    current, previous = [], []
    for event in events:
        if not _event_matches(event, filters, context):
            continue
        if start <= event["t"] <= end:
            current.append(event)
        elif start - span <= event["t"] < start:
            previous.append(event)
    totals, sessions = _overview_totals(current, context)
    previous_totals, _ = _overview_totals(previous, context)
    counters = {name: {} for name in (
        "pages", "referrers", "sources", "campaigns", "browsers", "os",
        "devices", "countries", "entryPages", "exitPages", "events",
        "mediums", "languages", "screens")}
    page_visitors = {}
    for event in current:
        properties = event["p"]
        name = event["e"]
        if not name.startswith("$") or name in ("$autocapture", "$rageclick",
                                                 "$exception"):
            counters["events"][name] = counters["events"].get(name, 0) + 1
        if name != "$pageview":
            continue
        page = _path_name(event, "$pageview")
        counters["pages"][page] = counters["pages"].get(page, 0) + 1
        page_visitors.setdefault(page, set()).add(context.actor(event))
        for counter, value in (
                ("referrers", _referrer_host(properties.get("$referrer"))
                 or "(direct)"),
                ("sources", properties.get("utm_source")),
                ("campaigns", properties.get("utm_campaign")),
                ("browsers", properties.get("$browser")),
                ("os", properties.get("$os")),
                ("devices", properties.get("$device_type")),
                ("countries", properties.get("$geo_country")),
                ("mediums", properties.get("utm_medium")),
                ("languages", properties.get("$language")),
                ("screens", _screen(properties))):
            if value:
                key = _text(value, 120)
                counters[counter][key] = counters[counter].get(key, 0) + 1
    for entry in sessions.values():
        if entry["entry"]:
            counters["entryPages"][entry["entry"]] = counters["entryPages"].get(
                entry["entry"], 0) + 1
        if entry["exit"]:
            counters["exitPages"][entry["exit"]] = counters["exitPages"].get(
                entry["exit"], 0) + 1
    interval = "hour" if span <= 2 * DAY_MS else "day"
    series = trends(current, {
        "interval": interval, "tzOffsetMinutes": query.get("tzOffsetMinutes"),
        "series": [{"event": "$pageview", "math": "dau", "label": "Visitors"},
                   {"event": "$pageview", "math": "total", "label": "Pageviews"},
                   {"event": "$pageview", "math": "sessions", "label": "Sessions"}]},
        context, start, end)
    breakdowns = {name: _top(counter) for name, counter in counters.items()}
    for row in breakdowns["pages"]:
        row["visitors"] = len(page_visitors.get(row["value"], ()))
    vitals = {}
    for event in current:
        if event["e"] != "$web_vitals":
            continue
        for key in ("LCP", "CLS", "INP", "FCP", "TTFB"):
            number = _number(event["p"].get("$web_vitals_" + key))
            if number is not None:
                vitals.setdefault(key, []).append(number)
    offset_ms = _bounded(query.get("tzOffsetMinutes"), -840, 840, 0) * 60000
    extra_breakdowns, extras = _overview_extras(
        current, previous, sessions, context, offset_ms)
    breakdowns.update(extra_breakdowns)
    labels = series.get("labels") or []
    slots = {label: position for position, label in enumerate(labels)}
    bounced, spans, counts = ([0] * len(labels) for _ in range(3))
    for entry in sessions.values():
        position = slots.get(_bucket_start(entry["start"], interval, offset_ms))
        if position is None:
            continue
        counts[position] += 1
        spans[position] += entry["end"] - entry["start"]
        if entry["pageviews"] <= 1 and not entry["interactions"] \
                and entry["end"] - entry["start"] < 10000:
            bounced[position] += 1
    extras["sessionSeries"] = {
        "bounceRate": [round(bounced[i] / counts[i], 4) if counts[i] else 0
                       for i in range(len(labels))],
        "averageSessionMs": [int(spans[i] / counts[i]) if counts[i] else 0
                             for i in range(len(labels))]}
    return {"totals": totals, "previous": previous_totals, "timeseries": series,
            "breakdowns": breakdowns, **extras,
            "webVitals": {key: {"p75": round(_percentile(values, 0.75), 3),
                                "samples": len(values)}
                          for key, values in vitals.items()}}


def event_list(events, query, context, start, end):
    name = _clean(query.get("event"), MAX_EVENT_NAME)
    filters = _filters(query.get("filters"))
    limit = _bounded(query.get("limit"), 1, 200, 100)
    before = _integer(query.get("before"), end + 1)
    person = _text(query.get("personId"), 32)
    session = _text(query.get("sessionId"), 64)
    rows = []
    for event in sorted(events, key=lambda item: -item["t"]):
        if event["t"] >= before or event["t"] < start:
            continue
        if name and event["e"] != name:
            continue
        if person and context.actor(event) != person:
            continue
        if session and event.get("s") != session:
            continue
        if not _event_matches(event, filters, context):
            continue
        rows.append({"event": event["e"], "timestamp": event["t"],
                     "distinctId": event["d"], "personId": context.actor(event),
                     "sessionId": event.get("s") or "",
                     "groups": event.get("g") or {},
                     "properties": event["p"]})
        if len(rows) >= limit:
            break
    return {"events": rows,
            "nextBefore": rows[-1]["timestamp"] if len(rows) >= limit else 0}


def heatmap(events, query, context, start, end):
    path = _text(query.get("path"), 200).rstrip("/") or "/"
    kind = query.get("type") if query.get("type") in (
        "click", "rageclick", "move", "scroll") else "click"
    low = _bounded(query.get("viewportMin"), 0, 10000, 0)
    high = _bounded(query.get("viewportMax"), 0, 10000, 10000)
    cells, depths, pageviews, heights = {}, [], 0, []

    def add(x, y, width, weight=1):
        ratio, top = _number(x), _number(y)
        if ratio is None or top is None or not (low <= (width or 0) <= high):
            return
        cell = (max(0, min(99, int(ratio * 100))), max(0, int(top // 20)))
        cells[cell] = cells.get(cell, 0) + weight

    for event in events:
        if not (start <= event["t"] <= end):
            continue
        if _path_name(event, "$pageview") != path:
            continue
        properties = event["p"]
        width = _integer(properties.get("$viewport_width"))
        if event["e"] == "$pageview":
            pageviews += 1
        elif event["e"] == "$pageleave":
            depth = _number(properties.get("$max_scroll_y"))
            if depth is not None and low <= width <= high:
                depths.append(depth + _integer(properties.get("$viewport_height")))
                heights.append(_integer(properties.get("$page_height")))
        elif event["e"] == "$autocapture" and kind == "click":
            if properties.get("$event_type") == "click":
                add(properties.get("$x_ratio"), properties.get("$page_y"), width)
        elif event["e"] == "$rageclick" and kind == "rageclick":
            add(properties.get("$x_ratio"), properties.get("$page_y"), width)
        elif event["e"] == "$heatmap" and kind == "move":
            for point in (properties.get("$heatmap_points") or [])[:400]:
                if isinstance(point, list) and len(point) >= 2:
                    add(point[0], point[1], width)
    page_height = int(_percentile(heights, 0.9)) if heights else 0
    scroll = []
    if depths:
        top = max(page_height, int(max(depths)))
        for position in range(0, top + 1, max(100, top // 40 or 100)):
            scroll.append({"y": position, "reached": round(
                sum(1 for depth in depths if depth >= position) / len(depths), 4)})
    ranked = sorted(cells.items(), key=lambda item: -item[1])[:4000]
    return {"path": path, "type": kind, "pageviews": pageviews,
            "pageHeight": page_height, "cellHeight": 20,
            "points": [[x, y * 20, count] for (x, y), count in ranked],
            "scroll": scroll}


def errors(events, query, context, start, end):
    groups = {}
    for event in events:
        if event["e"] != "$exception" or not (start <= event["t"] <= end):
            continue
        properties = event["p"]
        kind = _text(properties.get("$exception_type") or "Error", 120)
        message = _text(properties.get("$exception_message"), 300)
        fingerprint = _text(properties.get("$exception_fingerprint"), 64) or _hash(
            kind, re.sub(r"[0-9]+", "#", message))
        group = groups.get(fingerprint)
        if group is None:
            group = groups[fingerprint] = {
                "fingerprint": fingerprint, "type": kind, "message": message,
                "count": 0, "users": set(), "sessions": set(),
                "firstSeen": event["t"], "lastSeen": event["t"],
                "stack": _text(properties.get("$exception_stack"), MAX_LONG_TEXT),
                "url": _text(properties.get("$current_url"), 400),
                "browser": _text(properties.get("$browser"), 60),
                "sessionId": event.get("s") or ""}
        group["count"] += 1
        group["users"].add(context.actor(event))
        if event.get("s"):
            group["sessions"].add(event["s"])
        group["firstSeen"] = min(group["firstSeen"], event["t"])
        if event["t"] >= group["lastSeen"]:
            group["lastSeen"] = event["t"]
            group["sessionId"] = event.get("s") or group["sessionId"]
    rows = sorted(groups.values(), key=lambda item: -item["count"])[:100]
    for row in rows:
        row["users"] = len(row["users"])
        row["sessions"] = len(row["sessions"])
    return {"groups": rows, "total": sum(row["count"] for row in rows)}


def taxonomy(events, query, context, start, end):
    names, keys, person_keys = {}, {}, {}
    for event in events:
        if not (start <= event["t"] <= end):
            continue
        names[event["e"]] = names.get(event["e"], 0) + 1
        for key, value in event["p"].items():
            if key in ("$set", "$set_once"):
                for name in (value or {}):
                    person_keys[name] = person_keys.get(name, 0) + 1
                continue
            keys[key] = keys.get(key, 0) + 1
    for person in context.persons.values():
        for name in person.get("properties") or {}:
            person_keys[name] = person_keys.get(name, 0) + 1
    return {"events": _top(names, 300), "properties": _top(keys, 300),
            "personProperties": _top(person_keys, 200)}


def property_values(events, query, context, start, end):
    name = _text(query.get("property"), MAX_PROPERTY_KEY)
    kind = query.get("type") if query.get("type") in ("event", "person") else "event"
    counter = {}
    if kind == "person":
        for person in context.persons.values():
            value = (person.get("properties") or {}).get(name)
            if value is not None and not isinstance(value, (dict, list)):
                counter[_text(value, 120)] = counter.get(_text(value, 120), 0) + 1
    else:
        for event in events:
            value = event["e"] if name == "event" else event["p"].get(name)
            if value is not None and not isinstance(value, (dict, list)):
                counter[_text(value, 120)] = counter.get(_text(value, 120), 0) + 1
    return {"property": name, "values": _top(counter, 60)}


def _normal_cdf(value):
    return 0.5 * (1 + math.erf(value / math.sqrt(2)))


def experiment(events, query, context, start, end):
    flag_key = _text(query.get("flagKey"), 80)
    goal = (_series([query.get("goal") or {"event": "$pageview"}])
            or _series([{"event": "$pageview"}]))[0]
    exposure = {}
    for event in sorted(events, key=lambda item: item["t"]):
        if (event["e"] == "$feature_flag_called"
                and event["p"].get("$feature_flag") == flag_key
                and start <= event["t"] <= end):
            variant = event["p"].get("$feature_flag_response")
            variant = "true" if variant is True else _text(variant, 80)
            if variant and variant != "false":
                exposure.setdefault(context.actor(event), (variant, event["t"]))
    converted = {}
    for event in events:
        seen = exposure.get(context.actor(event))
        if (seen and seen[1] <= event["t"] <= end
                and _series_matches(event, goal, context)):
            converted[context.actor(event)] = converted.get(
                context.actor(event), 0) + 1
    variants = {}
    for actor, (variant, _) in exposure.items():
        row = variants.setdefault(variant, {"variant": variant, "exposures": 0,
                                            "conversions": 0, "goalEvents": 0})
        row["exposures"] += 1
        if actor in converted:
            row["conversions"] += 1
            row["goalEvents"] += converted[actor]
    rows = sorted(variants.values(), key=lambda row: (row["variant"] != "control",
                                                      row["variant"]))
    control = next((row for row in rows if row["variant"] == "control"),
                   rows[0] if rows else None)
    for row in rows:
        row["conversionRate"] = round(row["conversions"] / row["exposures"], 4) \
            if row["exposures"] else 0
        row["probabilityToBeatControl"] = None
        row["significant"] = False
        if control is None or row is control:
            continue
        a, b = control, row
        if a["exposures"] < 1 or b["exposures"] < 1:
            continue
        rate_a = a["conversions"] / a["exposures"]
        rate_b = b["conversions"] / b["exposures"]
        spread = math.sqrt(rate_a * (1 - rate_a) / a["exposures"]
                           + rate_b * (1 - rate_b) / b["exposures"])
        if spread <= 0:
            continue
        probability = _normal_cdf((rate_b - rate_a) / spread)
        row["probabilityToBeatControl"] = round(probability, 4)
        row["lift"] = round((rate_b - rate_a) / rate_a, 4) if rate_a else None
        # Two-sided 95%, and enough people in both arms that the normal
        # approximation means something.
        row["significant"] = bool(
            (probability >= 0.975 or probability <= 0.025)
            and min(a["exposures"], b["exposures"]) >= 100)
    return {"flagKey": flag_key, "goal": _series_label(goal, context),
            "variants": rows, "exposed": len(exposure)}


def survey_results(events, query, context, start, end):
    survey_id = _text(query.get("surveyId"), 32)
    counts = {"shown": set(), "dismissed": set(), "sent": set()}
    answers = {}
    for event in events:
        if event["p"].get("$survey_id") != survey_id:
            continue
        kind = {"survey shown": "shown", "survey dismissed": "dismissed",
                "survey sent": "sent"}.get(event["e"])
        if not kind:
            continue
        counts[kind].add(context.actor(event))
        if kind != "sent":
            continue
        for key, value in event["p"].items():
            if not key.startswith("$survey_response"):
                continue
            position = key[len("$survey_response"):].lstrip("_") or "0"
            bucket = answers.setdefault(position, {"counts": {}, "texts": [],
                                                   "numbers": []})
            values = value if isinstance(value, list) else [value]
            for item in values:
                number = _number(item)
                if number is not None and not isinstance(item, str):
                    bucket["numbers"].append(number)
                label = _text(item, 300)
                if len(label) > 60:
                    if len(bucket["texts"]) < 200:
                        bucket["texts"].append({"text": label, "timestamp": event["t"]})
                else:
                    bucket["counts"][label] = bucket["counts"].get(label, 0) + 1
    questions = {}
    for position, bucket in answers.items():
        numbers = bucket["numbers"]
        summary = {"counts": _top(bucket["counts"], 40), "texts": bucket["texts"]}
        if numbers:
            summary["average"] = round(sum(numbers) / len(numbers), 3)
            promoters = sum(1 for n in numbers if n >= 9)
            detractors = sum(1 for n in numbers if n <= 6)
            summary["nps"] = round((promoters - detractors) * 100 / len(numbers), 1)
        questions[position] = summary
    shown = len(counts["shown"])
    return {"surveyId": survey_id, "shown": shown,
            "dismissed": len(counts["dismissed"]), "sent": len(counts["sent"]),
            "responseRate": round(len(counts["sent"]) / shown, 4) if shown else 0,
            "questions": questions}


def session_list(events, query, context, start, end):
    window = [event for event in events if start <= event["t"] <= end]
    rows = sorted(_sessions(window, context).values(),
                  key=lambda item: -item["start"])[:200]
    return {"sessions": [dict(row, durationMs=row["end"] - row["start"])
                         for row in rows]}


CAMPAIGN_GROUPS = ("campaign", "content", "source", "landing", "term")
SEARCH_HOSTS = ("google.", "bing.com", "duckduckgo.com", "search.yahoo.",
                "yandex.", "baidu.com", "ecosia.org", "kagi.com",
                "search.brave.com", "startpage.com", "qwant.com")
SOCIAL_HOSTS = ("reddit.com", "x.com", "t.co", "twitter.com", "facebook.com",
                "linkedin.com", "lnkd.in", "news.ycombinator.com",
                "youtube.com", "bsky.app", "threads.net", "instagram.com",
                "mastodon.social", "fosstodon.org", "hachyderm.io",
                "lobste.rs", "discord.com", "t.me")


def _host_in(host, domains):
    return any(host == item or host.endswith("." + item)
               or (item.endswith(".") and (host.startswith(item)
                                           or ("." + item) in host))
               for item in domains)


def _session_channel(first, own_host):
    properties = first["p"]
    source = _text(properties.get("utm_source"), 100).strip().lower()
    medium = _text(properties.get("utm_medium"), 100).strip().lower()
    campaign = _text(properties.get("utm_campaign"), 100).strip().lower()
    content = _text(properties.get("utm_content"), 100).strip().lower()
    term = " ".join(_text(properties.get("utm_term"), 100).lower().split())
    if source or medium or campaign:
        return {"source": source or "(none)", "medium": medium or "(none)",
                "campaign": campaign or "(none)", "content": content or "(none)",
                "term": term or "(none)", "tagged": True}
    host = _referrer_host(properties.get("$referrer"))
    host = host[4:] if host.startswith("www.") else host
    if not host or host == own_host:
        source, medium = "(direct)", "(none)"
    elif _host_in(host, SEARCH_HOSTS):
        source, medium = host, "organic"
    elif _host_in(host, SOCIAL_HOSTS):
        source, medium = host, "social"
    else:
        source, medium = host, "referral"
    if properties.get("gclid") is True:
        medium = "cpc"
    return {"source": source, "medium": medium, "campaign": "(none)",
            "content": "(none)",
            "term": term or ("(not provided)" if medium == "organic" else "(none)"),
            "tagged": False}


def _campaign_sessions(events, filters, goals, context):
    sessions = {}
    for event in events:
        if not _event_matches(event, filters, context):
            continue
        key = event.get("s") or ("actor:" + context.actor(event))
        entry = sessions.get(key)
        if entry is None:
            entry = sessions[key] = {
                "first": event, "start": event["t"], "end": event["t"],
                "actor": context.actor(event), "pageviews": 0,
                "interactions": 0, "landing": "", "goals": [0] * len(goals)}
        if event["t"] < entry["start"]:
            entry["first"], entry["start"] = event, event["t"]
        entry["end"] = max(entry["end"], event["t"])
        if event["e"] == "$pageview":
            entry["pageviews"] += 1
            if not entry["landing"] or event["t"] <= entry["start"]:
                entry["landing"] = _path_name(event, "$pageview")
        elif event["e"] in ("$autocapture", "$rageclick"):
            entry["interactions"] += 1
        for index, goal in enumerate(goals):
            if _series_matches(event, goal, context):
                entry["goals"][index] = 1
    return sessions


def _campaign_totals(sessions, goals):
    count = len(sessions)
    bounces = sum(1 for entry in sessions if entry["pageviews"] <= 1
                  and not entry["interactions"]
                  and entry["end"] - entry["start"] < 10000)
    converted = sum(1 for entry in sessions if any(entry["goals"]))
    durations = [entry["end"] - entry["start"] for entry in sessions]
    return {
        "sessions": count,
        "visitors": len({entry["actor"] for entry in sessions}),
        "pageviews": sum(entry["pageviews"] for entry in sessions),
        "bounces": bounces,
        "bounceRate": round(bounces / count, 4) if count else 0,
        "averageSessionMs": int(sum(durations) / count) if count else 0,
        "conversions": converted if goals else None,
        "conversionRate": (round(converted / count, 4) if count else 0)
        if goals else None,
        "goals": [sum(entry["goals"][index] for entry in sessions)
                  for index in range(len(goals))],
    }


def campaigns(events, query, context, start, end):
    filters = _filters(query.get("filters"))
    goals = _series(query.get("goals"), 5)
    group = _text(query.get("groupBy"), 20)
    group = group if group in CAMPAIGN_GROUPS else "campaign"
    limit = _bounded(query.get("limit"), 1, 200, 50)
    own_host = _text(query.get("host"), 253).strip().lower()
    own_host = own_host[4:] if own_host.startswith("www.") else own_host
    span = max(1, end - start)
    current = _campaign_sessions(
        [event for event in events if start <= event["t"] <= end],
        filters, goals, context)
    previous = _campaign_sessions(
        [event for event in events if start - span <= event["t"] < start],
        filters, goals, context)

    def key_of(entry):
        channel = _session_channel(entry["first"], own_host)
        entry["channel"] = channel
        if group == "landing":
            return (entry["landing"] or "(unknown)",)
        if group == "term":
            return (channel["term"],)
        if group == "source":
            return (channel["source"], channel["medium"])
        if group == "content":
            return (channel["source"], channel["medium"], channel["campaign"],
                    channel["content"])
        return (channel["source"], channel["medium"], channel["campaign"])

    buckets, earlier = {}, {}
    for entry in current.values():
        buckets.setdefault(key_of(entry), []).append(entry)
    for entry in previous.values():
        earlier.setdefault(key_of(entry), []).append(entry)
    rows = []
    for key, members in buckets.items():
        before = _campaign_totals(earlier.get(key, []), goals)
        landing = {}
        for entry in members:
            if entry["landing"]:
                landing[entry["landing"]] = landing.get(entry["landing"], 0) + 1
        row = {"key": " / ".join(key), **_campaign_totals(members, goals),
               "previousSessions": before["sessions"],
               "previousConversions": before["conversions"],
               "tagged": any(entry["channel"]["tagged"] for entry in members),
               "topLanding": _top(landing, 3)}
        if group == "landing":
            row["landing"] = key[0]
        elif group == "term":
            row["term"] = key[0]
            sources = {}
            for entry in members:
                label = entry["channel"]["source"] + " / " + entry["channel"]["medium"]
                sources[label] = sources.get(label, 0) + 1
            row["topSources"] = _top(sources, 3)
        else:
            row.update({"source": key[0], "medium": key[1]})
            if group in ("campaign", "content"):
                row["campaign"] = key[2]
            if group == "content":
                row["content"] = key[3]
        rows.append(row)
    rows.sort(key=lambda item: (-item["sessions"], item["key"]))
    totals = _campaign_totals(list(current.values()), goals)
    tagged = sum(1 for entry in current.values() if entry["channel"]["tagged"])
    totals["taggedSessions"] = tagged
    totals["taggedShare"] = round(tagged / totals["sessions"], 4) \
        if totals["sessions"] else 0
    return {"groupBy": group,
            "goals": [_series_label(goal, context) for goal in goals],
            "totals": totals,
            "previous": _campaign_totals(list(previous.values()), goals),
            "rows": rows[:limit], "rowCount": len(rows)}


INSIGHTS = {
    "overview": overview, "trends": trends, "funnel": funnel,
    "retention": retention, "paths": paths, "stickiness": stickiness,
    "lifecycle": lifecycle, "events": event_list, "heatmap": heatmap,
    "errors": errors, "taxonomy": taxonomy, "propertyValues": property_values,
    "experiment": experiment, "survey": survey_results, "sessions": session_list,
    "campaigns": campaigns,
}
# Insights that look at history before the chosen window to classify it.
LOOKBACK_KINDS = {"overview": 1.0, "lifecycle": 1.0, "retention": 0.0,
                  "campaigns": 1.0}


# --------------------------------------------------------------------------
# Cohorts


def _cohort_data(data):
    static = [_text(item, 200) for item in (
        data.get("distinctIds") if isinstance(data.get("distinctIds"), list)
        else [])[:2000] if _text(item, 200)]
    conditions = []
    for raw in (data.get("conditions") if isinstance(data.get("conditions"), list)
                else [])[:12]:
        if not isinstance(raw, dict):
            continue
        if raw.get("kind") == "event":
            conditions.append({
                "kind": "event",
                "event": _clean(raw.get("event"), MAX_EVENT_NAME) or "$pageview",
                "performed": raw.get("performed") is not False,
                "minCount": _bounded(raw.get("minCount"), 1, 10000, 1),
                "days": _bounded(raw.get("days"), 1, 400, 30)})
        else:
            parsed = _filters([dict(raw, type="person")])
            if parsed:
                conditions.append(dict(parsed[0], kind="property"))
    return {"static": bool(data.get("static")) or bool(static and not conditions),
            "distinctIds": static,
            "match": "any" if data.get("match") == "any" else "all",
            "conditions": conditions}


def cohort_members(cohort, events, context, now):
    """Person ids inside one cohort, over the loaded events and people."""
    if cohort.get("static"):
        return {context.aliases.get(_hash(context.project_id, distinct),
                                    _hash(context.project_id, distinct))
                for distinct in cohort.get("distinctIds") or ()}
    conditions = cohort.get("conditions") or []
    if not conditions:
        return set()
    counts = {}
    for position, condition in enumerate(conditions):
        if condition["kind"] != "event":
            continue
        since = now - condition["days"] * DAY_MS
        for event in events:
            if event["t"] >= since and event["e"] == condition["event"]:
                key = (position, context.actor(event))
                counts[key] = counts.get(key, 0) + 1
    people = set(context.persons) | {context.actor(event) for event in events}
    members = set()
    for person in people:
        properties = (context.persons.get(person) or {}).get("properties") or {}
        outcomes = []
        for position, condition in enumerate(conditions):
            if condition["kind"] == "event":
                did = counts.get((position, person), 0) >= condition["minCount"]
                outcomes.append(did == condition["performed"])
            else:
                outcomes.append(_matches_value(
                    properties.get(condition["property"]), condition["operator"],
                    condition.get("value")))
        if (any(outcomes) if cohort.get("match") == "any" else all(outcomes)):
            members.add(person)
    return members


# --------------------------------------------------------------------------
# Feature flags, experiments, surveys


def _flag_data(data, existing=None):
    result = dict(existing or {})
    if "key" in data or "key" not in result:
        result["key"] = _text(data.get("key"), 80)
    if "name" in data or "name" not in result:
        result["name"] = _clean(data.get("name"), 120)
    if "description" in data:
        result["description"] = _text(data.get("description"), 1000)
    if "active" in data or "active" not in result:
        result["active"] = bool(data.get("active", True))
    if "conditions" in data or "conditions" not in result:
        conditions = []
        for raw in (data.get("conditions") if isinstance(
                data.get("conditions"), list) else [])[:10]:
            if isinstance(raw, dict):
                conditions.append({
                    "properties": _filters([dict(item, type=item.get("type")
                                                 if item.get("type") in (
                                                     "person", "cohort") else "person")
                                            for item in (raw.get("properties") or [])
                                            if isinstance(item, dict)]),
                    "rolloutPercent": _bounded(raw.get("rolloutPercent"), 0, 100, 100),
                    "variant": _text(raw.get("variant"), 80)})
        result["conditions"] = conditions or [{"properties": [],
                                               "rolloutPercent": 100, "variant": ""}]
    if "variants" in data or "variants" not in result:
        variants = []
        for raw in (data.get("variants") if isinstance(
                data.get("variants"), list) else [])[:10]:
            if isinstance(raw, dict) and FLAG_KEY_RE.fullmatch(_text(raw.get("key"), 80)):
                variants.append({"key": _text(raw.get("key"), 80),
                                 "percent": _bounded(raw.get("percent"), 0, 100, 0)})
        result["variants"] = variants
    if "payloads" in data or "payloads" not in result:
        payloads = data.get("payloads") if isinstance(data.get("payloads"), dict) else {}
        result["payloads"] = {_text(key, 80): _json_value(value)
                              for key, value in list(payloads.items())[:12]}
    return result


def _flag_error(flag):
    if not FLAG_KEY_RE.fullmatch(flag.get("key") or ""):
        return "invalid_flag_key"
    variants = flag.get("variants") or []
    if variants and sum(item["percent"] for item in variants) != 100:
        return "variant_percentages_must_total_100"
    return ""


def _flag_hash(key, distinct, salt=""):
    """A stable point in [0, 1) for this visitor and flag."""
    digest = hashlib.sha1(("%s.%s%s" % (key, distinct, salt)).encode("utf-8"))
    return int(digest.hexdigest()[:15], 16) / float(0xFFFFFFFFFFFFFFF)


def evaluate_flag(flag, distinct, properties, cohorts=None, person_id=""):
    """The flag's value for one visitor: False, True, or a variant key."""
    if not flag.get("active"):
        return False
    for condition in flag.get("conditions") or ():
        matched = True
        for item in condition.get("properties") or ():
            if item.get("type") == "cohort":
                inside = person_id in ((cohorts or {}).get(item.get("value")) or ())
                if inside != (item.get("operator") != "is_not"):
                    matched = False
                    break
                continue
            if not _matches_value(properties.get(item["property"]),
                                  item["operator"], item.get("value")):
                matched = False
                break
        if not matched:
            continue
        if _flag_hash(flag["key"], distinct) >= condition["rolloutPercent"] / 100.0:
            continue
        variants = flag.get("variants") or []
        if not variants:
            return True
        if condition.get("variant") and any(
                item["key"] == condition["variant"] for item in variants):
            return condition["variant"]
        point, ceiling = _flag_hash(flag["key"], distinct, "variant"), 0.0
        for item in variants:
            ceiling += item["percent"] / 100.0
            if point < ceiling:
                return item["key"]
        return variants[-1]["key"]
    return False


def _experiment_data(data, existing=None):
    result = dict(existing or {})
    for key, limit in (("name", 120), ("description", 1000), ("flagKey", 80)):
        if key in data or key not in result:
            result[key] = _text(data.get(key), limit)
    if "goal" in data or "goal" not in result:
        result["goal"] = (_series([data.get("goal") or {"event": "$pageview"}])
                          or _series([{"event": "$pageview"}]))[0]
    if "status" in data or "status" not in result:
        status = _text(data.get("status") or "draft", 12)
        result["status"] = status if status in (
            "draft", "running", "stopped") else "draft"
    for key in ("startedAt", "endedAt"):
        if key in data:
            result[key] = max(0, _integer(data.get(key)))
    return result


def _survey_data(data, existing=None):
    result = dict(existing or {})
    for key, limit in (("name", 120), ("description", 1000)):
        if key in data or key not in result:
            result[key] = _text(data.get(key), limit)
    if "active" in data or "active" not in result:
        result["active"] = bool(data.get("active"))
    if "questions" in data or "questions" not in result:
        questions = []
        for raw in (data.get("questions") if isinstance(
                data.get("questions"), list) else [])[:10]:
            if not isinstance(raw, dict):
                continue
            kind = _text(raw.get("type") or "open", 12)
            questions.append({
                "type": kind if kind in SURVEY_QUESTION_TYPES else "open",
                "question": _text(raw.get("question"), 300),
                "description": _text(raw.get("description"), 500),
                "choices": [_text(item, 120) for item in (
                    raw.get("choices") if isinstance(raw.get("choices"), list)
                    else [])[:12] if _text(item, 120)],
                "scale": _bounded(raw.get("scale"), 3, 10, 5),
                "lowLabel": _text(raw.get("lowLabel"), 60),
                "highLabel": _text(raw.get("highLabel"), 60),
                "link": _text(raw.get("link"), 500)
                if str(raw.get("link") or "").startswith("https://") else "",
                "buttonText": _text(raw.get("buttonText"), 40),
                "optional": bool(raw.get("optional"))})
        result["questions"] = questions
    if "targeting" in data or "targeting" not in result:
        raw = data.get("targeting") if isinstance(data.get("targeting"), dict) else {}
        matching = _text(raw.get("urlMatching") or "contains", 10)
        result["targeting"] = {
            "url": _text(raw.get("url"), 300),
            "urlMatching": matching if matching in ("contains", "exact", "regex")
            else "contains",
            "selector": _text(raw.get("selector"), 200),
            "flagKey": _text(raw.get("flagKey"), 80),
            "event": _clean(raw.get("event"), MAX_EVENT_NAME),
            "delaySeconds": _bounded(raw.get("delaySeconds"), 0, 600, 0),
            "waitDays": _bounded(raw.get("waitDays"), 0, 365, 30)}
    if "appearance" in data or "appearance" not in result:
        raw = data.get("appearance") if isinstance(data.get("appearance"), dict) else {}
        position = _text(raw.get("position") or "right", 8)
        result["appearance"] = {
            "position": position if position in ("left", "center", "right") else "right",
            "thankYou": _text(raw.get("thankYou") or "Thank you!", 200),
            "accent": _text(raw.get("accent"), 9)
            if re.fullmatch(r"#[0-9a-fA-F]{3,8}", str(raw.get("accent") or "")) else ""}
    return result


async def _decide_config(runtime, project, now):
    cached = _DECIDE_CACHE.get(project["id"])
    if cached and now - cached[0] < DECIDE_CACHE_MS:
        return cached[1]
    rows = await runtime.d1_all(
        "SELECT object_id,kind,data FROM analytics_objects WHERE project_id=? "
        "AND kind IN ('flag','survey','cohort') ORDER BY updated_at DESC LIMIT 400",
        project["id"]) or []
    config = {"flags": [], "surveys": [], "cohorts": {}}
    for row in rows:
        data = await _open(runtime, row.get("data"), {})
        if row["kind"] == "flag" and data.get("active"):
            config["flags"].append(data)
        elif row["kind"] == "survey" and data.get("active"):
            config["surveys"].append(dict(data, id=str(row["object_id"])))
        elif row["kind"] == "cohort":
            config["cohorts"][str(row["object_id"])] = data
    if len(_DECIDE_CACHE) > 200:
        _DECIDE_CACHE.clear()
    _DECIDE_CACHE[project["id"]] = (now, config)
    return config


async def _decide(runtime, data):
    now = runtime.now()
    key = _text(data.get("key"), 64)
    if not KEY_RE.fullmatch(key):
        return _response(runtime, {"error": "invalid_key"}, 401, public=True)
    project = await _ingest_project(runtime, key, now)
    if not project:
        return _response(runtime, {"error": "unknown_project"}, 401, public=True)
    settings = project["settings"]
    if not _origin_allowed(runtime, settings):
        return _response(runtime, {"error": "origin_not_allowed"}, 403, public=True)
    distinct = _text(data.get("distinctId"), 200)
    config = await _decide_config(runtime, project, now)
    properties = _properties(data.get("personProperties"))
    flags, payloads = {}, {}
    if distinct and config["flags"]:
        person_id = ""
        needs_person = any(condition.get("properties")
                           for flag in config["flags"]
                           for condition in flag.get("conditions") or ())
        if needs_person:
            person_id = await _resolve_person(runtime, project["id"], distinct)
            row = await _person_row(runtime, project["id"], person_id)
            stored = (await _open(runtime, (row or {}).get("data"), {})).get(
                "properties") or {}
            properties = dict(stored, **properties)
        # Only static cohorts can gate a flag: a behavioural cohort needs the
        # event history, which the public path deliberately never loads.
        cohorts = {cohort_id: {
            _hash(project["id"], item) for item in cohort.get("distinctIds") or ()}
            for cohort_id, cohort in config["cohorts"].items()
            if cohort.get("static")}
        own = _hash(project["id"], distinct)
        for flag in config["flags"]:
            value = evaluate_flag(flag, distinct, properties, cohorts,
                                  person_id or own)
            if value is not False:
                flags[flag["key"]] = value
                payload = (flag.get("payloads") or {}).get(
                    "true" if value is True else str(value))
                if payload is not None:
                    payloads[flag["key"]] = payload
    replay = dict(settings.get("replay") or {})
    return _response(runtime, {
        "ok": True, "flags": flags, "payloads": payloads,
        "surveys": [{"id": survey["id"], "name": survey.get("name") or "",
                     "questions": survey.get("questions") or [],
                     "targeting": survey.get("targeting") or {},
                     "appearance": survey.get("appearance") or {}}
                    for survey in config["surveys"]][:20],
        "replay": replay, "autocapture": bool(settings.get("autocapture")),
        "heatmaps": bool(settings.get("heatmaps")),
        "captureErrors": bool(settings.get("captureErrors")),
        "capturePerformance": bool(settings.get("capturePerformance")),
    }, public=True)


# --------------------------------------------------------------------------
# Loading events and people for a query


async def _load_events(runtime, project_id, start, end):
    rows = await runtime.d1_all(
        "SELECT data FROM analytics_batches WHERE project_id=? AND day>=? "
        "AND day<=? AND last_ts>=? AND first_ts<=? ORDER BY first_ts DESC LIMIT ?",
        project_id, _day(start - 7 * DAY_MS), _day(end), start, end,
        MAX_QUERY_BATCHES) or []
    events, truncated = [], len(rows) >= MAX_QUERY_BATCHES
    for row in rows:
        for event in await _open(runtime, row.get("data"), []):
            if isinstance(event, dict) and start <= _integer(event.get("t")) <= end:
                event.setdefault("p", {})
                events.append(event)
        if len(events) >= MAX_QUERY_EVENTS:
            truncated = True
            break
    events.sort(key=lambda item: item["t"])
    return events, truncated


async def _load_aliases(runtime, project_id):
    rows = await runtime.d1_all(
        "SELECT id_hash,person_id FROM analytics_aliases WHERE project_id=? "
        "LIMIT ?", project_id, MAX_ALIAS_SCAN) or []
    return {str(row["id_hash"]): str(row["person_id"]) for row in rows}


async def _load_persons(runtime, project_id, limit=MAX_PERSON_SCAN):
    rows = await runtime.d1_all(
        "SELECT person_id,data FROM analytics_persons WHERE project_id=? "
        "AND identified=1 ORDER BY last_seen DESC LIMIT ?", project_id, limit) or []
    return {str(row["person_id"]): await _open(runtime, row.get("data"), {})
            for row in rows}


async def _load_groups(runtime, project_id):
    rows = await runtime.d1_all(
        "SELECT group_type,data FROM analytics_groups WHERE project_id=? "
        "ORDER BY last_seen DESC LIMIT 2000", project_id) or []
    groups = {}
    for row in rows:
        data = await _open(runtime, row.get("data"), {})
        groups[(str(row["group_type"]), data.get("key"))] = \
            data.get("properties") or {}
    return groups


async def _objects(runtime, project_id, kind):
    rows = await runtime.d1_all(
        "SELECT object_id,kind,created_by_name,updated_by_name,created_at,"
        "updated_at,data FROM analytics_objects WHERE project_id=? AND kind=? "
        "ORDER BY updated_at DESC LIMIT ?", project_id, kind,
        MAX_OBJECTS_PER_KIND) or []
    return [await _object_project(runtime, row) for row in rows]


def _uses(query, kind):
    text = json.dumps(query)
    return ('"type": "%s"' % kind) in text


async def _context(runtime, project, query, events, now):
    project_id = project["id"]
    context = _Context(project_id, aliases=await _load_aliases(runtime, project_id))
    text = json.dumps(query)
    if "action:" in text:
        context.actions = {item["id"]: item for item in
                           await _objects(runtime, project_id, "action")}
    needs_cohort = _uses(query, "cohort")
    if (needs_cohort or _uses(query, "person")
            or query.get("kind") in ("taxonomy", "propertyValues")):
        context.persons = await _load_persons(runtime, project_id)
    if _uses(query, "group"):
        context.groups = await _load_groups(runtime, project_id)
    if needs_cohort:
        for cohort in await _objects(runtime, project_id, "cohort"):
            if cohort["id"] in text:
                context.cohorts[cohort["id"]] = cohort_members(
                    cohort, events, context, now)
    return context


def _window(query, now):
    end = _integer(query.get("to"), now)
    start = _integer(query.get("from"), end - 7 * DAY_MS)
    end = min(end, now + DAY_MS)
    if start >= end:
        start = end - DAY_MS
    return max(start, end - 400 * DAY_MS), end


async def _query(runtime, project, query, now):
    kind = _text(query.get("kind"), 20)
    if kind not in INSIGHTS:
        return _response(runtime, {"error": "unknown_query_kind",
                                   "kinds": list(QUERY_KINDS)}, 400)
    return _response(runtime, {"ok": True, **await _run_query(
        runtime, project, kind, query, now)})


async def _run_query(runtime, project, kind, query, now):
    prepared = await _prepare_query(runtime, project, kind, query, now)
    events, merged, context, start, end, truncated = prepared
    result = INSIGHTS[kind](events, merged, context, start, end)
    return {"kind": kind, "from": start, "to": end, "truncated": truncated,
            "scannedEvents": len(events), "result": result}


async def _prepare_query(runtime, project, kind, query, now):
    start, end = _window(query, now)
    load_from = start
    if kind in LOOKBACK_KINDS:
        load_from = start - int((end - start) * LOOKBACK_KINDS[kind])
    if kind in ("experiment", "survey"):
        load_from = min(load_from, start)
    events, truncated = await _load_events(runtime, project["id"], load_from, end)
    settings = project["settings"]
    merged = dict(query)
    if query.get("filterTestAccounts") and settings.get("testAccountFilters"):
        merged["filters"] = list(query.get("filters") or []) + list(
            settings["testAccountFilters"])
    if "tzOffsetMinutes" not in merged:
        merged["tzOffsetMinutes"] = settings.get("timezoneOffsetMinutes") or 0
    context = await _context(runtime, project, merged, events, now)
    return events, merged, context, start, end, truncated


async def campaign_report(runtime, org_bi, query, now=None):
    now = now or runtime.now()
    query = query if isinstance(query, dict) else {}
    if not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return {"status": "not_installed"}
    try:
        rows = await _project_rows(runtime, org_bi)
    except Exception as failure:
        if "no such table" in str(failure).lower():
            return {"status": "not_installed"}
        raise
    if not rows:
        return {"status": "no_project"}
    projects = []
    for row in rows:
        settings = _settings_data(None, await _open(runtime, row.get("data"), {}))
        projects.append({"id": str(row["project_id"]), "name": settings["name"],
                         "siteDefault": bool(row.get("site_default")),
                         "allowedOrigins": settings["allowedOrigins"],
                         "settings": settings})
    wanted = _text(query.get("project"), 32)
    chosen = (next((item for item in projects if item["id"] == wanted), None)
              or next((item for item in projects if item["siteDefault"]), None)
              or projects[0])
    latest = await runtime.d1_first(
        "SELECT last_ts FROM analytics_batches WHERE project_id=? "
        "ORDER BY day DESC,first_ts DESC LIMIT 1", chosen["id"])
    events, merged, context, start, end, truncated = await _prepare_query(
        runtime, {"id": chosen["id"], "settings": chosen["settings"]},
        "campaigns", query, now)
    groups = query.get("groups") if isinstance(query.get("groups"), list) else []
    groups = [item for item in CAMPAIGN_GROUPS if item in groups] or ["campaign"]
    results = {group: campaigns(events, dict(merged, groupBy=group), context,
                                start, end) for group in groups}
    return {"status": "ok",
            "project": {key: chosen[key] for key in
                        ("id", "name", "siteDefault", "allowedOrigins")},
            "projects": [{key: item[key] for key in ("id", "name", "siteDefault")}
                         for item in projects],
            "lastEventAt": _integer((latest or {}).get("last_ts")),
            "from": start, "to": end, "truncated": truncated,
            "scannedEvents": len(events), "results": results}


# --------------------------------------------------------------------------
# Saved objects


def _insight_data(data, existing=None):
    result = dict(existing or {})
    for key, limit in (("name", 120), ("description", 1000)):
        if key in data or key not in result:
            result[key] = _text(data.get(key), limit)
    if "query" in data or "query" not in result:
        query = data.get("query") if isinstance(data.get("query"), dict) else {}
        encoded = json.dumps(query, default=str)
        result["query"] = json.loads(encoded) if len(encoded) <= 20000 else {}
    if "pinned" in data:
        result["pinned"] = bool(data.get("pinned"))
    return result


def _dashboard_data(data, existing=None):
    result = dict(existing or {})
    for key, limit in (("name", 120), ("description", 1000)):
        if key in data or key not in result:
            result[key] = _text(data.get(key), limit)
    if "tiles" in data or "tiles" not in result:
        tiles = []
        for raw in (data.get("tiles") if isinstance(data.get("tiles"), list)
                    else [])[:40]:
            if isinstance(raw, dict) and ID_RE.fullmatch(_text(raw.get("insightId"), 32)):
                tiles.append({"insightId": _text(raw.get("insightId"), 32),
                              "width": _bounded(raw.get("width"), 1, 4, 2)})
        result["tiles"] = tiles
    if "pinned" in data:
        result["pinned"] = bool(data.get("pinned"))
    return result


def _annotation_data(data, existing=None):
    result = dict(existing or {})
    if "content" in data or "content" not in result:
        result["content"] = _text(data.get("content"), 400)
    if "date" in data or "date" not in result:
        result["date"] = max(0, _integer(data.get("date")))
    return result


def _named(parser):
    """Wrap a parser whose object also carries a name and a description."""
    def build(data, existing=None):
        merged = dict(existing or {}, **data)
        result = parser(merged)
        result["name"] = _text(merged.get("name"), 120)
        result["description"] = _text(merged.get("description"), 1000)
        return result
    return build


OBJECT_PARSERS = {
    "insight": _insight_data, "dashboard": _dashboard_data,
    "cohort": _named(_cohort_data), "action": _named(_action_data),
    "annotation": _annotation_data, "flag": _flag_data,
    "experiment": _experiment_data, "survey": _survey_data,
}


async def _object_project(runtime, row):
    data = await _open(runtime, row.get("data"), {})
    return dict(data, id=str(row.get("object_id") or ""),
                kind=str(row.get("kind") or ""),
                createdBy=str(row.get("created_by_name") or ""),
                updatedBy=str(row.get("updated_by_name") or ""),
                createdAt=_integer(row.get("created_at")),
                updatedAt=_integer(row.get("updated_at")))


async def _handle_objects(runtime, method, project, actor, role, item, data):
    project_id = project["id"]
    if not item:
        if method == "GET":
            kind = _text(runtime.query_params().get("kind"), 16)
            if kind not in OBJECT_KINDS:
                return _response(runtime, {"error": "unknown_object_kind"}, 400)
            return _response(runtime, {"ok": True, "kind": kind,
                                       "objects": await _objects(runtime, project_id, kind)})
        kind = _text(data.get("kind"), 16)
        if kind not in OBJECT_KINDS:
            return _response(runtime, {"error": "unknown_object_kind"}, 400)
        parsed = OBJECT_PARSERS[kind](data)
        if kind == "flag":
            error = _flag_error(parsed)
            if not error:
                existing = await _objects(runtime, project_id, "flag")
                if any(flag.get("key") == parsed["key"] for flag in existing):
                    error = "flag_key_taken"
            if error:
                return _response(runtime, {"error": error}, 400)
        count = await runtime.d1_first(
            "SELECT COUNT(*) AS total FROM analytics_objects WHERE project_id=? "
            "AND kind=?", project_id, kind)
        if _integer((count or {}).get("total")) >= MAX_OBJECTS_PER_KIND:
            return _response(runtime, {"error": "object_limit_reached"}, 409)
        now, object_id = runtime.now(), runtime.new_id()
        await runtime.d1_run(
            "INSERT INTO analytics_objects(object_id,project_id,org_bi,kind,"
            "created_by_name,updated_by_name,created_at,updated_at,data) "
            "VALUES(?,?,?,?,?,?,?,?,?)", object_id, project_id, project["org_bi"],
            kind, actor, actor, now, now, await _seal(runtime, parsed))
        _DECIDE_CACHE.pop(project_id, None)
        await runtime.audit(actor, "analytics.%s.create" % kind,
                            "analytics_object", object_id)
        row = await runtime.d1_first(
            "SELECT object_id,kind,created_by_name,updated_by_name,created_at,"
            "updated_at,data FROM analytics_objects WHERE object_id=?", object_id)
        return _response(runtime, {"ok": True,
                                   "object": await _object_project(runtime, row)}, 201)
    row = await runtime.d1_first(
        "SELECT object_id,kind,created_by_name,updated_by_name,created_at,"
        "updated_at,data FROM analytics_objects WHERE project_id=? AND object_id=?",
        project_id, item)
    if not row:
        return _response(runtime, {"error": "object_not_found"}, 404)
    if method == "GET":
        return _response(runtime, {"ok": True,
                                   "object": await _object_project(runtime, row)})
    if method == "DELETE":
        await runtime.d1_run(
            "DELETE FROM analytics_objects WHERE project_id=? AND object_id=?",
            project_id, item)
        _DECIDE_CACHE.pop(project_id, None)
        await runtime.audit(actor, "analytics.%s.delete" % row["kind"],
                            "analytics_object", item)
        return _response(runtime, {"ok": True, "deleted": True})
    kind = str(row["kind"])
    parsed = OBJECT_PARSERS[kind](data, await _open(runtime, row.get("data"), {}))
    if kind == "flag":
        error = _flag_error(parsed)
        if error:
            return _response(runtime, {"error": error}, 400)
    now = runtime.now()
    await runtime.d1_run(
        "UPDATE analytics_objects SET data=?,updated_by_name=?,updated_at=? "
        "WHERE project_id=? AND object_id=?", await _seal(runtime, parsed), actor,
        now, project_id, item)
    _DECIDE_CACHE.pop(project_id, None)
    row = dict(row, data=await _seal(runtime, parsed), updated_by_name=actor,
               updated_at=now)
    return _response(runtime, {"ok": True,
                               "object": await _object_project(runtime, row)})


# --------------------------------------------------------------------------
# People, groups, recordings


async def _person_project(runtime, row):
    data = await _open(runtime, row.get("data"), {})
    return {"id": str(row.get("person_id") or ""),
            "identified": bool(row.get("identified")),
            "firstSeen": _integer(row.get("first_seen")),
            "lastSeen": _integer(row.get("last_seen")),
            "events": _integer(row.get("events")),
            "distinctIds": data.get("distinctIds") or [],
            "properties": data.get("properties") or {}}


async def _handle_persons(runtime, method, project, actor, role, item, now):
    project_id = project["id"]
    if not item:
        params = runtime.query_params()
        search = _text(params.get("search"), 120).lower()
        before = _integer(params.get("before"), now + DAY_MS)
        identified = params.get("identified") == "1"
        rows = await runtime.d1_all(
            "SELECT person_id,identified,first_seen,last_seen,events,data "
            "FROM analytics_persons WHERE project_id=? AND last_seen<? "
            + ("AND identified=1 " if identified or search else "")
            + "ORDER BY last_seen DESC LIMIT ?", project_id, before,
            600 if search else 100) or []
        people = []
        for row in rows:
            person = await _person_project(runtime, row)
            if search and search not in json.dumps(
                    [person["distinctIds"], person["properties"]]).lower():
                continue
            people.append(person)
            if len(people) >= 100:
                break
        total = await runtime.d1_first(
            "SELECT COUNT(*) AS total,SUM(identified) AS identified "
            "FROM analytics_persons WHERE project_id=?", project_id)
        return _response(runtime, {
            "ok": True, "persons": people,
            "total": _integer((total or {}).get("total")),
            "identified": _integer((total or {}).get("identified")),
            "nextBefore": people[-1]["lastSeen"] if len(rows) >= 100 and people
            else 0})
    row = await _person_row(runtime, project_id, item)
    if not row:
        return _response(runtime, {"error": "person_not_found"}, 404)
    if method == "DELETE":
        if role not in MANAGER_ROLES:
            return _response(runtime, {"error": "organization_admin_required"}, 403)
        removed = await _erase_person(runtime, project_id, item)
        await runtime.audit(actor, "analytics.person.delete", "analytics_person", item)
        return _response(runtime, {"ok": True, "deleted": True,
                                   "eventsRemoved": removed})
    person = await _person_project(runtime, row)
    events, _ = await _load_events(runtime, project_id, now - 30 * DAY_MS, now)
    context = _Context(project_id, aliases=await _load_aliases(runtime, project_id))
    recent = event_list(events, {"personId": item, "limit": 100}, context,
                        now - 30 * DAY_MS, now)
    recordings = await runtime.d1_all(
        "SELECT session_id,started_at,last_at,active_ms,clicks,keys,errors,bytes "
        "FROM analytics_recordings WHERE project_id=? AND person_id=? "
        "ORDER BY started_at DESC LIMIT 20", project_id, item) or []
    return _response(runtime, {"ok": True, "person": person,
                               "events": recent["events"],
                               "recordings": [_recording_row(r) for r in recordings]})


async def _erase_person(runtime, project_id, person_id):
    """Right-to-erasure: drop the person, their replays, and their events."""
    aliases = await runtime.d1_all(
        "SELECT id_hash FROM analytics_aliases WHERE project_id=? AND person_id=?",
        project_id, person_id) or []
    hashes = {person_id} | {str(row["id_hash"]) for row in aliases}
    await _ensure_page_schema(runtime)
    marks = ",".join("?" for _ in hashes)
    visited = await runtime.d1_all(
        "SELECT DISTINCT path FROM analytics_page_visitors WHERE project_id=? "
        "AND visitor IN (" + marks + ")", project_id, *sorted(hashes)) or []
    await runtime.d1_run(
        "DELETE FROM analytics_page_visitors WHERE project_id=? AND visitor IN ("
        + marks + ")", project_id, *sorted(hashes))
    for row in visited:
        await _recount_page(runtime, project_id, str(row["path"]))
    removed = 0
    rows = await runtime.d1_all(
        "SELECT batch_id,data FROM analytics_batches WHERE project_id=? "
        "ORDER BY first_ts DESC LIMIT ?", project_id, MAX_QUERY_BATCHES) or []
    for row in rows:
        events = await _open(runtime, row.get("data"), [])
        kept = [event for event in events
                if _hash(project_id, event.get("d")) not in hashes]
        if len(kept) == len(events):
            continue
        removed += len(events) - len(kept)
        if kept:
            await runtime.d1_run(
                "UPDATE analytics_batches SET data=?,events=? WHERE batch_id=?",
                await _seal(runtime, kept), len(kept), row["batch_id"])
        else:
            await runtime.d1_run(
                "DELETE FROM analytics_batches WHERE batch_id=?", row["batch_id"])
    await _run_batch(runtime, [
        ("DELETE FROM analytics_recording_chunks WHERE project_id=? AND session_id "
         "IN (SELECT session_id FROM analytics_recordings WHERE project_id=? "
         "AND person_id=?)", (project_id, project_id, person_id)),
        ("DELETE FROM analytics_recordings WHERE project_id=? AND person_id=?",
         (project_id, person_id)),
        ("DELETE FROM analytics_aliases WHERE project_id=? AND person_id=?",
         (project_id, person_id)),
        ("DELETE FROM analytics_persons WHERE project_id=? AND person_id=?",
         (project_id, person_id))])
    return removed


async def _handle_groups(runtime, project):
    group_type = _clean(runtime.query_params().get("type"), 40).lower()
    rows = await runtime.d1_all(
        "SELECT group_type,first_seen,last_seen,data FROM analytics_groups "
        "WHERE project_id=? " + ("AND group_type=? " if group_type else "")
        + "ORDER BY last_seen DESC LIMIT 300",
        *([project["id"], group_type] if group_type else [project["id"]])) or []
    groups = []
    for row in rows:
        data = await _open(runtime, row.get("data"), {})
        groups.append({"type": str(row["group_type"]), "key": data.get("key") or "",
                       "properties": data.get("properties") or {},
                       "firstSeen": _integer(row.get("first_seen")),
                       "lastSeen": _integer(row.get("last_seen"))})
    return _response(runtime, {"ok": True, "groups": groups,
                               "types": sorted({group["type"] for group in groups})})


def _recording_row(row):
    started, last = _integer(row.get("started_at")), _integer(row.get("last_at"))
    return {"sessionId": str(row.get("session_id") or ""),
            "personId": str(row.get("person_id") or ""),
            "startedAt": started, "lastAt": last,
            "durationMs": max(0, last - started),
            "activeMs": _integer(row.get("active_ms")),
            "chunks": _integer(row.get("chunks")),
            "bytes": _integer(row.get("bytes")),
            "clicks": _integer(row.get("clicks")),
            "keys": _integer(row.get("keys")),
            "errors": _integer(row.get("errors"))}


async def _handle_recordings(runtime, method, project, actor, role, item, now):
    project_id = project["id"]
    if not item:
        params = runtime.query_params()
        before = _integer(params.get("before"), now + DAY_MS)
        minimum = _bounded(params.get("minDurationMs"), 0, 86400000, 0)
        person = _text(params.get("personId"), 32)
        clauses, values = ["project_id=?", "started_at<?", "last_at-started_at>=?"], [
            project_id, before, minimum]
        if ID_RE.fullmatch(person):
            clauses.append("person_id=?")
            values.append(person)
        if params.get("errors") == "1":
            clauses.append("errors>0")
        rows = await runtime.d1_all(
            "SELECT session_id,person_id,started_at,last_at,active_ms,chunks,bytes,"
            "clicks,keys,errors,data FROM analytics_recordings WHERE "
            + " AND ".join(clauses) + " ORDER BY started_at DESC LIMIT 60",
            *values) or []
        recordings = []
        for row in rows:
            meta = await _open(runtime, row.get("data"), {})
            recordings.append(dict(_recording_row(row),
                                   startUrl=meta.get("startUrl") or "",
                                   distinctId=meta.get("distinctId") or "",
                                   device=meta.get("device") or {}))
        return _response(runtime, {
            "ok": True, "recordings": recordings,
            "nextBefore": recordings[-1]["startedAt"] if len(rows) >= 60 else 0})
    if not SESSION_RE.fullmatch(item):
        return _response(runtime, {"error": "recording_not_found"}, 404)
    row = await runtime.d1_first(
        "SELECT session_id,person_id,started_at,last_at,active_ms,chunks,bytes,"
        "clicks,keys,errors,data FROM analytics_recordings WHERE project_id=? "
        "AND session_id=?", project_id, item)
    if not row:
        return _response(runtime, {"error": "recording_not_found"}, 404)
    if method == "DELETE":
        await _run_batch(runtime, [
            ("DELETE FROM analytics_recording_chunks WHERE project_id=? "
             "AND session_id=?", (project_id, item)),
            ("DELETE FROM analytics_recordings WHERE project_id=? AND session_id=?",
             (project_id, item))])
        await runtime.audit(actor, "analytics.recording.delete",
                            "analytics_recording", item)
        return _response(runtime, {"ok": True, "deleted": True})
    params = runtime.query_params()
    after = _bounded(params.get("afterSeq"), -1, 100000, -1)
    chunk_rows = await runtime.d1_all(
        "SELECT seq,bytes,data FROM analytics_recording_chunks WHERE project_id=? "
        "AND session_id=? AND seq>? ORDER BY seq LIMIT 12", project_id, item,
        after) or []
    chunks, size = [], 0
    for chunk in chunk_rows:
        size += _integer(chunk.get("bytes"))
        # Keep one response well under the platform's body ceiling; the player
        # pages through the rest with afterSeq.
        if chunks and size > 3 * 1024 * 1024:
            break
        opened = await _open(runtime, chunk.get("data"), {})
        chunks.append({"seq": _integer(chunk.get("seq")),
                       "encoding": opened.get("encoding") or "json",
                       "events": opened.get("events") or ""})
    meta = await _open(runtime, row.get("data"), {})
    last_seq = chunks[-1]["seq"] if chunks else after
    remaining = await runtime.d1_first(
        "SELECT COUNT(*) AS total FROM analytics_recording_chunks WHERE "
        "project_id=? AND session_id=? AND seq>?", project_id, item, last_seq)
    return _response(runtime, {
        "ok": True,
        "recording": dict(_recording_row(row), startUrl=meta.get("startUrl") or "",
                          distinctId=meta.get("distinctId") or "",
                          viewport=meta.get("viewport") or {},
                          device=meta.get("device") or {}),
        "chunks": chunks,
        "nextAfterSeq": last_seq if _integer((remaining or {}).get("total")) else -1})


# --------------------------------------------------------------------------
# Projects


async def _create_project(runtime, org_bi, org_name, actor, data):
    rows = await _project_rows(runtime, org_bi)
    if len(rows) >= MAX_PROJECTS:
        return _response(runtime, {"error": "project_limit_reached"}, 409)
    now, project_id = runtime.now(), runtime.new_id()
    settings = _settings_data(data)
    await runtime.d1_run(
        "INSERT INTO analytics_projects(project_id,org_bi,org_name,write_key,"
        "site_default,created_by_name,created_at,updated_at,data) "
        "VALUES(?,?,?,?,0,?,?,?,?)", project_id, org_bi, org_name,
        _new_write_key(runtime), actor, now, now, await _seal(runtime, settings))
    await runtime.audit(actor, "analytics.project.create", "analytics_project",
                        project_id)
    row = await _project_row(runtime, org_bi, project_id)
    return _response(runtime, {"ok": True,
                               "project": await _project_project(runtime, row)}, 201)


async def _handle_project(runtime, method, org_bi, actor, role, row, data, action):
    project_id = str(row["project_id"])
    if method == "GET":
        usage = await _usage_today(runtime, project_id, runtime.now())
        history = await runtime.d1_all(
            "SELECT day,events,replay_bytes,requests FROM analytics_usage "
            "WHERE project_id=? ORDER BY day DESC LIMIT 30", project_id) or []
        return _response(runtime, {
            "ok": True, "project": await _project_project(runtime, row, usage),
            "usageHistory": [{"day": _integer(item.get("day")),
                              "events": _integer(item.get("events")),
                              "replayBytes": _integer(item.get("replay_bytes")),
                              "requests": _integer(item.get("requests"))}
                             for item in history]})
    if role not in MANAGER_ROLES:
        return _response(runtime, {"error": "organization_admin_required"}, 403)
    now = runtime.now()
    if action == "rotate-key":
        _forget_project(str(row["write_key"]), project_id)
        await runtime.d1_run(
            "UPDATE analytics_projects SET write_key=?,updated_at=? "
            "WHERE org_bi=? AND project_id=?", _new_write_key(runtime), now,
            org_bi, project_id)
        await runtime.audit(actor, "analytics.project.rotate_key",
                            "analytics_project", project_id)
    elif method == "DELETE":
        _forget_project(str(row["write_key"]), project_id)
        await _purge_project(runtime, project_id)
        await runtime.audit(actor, "analytics.project.delete", "analytics_project",
                            project_id)
        return _response(runtime, {"ok": True, "deleted": True})
    else:
        settings = _settings_data(data, await _open(runtime, row.get("data"), {}))
        site_default = _integer(row.get("site_default"))
        if "siteDefault" in data:
            # The bare "site" key is what this instance's own pages send, so
            # only an instance administrator may point it at a project.
            if not await runtime.is_admin(actor):
                return _response(runtime, {"error": "instance_admin_required"}, 403)
            site_default = 1 if data.get("siteDefault") else 0
            if site_default:
                await runtime.d1_run(
                    "UPDATE analytics_projects SET site_default=0 "
                    "WHERE site_default=1")
        await runtime.d1_run(
            "UPDATE analytics_projects SET data=?,site_default=?,updated_at=? "
            "WHERE org_bi=? AND project_id=?", await _seal(runtime, settings),
            site_default, now, org_bi, project_id)
        _forget_project(str(row["write_key"]), project_id)
        await runtime.audit(actor, "analytics.project.update", "analytics_project",
                            project_id)
    fresh = await _project_row(runtime, org_bi, project_id)
    return _response(runtime, {"ok": True,
                               "project": await _project_project(runtime, fresh)})


async def _purge_project(runtime, project_id):
    await _run_batch(runtime, [
        ("DELETE FROM " + table + " WHERE project_id=?", (project_id,))
        for table in DATA_TABLES])


async def _export_project(runtime, project, now):
    events, truncated = await _load_events(
        runtime, project["id"], now - 400 * DAY_MS, now)
    return _response(runtime, {
        "ok": True, "format": "forkmesh-analytics-events-v1",
        "exportedAt": now, "truncated": truncated or len(events) > MAX_EXPORT_EVENTS,
        "events": [{"event": event["e"], "distinctId": event["d"],
                    "timestamp": event["t"], "sessionId": event.get("s") or "",
                    "groups": event.get("g") or {}, "properties": event["p"]}
                   for event in events[-MAX_EXPORT_EVENTS:]]})


# --------------------------------------------------------------------------
# Organization lifecycle hooks and maintenance


async def _page_rows(runtime, project_id):
    await _ensure_page_schema(runtime)
    rows = await runtime.d1_all(
        "SELECT path,views,visitors FROM analytics_pages WHERE project_id=? "
        "ORDER BY visitors DESC,path LIMIT ?", project_id, MAX_EXPORT_EVENTS) or []
    return [{"path": str(row["path"]), "views": _integer(row["views"]),
             "visitors": _integer(row["visitors"])} for row in rows]


async def org_has_data(runtime, org_bi):
    return bool(await runtime.d1_first(
        "SELECT 1 AS present FROM analytics_projects WHERE org_bi=? LIMIT 1", org_bi))


async def export_org_data(runtime, org_bi, org_name):
    now = runtime.now()
    projects = []
    for row in await _project_rows(runtime, org_bi):
        project_id = str(row["project_id"])
        events, truncated = await _load_events(
            runtime, project_id, now - 400 * DAY_MS, now)
        saved = {}
        for kind in OBJECT_KINDS:
            saved[kind] = await _objects(runtime, project_id, kind)
        people = await runtime.d1_all(
            "SELECT person_id,identified,first_seen,last_seen,events,data "
            "FROM analytics_persons WHERE project_id=? ORDER BY last_seen DESC "
            "LIMIT ?", project_id, MAX_PERSON_SCAN) or []
        projects.append({
            "project": await _project_project(runtime, row),
            "events": events[-MAX_EXPORT_EVENTS:], "eventsTruncated": truncated,
            "persons": [await _person_project(runtime, person) for person in people],
            "pages": await _page_rows(runtime, project_id),
            "objects": saved})
    return {"plugin": PLUGIN_ID, "format": "forkmesh-analytics-export-v1",
            "organization": org_name, "exportedAt": now, "projects": projects}


async def purge_org_data(runtime, org_bi):
    _PROJECT_CACHE.clear()
    _DECIDE_CACHE.clear()
    for table in DATA_TABLES:
        await runtime.d1_run("DELETE FROM " + table + " WHERE org_bi=?", org_bi)


async def _compact_one(runtime, now):
    """Merge one finished day's small flush rows into a single sealed row."""
    today = _day(now)
    candidate = await runtime.d1_first(
        "SELECT project_id,day,COUNT(*) AS total FROM analytics_batches "
        "WHERE compacted=0 AND day<? GROUP BY project_id,day HAVING COUNT(*)>1 "
        "ORDER BY day LIMIT 1", today)
    if not candidate:
        # Nothing to merge: mark lone rows so the scan above stays small.
        await runtime.d1_run(
            "UPDATE analytics_batches SET compacted=1 WHERE compacted=0 AND day<? "
            "AND batch_id IN (SELECT batch_id FROM analytics_batches WHERE "
            "compacted=0 AND day<? LIMIT 50)", today, today)
        return 0
    rows = await runtime.d1_all(
        "SELECT batch_id,org_bi,data FROM analytics_batches WHERE project_id=? "
        "AND day=? AND compacted=0 ORDER BY first_ts LIMIT ?",
        candidate["project_id"], candidate["day"], COMPACT_MAX_ROWS) or []
    merged, used = [], []
    for row in rows:
        events = await _open(runtime, row.get("data"), [])
        if merged and len(merged) + len(events) > COMPACT_TARGET_EVENTS:
            break
        merged += events
        used.append(str(row["batch_id"]))
    if len(used) < 2:
        if used:
            await runtime.d1_run(
                "UPDATE analytics_batches SET compacted=1 WHERE batch_id=?", used[0])
        return 0
    merged.sort(key=lambda item: _integer(item.get("t")))
    statements = [(
        "INSERT INTO analytics_batches(batch_id,project_id,org_bi,day,first_ts,"
        "last_ts,events,compacted,data) VALUES(?,?,?,?,?,?,?,1,?)",
        (runtime.new_id(), candidate["project_id"], rows[0]["org_bi"],
         candidate["day"], _integer(merged[0].get("t")),
         _integer(merged[-1].get("t")), len(merged), await _seal(runtime, merged)))]
    statements += [("DELETE FROM analytics_batches WHERE batch_id=?", (batch_id,))
                   for batch_id in used]
    await _run_batch(runtime, statements)
    return len(used)


async def _expire(runtime, now):
    """Apply each project's retention, a bounded slice at a time."""
    rows = await runtime.d1_all(
        "SELECT project_id,data FROM analytics_projects ORDER BY updated_at "
        "LIMIT 200") or []
    for row in rows:
        settings = _settings_data(None, await _open(runtime, row.get("data"), {}))
        project_id = str(row["project_id"])
        await runtime.d1_run(
            "DELETE FROM analytics_batches WHERE project_id=? AND day<?",
            project_id, _day(now - settings["retentionDays"] * DAY_MS))
        replay_cutoff = now - settings["replayRetentionDays"] * DAY_MS
        await _run_batch(runtime, [
            ("DELETE FROM analytics_recording_chunks WHERE project_id=? AND "
             "created_at<?", (project_id, replay_cutoff)),
            ("DELETE FROM analytics_recordings WHERE project_id=? AND last_at<?",
             (project_id, replay_cutoff)),
            ("DELETE FROM analytics_usage WHERE project_id=? AND day<?",
             (project_id, _day(now - 120 * DAY_MS)))])


_MAINTENANCE = {"expired_at": 0}


async def deliver_reminders(runtime):
    """The host's periodic package hook: compaction and retention.

    The package has nothing to remind anyone about; this is simply the one
    scheduled slot a package is given. Each tick does one bounded merge, and
    retention runs at most once an hour per isolate.
    """
    now = runtime.now()
    await _compact_one(runtime, now)
    if now - _MAINTENANCE["expired_at"] >= 3600000:
        _MAINTENANCE["expired_at"] = now
        await _expire(runtime, now)
    return {"delivered": 0}


# --------------------------------------------------------------------------
# Routing


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ("organizations",)
    match = re.fullmatch(r"/api/analytics/_/(capture|decide|pages)", clean)
    if match:
        return ("public", match.group(1))
    match = re.fullmatch(
        r"/api/analytics/([a-z][a-z0-9-]{0,62})(?:/projects(?:/([0-9a-f]{32})"
        r"(?:/(query|objects|persons|groups|recordings|export|rotate-key)"
        r"(?:/([A-Za-z0-9_-]{8,64}))?)?)?)?", clean)
    if not match:
        return None
    return ("organization", match.group(1), "projects" if "/projects" in clean
            else "", match.group(2) or "", match.group(3) or "",
            match.group(4) or "")


async def _public(runtime, endpoint):
    method = runtime.method()
    if endpoint == "pages":
        if method == "OPTIONS":
            return _response(runtime, {"ok": True}, 200, "GET, OPTIONS", public=True)
        if method not in ("GET", "HEAD"):
            return _response(runtime, {"error": "method_not_allowed"}, 405,
                             "GET, OPTIONS", public=True)
        try:
            return await _page_count(runtime)
        except Exception as failure:
            if "no such table" in str(failure).lower():
                return _response(runtime, {"error": "unknown_project"}, 404,
                                 public=True)
            raise
    if method == "OPTIONS":
        return _response(runtime, {"ok": True}, 200, "POST, OPTIONS", public=True)
    if method != "POST":
        return _response(runtime, {"error": "method_not_allowed"}, 405,
                         "POST, OPTIONS", public=True)
    data, error = await runtime.json_body(MAX_CAPTURE_BYTES)
    if error:
        return _response(runtime, {"error": error},
                         413 if error == "payload_too_large" else 400, public=True)
    try:
        if endpoint == "decide":
            return await _decide(runtime, data)
        return await _capture(runtime, data)
    except Exception as failure:
        # An organization that uninstalled (tables dropped) must look exactly
        # like an unknown key to a snippet still deployed on its site.
        if "no such table" in str(failure).lower():
            return _response(runtime, {"error": "unknown_project"}, 401, public=True)
        raise


async def handle(runtime, path):
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    if route[0] == "public":
        return await _public(runtime, route[1])
    method = runtime.method()
    if route[0] == "organizations":
        allowed = ("GET",)
        org_name = section = project_id = resource = item = ""
    else:
        _, org_name, section, project_id, resource, item = route
        if not section:
            allowed = ("GET",)
        elif not project_id:
            allowed = ("GET", "POST")
        elif not resource:
            allowed = ("GET", "PATCH", "DELETE")
        elif resource in ("query", "rotate-key"):
            allowed = ("POST",) if not item else ()
        elif resource in ("groups", "export"):
            allowed = ("GET",) if not item else ()
        elif resource == "objects":
            allowed = ("GET", "PATCH", "DELETE") if item else ("GET", "POST")
        else:
            allowed = ("GET", "DELETE") if item else ("GET",)
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405,
                         ", ".join(allowed))
    if method not in ("GET", "HEAD") and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method not in ("GET", "HEAD"):
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(runtime, {"error": error},
                             413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    account_bi, record = await runtime.session(data)
    actor = _clean((record or {}).get("name"), 64).lower()
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    if route[0] == "organizations":
        rows = await runtime.d1_all(
            "SELECT o.name,m.role,p.plugin AS installed FROM org_members m "
            "JOIN orgs o ON o.org_bi=m.org_bi LEFT JOIN org_plugin_installs p "
            "ON p.org_bi=o.org_bi AND p.plugin=? WHERE m.member_bi=? "
            "ORDER BY o.name COLLATE NOCASE LIMIT 200", PLUGIN_ID, account_bi)
        return _response(runtime, {"ok": True, "actor": actor, "organizations": [
            {"name": str(row.get("name") or ""),
             "role": str(row.get("role") or "member"),
             "installed": bool(row.get("installed"))} for row in rows or []]})
    if not ORG_RE.fullmatch(org_name):
        return _response(runtime, {"error": "organization_not_found"}, 404)
    org_bi, role = await runtime.organization(org_name, account_bi)
    if not org_bi or not role:
        return _response(runtime, {"error": "organization_not_found"}, 404)
    if not await runtime.plugin_installed(org_bi, PLUGIN_ID):
        return _response(runtime, {"error": "plugin_not_installed",
                                   "organization": org_name,
                                   "canInstall": role in MANAGER_ROLES}, 409)
    now = runtime.now()
    if not project_id:
        if method == "POST":
            if role not in MANAGER_ROLES:
                return _response(runtime,
                                 {"error": "organization_admin_required"}, 403)
            return await _create_project(runtime, org_bi, org_name, actor, data)
        projects = []
        for row in await _project_rows(runtime, org_bi):
            projects.append(await _project_project(
                runtime, row, await _usage_today(runtime, str(row["project_id"]), now)))
        admin_check = getattr(runtime, "is_admin", None)
        return _response(runtime, {
            "ok": True, "organization": org_name, "role": role,
            "canManage": role in MANAGER_ROLES,
            "instanceAdmin": bool(admin_check and await admin_check(actor)),
            "projects": projects, "sdkVersion": SDK_VERSION,
            "objectKinds": list(OBJECT_KINDS), "queryKinds": list(QUERY_KINDS),
            "operators": list(OPERATORS), "maths": list(MATHS)})
    row = await _project_row(runtime, org_bi, project_id)
    if not row:
        return _response(runtime, {"error": "project_not_found"}, 404)
    if not resource or resource == "rotate-key":
        return await _handle_project(runtime, method, org_bi, actor, role, row,
                                     data, resource)
    project = {"id": project_id, "org_bi": org_bi, "settings": _settings_data(
        None, await _open(runtime, row.get("data"), {}))}
    if resource == "query":
        return await _query(runtime, project, data, now)
    if resource == "objects":
        if item and not ID_RE.fullmatch(item):
            return _response(runtime, {"error": "object_not_found"}, 404)
        return await _handle_objects(runtime, method, project, actor, role, item, data)
    if resource == "persons":
        if item and not ID_RE.fullmatch(item):
            return _response(runtime, {"error": "person_not_found"}, 404)
        return await _handle_persons(runtime, method, project, actor, role, item, now)
    if resource == "groups":
        return await _handle_groups(runtime, project)
    if resource == "export":
        return await _export_project(runtime, project, now)
    return await _handle_recordings(runtime, method, project, actor, role, item, now)
