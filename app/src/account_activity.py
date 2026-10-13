import datetime

ACTIVITY_SLOTS = 288
ACTIVITY_SLOT_MAX = 100000
ACTIVITY_DEVICES_PER_DAY = 24
ACTIVITY_KEEP_DAYS = 3
ACTIVITY_DAY_SLACK = 1
ACTIVITY_KINDS = ("desktop", "ios", "android", "web")
ACTIVITY_DEVICE_CHARS = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-")


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def _utc_day(now_ms):
    return datetime.datetime.fromtimestamp(
        now_ms / 1000, tz=datetime.timezone.utc).date()


def clean_activity_day(value, now_ms):
    text = str(value or "").strip()
    if len(text) != 10:
        return ""
    try:
        day = datetime.date.fromisoformat(text)
    except ValueError:
        return ""
    if abs((day - _utc_day(now_ms)).days) > ACTIVITY_DAY_SLACK:
        return ""
    return day.isoformat()


def clean_activity_device(value):
    text = str(value or "").strip()
    if not 8 <= len(text) <= 64 or not set(text) <= ACTIVITY_DEVICE_CHARS:
        return ""
    return text


def clean_activity_kind(value):
    text = str(value or "").strip().lower()
    return text if text in ACTIVITY_KINDS else ""


def clean_activity_counts(value):
    if not isinstance(value, list) or len(value) != ACTIVITY_SLOTS:
        return None
    if any(isinstance(item, bool) or not isinstance(item, int)
           for item in value):
        return None
    return [max(0, min(ACTIVITY_SLOT_MAX, item)) for item in value]


def parse_activity_counts(text):
    parts = str(text or "").split(",")
    if len(parts) != ACTIVITY_SLOTS:
        return [0] * ACTIVITY_SLOTS
    counts = []
    for part in parts:
        try:
            counts.append(max(0, min(ACTIVITY_SLOT_MAX, int(part))))
        except ValueError:
            counts.append(0)
    return counts


def summarize_activity_rows(rows, device):
    others = [0] * ACTIVITY_SLOTS
    total = [0] * ACTIVITY_SLOTS
    sources = []
    for row in rows or []:
        counts = parse_activity_counts(row.get("counts"))
        mine = str(row.get("device_key") or "") == device
        for slot, value in enumerate(counts):
            total[slot] += value
            if not mine:
                others[slot] += value
        sources.append({
            "kind": str(row.get("kind") or ""),
            "total": sum(counts),
            "updatedAt": int(row.get("updated_at") or 0),
            "self": mine,
        })
    sources.sort(key=lambda item: (-item["total"], item["kind"]))
    return {"others": others, "counts": total, "sources": sources}


async def account_activity_today_route(env, request):
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response({"error": "invalid_json"}, status=400)
    if not isinstance(data, dict):
        return json_response({"error": "invalid_json"}, status=400)
    name_bi, rec = await _account_session_record(env, request, data)
    if not rec or not name_bi:
        return json_response({"error": "unauthorized"}, status=401)
    now = int(Date.now())
    day = clean_activity_day(data.get("day"), now)
    if not day:
        return json_response({"error": "invalid_day"}, status=400)
    device = clean_activity_device(data.get("device"))
    if not device:
        return json_response({"error": "invalid_device"}, status=400)
    counts = None
    if "counts" in data:
        counts = clean_activity_counts(data.get("counts"))
        if counts is None:
            return json_response({"error": "invalid_counts"}, status=400)
    await ensure_schema(env)
    rows = list(await d1_all(
        env,
        "SELECT device_key,kind,counts,updated_at FROM account_activity_days "
        "WHERE account_bi=? AND day=? ORDER BY updated_at DESC LIMIT ?",
        name_bi, day, ACTIVITY_DEVICES_PER_DAY + 1) or [])
    if counts is not None:
        known = any(
            str(row.get("device_key") or "") == device for row in rows)
        if not known and len(rows) >= ACTIVITY_DEVICES_PER_DAY:
            return json_response(
                {"error": "too_many_activity_devices"}, status=429)
        kind = clean_activity_kind(data.get("kind"))
        joined = ",".join(str(value) for value in counts)
        await d1_run(
            env,
            "INSERT INTO account_activity_days "
            "(account_bi,day,device_key,kind,counts,updated_at) "
            "VALUES (?,?,?,?,?,?) "
            "ON CONFLICT(account_bi,day,device_key) DO UPDATE SET "
            "kind=excluded.kind,counts=excluded.counts,"
            "updated_at=excluded.updated_at",
            name_bi, day, device, kind, joined, now)
        if not known:
            floor = _utc_day(now) - datetime.timedelta(days=ACTIVITY_KEEP_DAYS)
            await d1_run(
                env,
                "DELETE FROM account_activity_days "
                "WHERE account_bi=? AND day<?",
                name_bi, floor.isoformat())
        rows = [row for row in rows
                if str(row.get("device_key") or "") != device]
        rows.insert(0, {
            "device_key": device, "kind": kind, "counts": joined,
            "updated_at": now,
        })
    return json_response({
        "ok": True,
        "nodeName": rec.get("name", ""),
        "day": day,
        **summarize_activity_rows(rows, device),
    }, cache_control="no-store, max-age=0")
