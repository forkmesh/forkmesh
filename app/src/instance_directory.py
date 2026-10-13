import base64
import binascii
import re

SCHEMA = (
    """CREATE TABLE IF NOT EXISTS instance_directory (
    domain TEXT PRIMARY KEY,
    id TEXT NOT NULL UNIQUE,
    name TEXT NOT NULL,
    url TEXT NOT NULL,
    logo TEXT NOT NULL DEFAULT '',
    world INTEGER NOT NULL DEFAULT 0 CHECK (world IN (0, 1)),
    listed INTEGER NOT NULL DEFAULT 0 CHECK (listed IN (0, 1)),
    slot INTEGER NOT NULL DEFAULT -1,
    hidden INTEGER NOT NULL DEFAULT 0 CHECK (hidden IN (0, 1)),
    failures INTEGER NOT NULL DEFAULT 0,
    offline_since INTEGER NOT NULL DEFAULT 0,
    checked_at INTEGER NOT NULL DEFAULT 0,
    pinged_at INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL
)""",
    """CREATE INDEX IF NOT EXISTS idx_instance_directory_created
    ON instance_directory(created_at)""",
    """CREATE INDEX IF NOT EXISTS idx_instance_directory_checked
    ON instance_directory(checked_at)""",
)
ID_RE = re.compile(r"^[0-9a-f]{32}$")
LOGO_RE = re.compile(
    r"^data:(image/(?:png|jpeg|webp|svg\+xml));base64,([A-Za-z0-9+/]+={0,2})$")
WORLD_SLOTS = 12
MAX_ENTRIES = 200
LIST_MAX_AGE = 60
LOGO_MAX_AGE = 3600
FALSE_WORDS = ("false", "no", "off", "0")
OFFLINE_FAILURES = 2
HEALTH_BATCH = 20
PING_AFTER_MS = 24 * 60 * 60 * 1000
VISIBLE = ("hidden=0 AND failures<%d AND (listed=1 OR (world=1 AND slot>=0))"
           % OFFLINE_FAILURES)
DOMAIN_RE = re.compile(
    r"^(?=.{4,253}$)(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+[a-z]{2,63}$")
_SCHEMA_READY = False


def flag(data, key):
    value = data.get(key)
    if value is None or value == "":
        return True
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return bool(value)
    if isinstance(value, str):
        return value.strip().lower() not in FALSE_WORDS
    return True


def listing(form):
    return {
        "name": form["projectName"],
        "logo": form["logo"],
        "world": bool(form["ownWorld"]),
        "listed": bool(form["listOnHomepage"]),
    }


async def ensure(runtime):
    global _SCHEMA_READY
    if _SCHEMA_READY:
        return
    for statement in SCHEMA:
        await runtime.d1_run(statement)
    _SCHEMA_READY = True


async def free_slot(runtime):
    rows = await runtime.d1_all(
        "SELECT slot FROM instance_directory WHERE slot>=0")
    taken = {int(row.get("slot") or 0) for row in rows or []}
    return next((slot for slot in range(WORLD_SLOTS) if slot not in taken), -1)


async def publish(runtime, launch_id, domain, entry):
    await ensure(runtime)
    if not entry or not (entry.get("world") or entry.get("listed")):
        await runtime.d1_run(
            "DELETE FROM instance_directory WHERE domain=?", domain)
        return None
    row = await runtime.d1_first(
        "SELECT slot FROM instance_directory WHERE domain=?", domain)
    slot = int(row["slot"]) if row else -1
    if not entry.get("world"):
        slot = -1
    elif slot < 0:
        slot = await free_slot(runtime)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO instance_directory (domain,id,name,url,logo,world,listed,"
        "slot,created_at,updated_at) VALUES (?,?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(domain) DO UPDATE SET name=excluded.name,"
        "url=excluded.url,logo=excluded.logo,world=excluded.world,"
        "listed=excluded.listed,slot=excluded.slot,failures=0,"
        "offline_since=0,pinged_at=0,checked_at=excluded.updated_at,"
        "updated_at=excluded.updated_at",
        domain, launch_id, entry.get("name") or domain,
        "https://%s/" % domain, entry.get("logo") or "",
        1 if entry.get("world") else 0, 1 if entry.get("listed") else 0,
        slot, now, now)
    return slot


def public_entry(row):
    world = bool(row.get("world")) and int(row.get("slot", -1)) >= 0
    logo = ""
    if row.get("has_logo"):
        logo = "/api/start/instances/%s/logo?v=%d" % (
            row["id"], int(row.get("updated_at") or 0))
    return {
        "id": row["id"],
        "name": row.get("name") or "",
        "url": row.get("url") or "",
        "logo": logo,
        "listed": bool(row.get("listed")),
        "world": world,
        "slot": int(row.get("slot", -1)) if world else -1,
        "createdAt": int(row.get("created_at") or 0),
    }


async def instances(runtime):
    await ensure(runtime)
    rows = await runtime.d1_all(
        "SELECT id,name,url,logo<>'' AS has_logo,world,listed,slot,"
        "created_at,updated_at FROM instance_directory WHERE " + VISIBLE +
        " ORDER BY created_at, domain LIMIT ?",
        MAX_ENTRIES)
    return runtime.public_response(
        {"ok": True, "instances": [public_entry(row) for row in rows or []]},
        LIST_MAX_AGE)


def decode_logo(value):
    match = LOGO_RE.fullmatch(str(value or ""))
    if not match:
        return "", b""
    try:
        return match.group(1), base64.b64decode(match.group(2), validate=True)
    except (binascii.Error, ValueError):
        return "", b""


async def logo(runtime, entry_id):
    if not ID_RE.fullmatch(entry_id):
        return None
    await ensure(runtime)
    row = await runtime.d1_first(
        "SELECT logo FROM instance_directory WHERE id=? AND " + VISIBLE,
        entry_id)
    media_type, raw = decode_logo((row or {}).get("logo"))
    if not raw:
        return None
    return runtime.image_response(raw, media_type, LOGO_MAX_AGE)


def health_url(domain):
    domain = str(domain or "").lower()
    return "https://%s/readyz" % domain if DOMAIN_RE.fullmatch(domain) else ""


def offline(row):
    return int(row.get("failures") or 0) >= OFFLINE_FAILURES


def offline_ping(row, now):
    name = row.get("name") or row["domain"]
    days = max(1, (now - int(row.get("offline_since") or now)) // (
        24 * 60 * 60 * 1000))
    return {
        "title": "Instance offline: %s" % name[:60],
        "body": "%s (%s) has not answered for %d day%s, so it is hidden from "
                "the homepage and the World. Should it be deleted from the "
                "instance directory? Open Instances to delete it or keep it."
                % (name, row["domain"], days, "" if days == 1 else "s"),
        "dedupe": "instance-offline:%s:%d" % (
            row["id"], int(row.get("offline_since") or 0)),
    }


async def check_health(runtime):
    await ensure(runtime)
    now = runtime.now()
    rows = await runtime.d1_all(
        "SELECT domain,id,name,hidden,failures,offline_since,pinged_at "
        "FROM instance_directory ORDER BY checked_at, domain LIMIT ?",
        HEALTH_BATCH)
    checked = pinged = 0
    for row in rows or []:
        row = dict(row)
        url = health_url(row["domain"])
        online = bool(url) and await runtime.probe(url)
        checked += 1
        if online:
            await runtime.d1_run(
                "UPDATE instance_directory SET failures=0,offline_since=0,"
                "pinged_at=0,checked_at=? WHERE domain=?", now, row["domain"])
            continue
        row["failures"] = int(row.get("failures") or 0) + 1
        row["offline_since"] = int(row.get("offline_since") or 0) or now
        due = (offline(row) and not int(row.get("hidden") or 0)
               and not int(row.get("pinged_at") or 0)
               and now - row["offline_since"] >= PING_AFTER_MS)
        await runtime.d1_run(
            "UPDATE instance_directory SET failures=?,offline_since=?,"
            "checked_at=?,pinged_at=? WHERE domain=?", row["failures"],
            row["offline_since"], now,
            now if due else int(row.get("pinged_at") or 0), row["domain"])
        if due:
            await runtime.ping_admins(offline_ping(row, now))
            pinged += 1
    return {"checked": checked, "pinged": pinged}


async def admin_rows(runtime):
    await ensure(runtime)
    rows = await runtime.d1_all(
        "SELECT domain,id,name,url,world,listed,slot,hidden,failures,"
        "offline_since,checked_at,pinged_at,created_at FROM instance_directory "
        "ORDER BY created_at, domain LIMIT ?", MAX_ENTRIES)
    return [dict(row) for row in rows or []]


async def set_hidden(runtime, entry_id, hidden):
    if not ID_RE.fullmatch(str(entry_id or "")):
        return None
    await ensure(runtime)
    row = await runtime.d1_first(
        "SELECT domain FROM instance_directory WHERE id=?", entry_id)
    if not row:
        return None
    await runtime.d1_run(
        "UPDATE instance_directory SET hidden=?,updated_at=? WHERE id=?",
        1 if hidden else 0, runtime.now(), entry_id)
    return row["domain"]


async def delete(runtime, entry_id):
    if not ID_RE.fullmatch(str(entry_id or "")):
        return None
    await ensure(runtime)
    row = await runtime.d1_first(
        "SELECT domain FROM instance_directory WHERE id=?", entry_id)
    if not row:
        return None
    await runtime.d1_run("DELETE FROM instance_directory WHERE id=?", entry_id)
    return row["domain"]
