from datetime import date, datetime, time as dtime, timedelta, timezone
from html import escape
from urllib.parse import quote as _quote
from zoneinfo import ZoneInfo
import hmac
import ipaddress
import re


PREFIX = "/api/booking"
PLUGIN_ID = "booking"
REQUEST_PROOF = "forkmesh-booking-request-v1"
PAGE_PATH = "/dashboard/booking"
PUBLIC_PATH = "/meet"
MAX_BODY_BYTES = 64 * 1024
KEY_VERSION = 1
MINUTE_MS = 60 * 1000
HOUR_MS = 60 * MINUTE_MS
DAY_MS = 24 * HOUR_MS
MAX_TYPES = 12
MAX_WINDOWS = 6
MAX_SLOT_RANGE_MS = 42 * DAY_MS
MAX_SLOTS = 3000
MAX_LISTED = 300
MAX_UPCOMING_PER_GUEST = 3
PAGE_DAILY_BOOKINGS = 40
VISITOR_HOURLY_BOOKINGS = 6
VISITOR_DAILY_BOOKINGS = 20
VISITOR_HOURLY_CHANGES = 12
MAIL_PER_BOOKING_DAILY = 6
MAIL_PER_GUEST_DAILY = 10
MAIL_PER_OWNER_ADDRESS_DAILY = 100
REMINDER_LEAD_MS = DAY_MS
REMINDER_BATCH = 25
DURATIONS = (10, 15, 20, 25, 30, 45, 50, 60, 75, 90, 120, 180, 240)
STEPS = (0, 5, 10, 15, 20, 30, 60)
BUFFERS = (0, 5, 10, 15, 30, 45, 60)
NOTICES = (0, 30, 60, 120, 240, 480, 720, 1440, 2880, 4320, 10080)
HORIZONS = (7, 14, 21, 30, 45, 60, 90, 120, 180, 365)
LOCATIONS = ("video", "phone", "place", "custom", "none")
LOCATION_LABELS = {
    "video": "Video call",
    "phone": "Phone call",
    "place": "In person",
    "custom": "Details after booking",
    "none": "",
}
ACCENTS = ("blue", "violet", "teal", "rose", "amber", "slate")
ACCENT_HEX = {
    "blue": "#2563eb",
    "violet": "#7c3aed",
    "teal": "#0d9488",
    "rose": "#e11d48",
    "amber": "#d97706",
    "slate": "#475569",
}
TIME_FORMATS = ("12h", "24h")
WEEKDAYS = ("mon", "tue", "wed", "thu", "fri", "sat", "sun")
WEEKDAY_NAMES = ("Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday")
MONTH_NAMES = ("January", "February", "March", "April", "May", "June", "July",
               "August", "September", "October", "November", "December")
RESERVED_SLUGS = frozenset({
    "admin", "api", "app", "book", "booking", "dashboard", "forkmesh", "help",
    "meet", "root", "security", "support", "system", "www",
})
SITE_ORIGINS = {"https://app.forkmesh.com": "https://forkmesh.com"}
APP_ORIGINS = {"https://forkmesh.com": "https://app.forkmesh.com",
               "https://www.forkmesh.com": "https://app.forkmesh.com"}
DEFAULT_ORIGIN = "https://forkmesh.com"
SLUG_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")
TYPE_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,30}[a-z0-9])?$")
ID_RE = re.compile(r"^[0-9a-f]{32}$")
TOKEN_RE = re.compile(r"^([0-9a-f]{32})\.([0-9a-f]{24})$")
CLOCK_RE = re.compile(r"^([01]\d|2[0-3]):([0-5]\d)$|^24:00$")
TIMEZONE_RE = re.compile(r"^[A-Za-z0-9_+-]+(?:/[A-Za-z0-9_+-]+){0,3}$")
ORIGIN_RE = re.compile(r"^https?://[a-z0-9](?:[a-z0-9.-]{0,251}[a-z0-9])?(?::[0-9]{1,5})?$")
HTTPS_RE = re.compile(r"^https://[^\s<>\"']{1,990}$", re.IGNORECASE)
EMAIL_RE = re.compile(
    r"^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@"
    r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?\.[A-Za-z]{2,63}$"
)
PHONE_RE = re.compile(r"^\+?[0-9 ().-]{6,32}$")
DEFAULT_AVAILABILITY = {
    day: ([["09:00", "17:00"]] if day not in ("sat", "sun") else []) for day in WEEKDAYS
}
DEFAULT_TYPES = [
    {"id": "15min", "title": "Quick chat", "minutes": 15,
     "description": "A short call to say hello or answer a quick question.",
     "location": {"kind": "video", "detail": ""}, "enabled": True},
    {"id": "30min", "title": "Meeting", "minutes": 30,
     "description": "Time to talk something through properly.",
     "location": {"kind": "video", "detail": ""}, "enabled": True},
]
STORED_KEYS = (
    "displayName", "headline", "welcome", "timezone", "timeFormat", "accent",
    "availability", "minNoticeMinutes", "maxDaysAhead", "bufferMinutes",
    "slotStepMinutes", "dailyLimit", "replyTo", "notifyEmail", "emailOwner",
    "addToCalendar", "checkCalendar", "types",
)


DATA_SCHEMA_UP = [
    """CREATE TABLE IF NOT EXISTS booking_keys (
        account_bi TEXT PRIMARY KEY,
        key_version INTEGER NOT NULL DEFAULT 1,
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        wrapped TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS booking_pages (
        account_bi TEXT PRIMARY KEY,
        account_name TEXT NOT NULL,
        slug TEXT NOT NULL UNIQUE,
        enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
        installed_at INTEGER NOT NULL CHECK(installed_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    """CREATE TABLE IF NOT EXISTS booking_bookings (
        booking_id TEXT PRIMARY KEY,
        account_bi TEXT NOT NULL,
        type_id TEXT NOT NULL,
        status TEXT NOT NULL CHECK(status IN ('confirmed','cancelled')),
        start_at INTEGER NOT NULL CHECK(start_at >= 0),
        end_at INTEGER NOT NULL CHECK(end_at > start_at),
        guest_bi TEXT NOT NULL,
        calendar_event_id TEXT NOT NULL DEFAULT '',
        reminded_at INTEGER NOT NULL DEFAULT 0 CHECK(reminded_at >= 0),
        cancelled_at INTEGER NOT NULL DEFAULT 0 CHECK(cancelled_at >= 0),
        cancelled_by TEXT NOT NULL DEFAULT '',
        created_at INTEGER NOT NULL CHECK(created_at >= 0),
        updated_at INTEGER NOT NULL CHECK(updated_at >= 0),
        data TEXT NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_booking_bookings_account_start "
    "ON booking_bookings(account_bi,start_at)",
    "CREATE UNIQUE INDEX IF NOT EXISTS idx_booking_bookings_slot "
    "ON booking_bookings(account_bi,start_at) WHERE status='confirmed'",
    "CREATE INDEX IF NOT EXISTS idx_booking_bookings_reminders "
    "ON booking_bookings(status,reminded_at,start_at)",
    "CREATE INDEX IF NOT EXISTS idx_booking_bookings_guest "
    "ON booking_bookings(account_bi,guest_bi,status,start_at)",
    """CREATE TABLE IF NOT EXISTS booking_rate (
        bucket TEXT PRIMARY KEY,
        count INTEGER NOT NULL DEFAULT 0,
        expires_at INTEGER NOT NULL)""",
    "CREATE INDEX IF NOT EXISTS idx_booking_rate_expires ON booking_rate(expires_at)",
]

DATA_SCHEMA_DOWN = [
    "DROP INDEX IF EXISTS idx_booking_rate_expires",
    "DROP TABLE IF EXISTS booking_rate",
    "DROP INDEX IF EXISTS idx_booking_bookings_guest",
    "DROP INDEX IF EXISTS idx_booking_bookings_reminders",
    "DROP INDEX IF EXISTS idx_booking_bookings_slot",
    "DROP INDEX IF EXISTS idx_booking_bookings_account_start",
    "DROP TABLE IF EXISTS booking_bookings",
    "DROP TABLE IF EXISTS booking_pages",
    "DROP TABLE IF EXISTS booking_keys",
]

BOOKING_COLUMNS = (
    "booking_id,account_bi,type_id,status,start_at,end_at,guest_bi,calendar_event_id,"
    "reminded_at,cancelled_at,cancelled_by,created_at,updated_at,data"
)

_SCHEMA_READY = False


def _response(runtime, payload, status=200, allow=""):
    headers = {"x-content-type-options": "nosniff", "referrer-policy": "no-referrer"}
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


def _text(value, limit):
    return str(value or "").replace("\x00", "").replace("\r\n", "\n")[:limit].strip()


def _integer(value, default=0):
    if isinstance(value, bool):
        return default
    try:
        return int(value)
    except (TypeError, ValueError):
        try:
            return int(float(value))
        except (TypeError, ValueError, OverflowError):
            return default


def _choice(value, options, default):
    number = _integer(value, default)
    return number if number in options else default


def _email(value):
    value = _clean(value, 254)
    return value.lower() if EMAIL_RE.fullmatch(value) else ""


def _https(value):
    value = _clean(value, 1000)
    return value if HTTPS_RE.fullmatch(value) else ""


def _zone(name):
    try:
        return ZoneInfo(str(name or "UTC"))
    except Exception:
        return timezone.utc


def _timezone_name(value, default="UTC"):
    value = _clean(value, 64)
    if not value or not TIMEZONE_RE.fullmatch(value):
        return default
    try:
        ZoneInfo(value)
    except Exception:
        return default
    return value


def _minutes(clock):
    if clock == "24:00":
        return 24 * 60
    hours, minutes = clock.split(":")
    return int(hours) * 60 + int(minutes)


def _clock(total):
    return "%02d:%02d" % divmod(total, 60)


def _availability(value):
    value = value if isinstance(value, dict) else {}
    result = {}
    for day in WEEKDAYS:
        spans = []
        for window in value.get(day) if isinstance(value.get(day), list) else []:
            if isinstance(window, dict):
                window = [window.get("start"), window.get("end")]
            if not isinstance(window, (list, tuple)) or len(window) != 2:
                continue
            start, end = str(window[0] or ""), str(window[1] or "")
            if not CLOCK_RE.fullmatch(start) or not CLOCK_RE.fullmatch(end) or start == "24:00":
                continue
            first, last = _minutes(start), _minutes(end)
            if last > first:
                spans.append([first, last])
        spans.sort()
        merged = []
        for first, last in spans:
            if merged and first <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], last)
            else:
                merged.append([first, last])
        result[day] = [[_clock(first), _clock(last)] for first, last in merged[:MAX_WINDOWS]]
    return result


def _location(value):
    value = value if isinstance(value, dict) else {}
    kind = _clean(value.get("kind"), 16).lower()
    kind = kind if kind in LOCATIONS else "video"
    detail = _clean(value.get("detail"), 500)
    if kind == "video" and detail and not _https(detail):
        detail = ""
    return {"kind": kind, "detail": detail}


def _types(value, fallback=None):
    source = value if isinstance(value, list) else (fallback if fallback is not None else DEFAULT_TYPES)
    result, seen = [], set()
    for item in source[:MAX_TYPES * 2]:
        if not isinstance(item, dict):
            continue
        title = _clean(item.get("title"), 80)
        minutes = _choice(item.get("minutes"), DURATIONS, 30)
        type_id = _clean(item.get("id"), 32).lower()
        if not TYPE_RE.fullmatch(type_id):
            type_id = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")[:32].strip("-")
        if not TYPE_RE.fullmatch(type_id or ""):
            type_id = "%dmin" % minutes
        base, counter = type_id, 2
        while type_id in seen:
            suffix = "-%d" % counter
            type_id = base[:32 - len(suffix)] + suffix
            counter += 1
        if not title:
            continue
        seen.add(type_id)
        result.append({
            "id": type_id,
            "title": title,
            "minutes": minutes,
            "description": _text(item.get("description"), 600),
            "location": _location(item.get("location")),
            "enabled": item.get("enabled") is not False,
        })
        if len(result) >= MAX_TYPES:
            break
    return result


def _settings_data(data, existing=None, account_name="", account_email=""):
    result = {
        "displayName": account_name,
        "headline": "Pick a time that works for you",
        "welcome": "",
        "timezone": "UTC",
        "timeFormat": "12h",
        "accent": "blue",
        "availability": _availability(DEFAULT_AVAILABILITY),
        "minNoticeMinutes": 240,
        "maxDaysAhead": 60,
        "bufferMinutes": 0,
        "slotStepMinutes": 0,
        "dailyLimit": 0,
        "replyTo": account_email,
        "notifyEmail": account_email,
        "emailOwner": True,
        "addToCalendar": True,
        "checkCalendar": True,
        "types": [dict(item) for item in DEFAULT_TYPES],
    }
    existing = existing if isinstance(existing, dict) else {}
    result.update({key: existing[key] for key in STORED_KEYS if key in existing})
    data = data if isinstance(data, dict) else {}
    if "displayName" in data:
        result["displayName"] = _clean(data.get("displayName"), 80)
    if "headline" in data:
        result["headline"] = _clean(data.get("headline"), 140)
    if "welcome" in data:
        result["welcome"] = _text(data.get("welcome"), 1500)
    if "timezone" in data:
        result["timezone"] = _timezone_name(data.get("timezone"), result["timezone"])
    if "timeFormat" in data:
        chosen = _clean(data.get("timeFormat"), 4)
        result["timeFormat"] = chosen if chosen in TIME_FORMATS else result["timeFormat"]
    if "accent" in data:
        chosen = _clean(data.get("accent"), 12).lower()
        result["accent"] = chosen if chosen in ACCENTS else result["accent"]
    if "availability" in data:
        result["availability"] = _availability(data.get("availability"))
    for key, options, default in (
        ("minNoticeMinutes", NOTICES, 240),
        ("maxDaysAhead", HORIZONS, 60),
        ("bufferMinutes", BUFFERS, 0),
        ("slotStepMinutes", STEPS, 0),
    ):
        if key in data:
            result[key] = _choice(data.get(key), options, default)
    if "dailyLimit" in data:
        result["dailyLimit"] = max(0, min(50, _integer(data.get("dailyLimit"))))
    for key in ("replyTo", "notifyEmail"):
        if key in data:
            result[key] = _email(data.get(key))
    for key in ("emailOwner", "addToCalendar", "checkCalendar"):
        if key in data:
            result[key] = bool(data.get(key))
    if "types" in data:
        result["types"] = _types(data.get("types"), [])
    else:
        result["types"] = _types(result.get("types"))
    result["availability"] = _availability(result.get("availability"))
    result["timezone"] = _timezone_name(result.get("timezone"))
    result["displayName"] = result["displayName"] or account_name
    return result


def _route(path):
    clean = str(path or "").rstrip("/")
    if clean == PREFIX:
        return ("owner",)
    if not clean.startswith(PREFIX + "/"):
        return None
    match = re.fullmatch(
        r"/api/booking/-/p/([a-z0-9-]{1,63})(?:/([a-z0-9-]{1,32})/(slots|book))?", clean)
    if match:
        return ("page", match.group(1), match.group(2) or "", match.group(3) or "")
    match = re.fullmatch(
        r"/api/booking/-/b/([0-9a-f]{32}\.[0-9a-f]{24})(?:/(cancel|reschedule|slots))?", clean)
    if match:
        return ("manage", match.group(1), match.group(2) or "")
    parts = tuple(part for part in clean[len(PREFIX) + 1:].split("/") if part)
    if any(len(part) > 64 for part in parts):
        return None
    return ("owner",) + parts


def _allowed(route):
    kind = route[0]
    if kind == "page":
        return ("POST",) if route[3] == "book" else ("GET",)
    if kind == "manage":
        return ("POST",) if route[2] in ("cancel", "reschedule") else ("GET",)
    parts = route[1:]
    if not parts:
        return ("GET",)
    head = parts[0]
    if head in ("install", "uninstall") and len(parts) == 1:
        return ("POST",)
    if head == "page" and len(parts) == 1:
        return ("GET", "PUT")
    if head == "export" and len(parts) == 1:
        return ("GET",)
    if head == "slug" and len(parts) == 2:
        return ("GET",)
    if head == "bookings" and len(parts) == 1:
        return ("GET",)
    if head == "bookings" and len(parts) == 3 and parts[2] == "cancel":
        return ("POST",)
    return ()


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
                "INSERT INTO booking_keys(account_bi,key_version,created_at,wrapped) "
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
            "SELECT wrapped FROM booking_keys WHERE account_bi=?", account_bi)
        if not row:
            return ""
        opened = await self._runtime.open(row.get("wrapped"))
        return str(opened.get("key") or "") if isinstance(opened, dict) else ""

    async def seal_for(self, account_bi, value):
        material = await self.key(account_bi, create=True)
        if not material:
            raise RuntimeError("booking: no key for this account")
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


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    record = record if isinstance(record, dict) else {}
    email = _email(record.get("email")) if record.get("email_verified") else ""
    return str(account_bi or ""), _clean(record.get("name"), 64).lower(), email


def _site_origin(runtime):
    secret = getattr(runtime, "package_secret", None)
    configured = str(secret("LINK_ORIGIN") if secret else "").rstrip("/").lower()
    if ORIGIN_RE.fullmatch(configured):
        return configured
    reader = getattr(runtime, "public_origin", None)
    origin = str(reader() if reader else "").rstrip("/").lower()
    if ORIGIN_RE.fullmatch(origin) and (origin.startswith("https://") or "localhost" in origin):
        return SITE_ORIGINS.get(origin, origin)
    return DEFAULT_ORIGIN


def _app_origin(runtime):
    secret = getattr(runtime, "package_secret", None)
    configured = str(secret("APP_ORIGIN") if secret else "").rstrip("/").lower()
    if ORIGIN_RE.fullmatch(configured):
        return configured
    reader = getattr(runtime, "public_origin", None)
    origin = str(reader() if reader else "").rstrip("/").lower()
    if ORIGIN_RE.fullmatch(origin) and (origin.startswith("https://") or "localhost" in origin):
        return APP_ORIGINS.get(origin, origin)
    return APP_ORIGINS[DEFAULT_ORIGIN]


def _page_link(origin, slug, type_id=""):
    return "%s%s?%s%s" % (origin, PUBLIC_PATH, slug, ("/" + type_id) if type_id else "")


def _manage_link(origin, token):
    return "%s%s?b=%s" % (origin, PUBLIC_PATH, token)


async def _manage_token(runtime, booking_id):
    return "%s.%s" % (booking_id, (await runtime.blind("booking-manage:" + booking_id))[:24])


async def _token_booking(runtime, token):
    match = TOKEN_RE.fullmatch(str(token or ""))
    if not match:
        return ""
    booking_id = match.group(1)
    if not hmac.compare_digest(await _manage_token(runtime, booking_id), token):
        return ""
    return booking_id


def _initials(name):
    words = [word for word in re.split(r"[\s._-]+", str(name or "")) if word]
    letters = "".join(word[0] for word in words[:2]).upper()
    return letters or "?"


def _format_clock(local, fmt):
    if fmt == "24h":
        return "%02d:%02d" % (local.hour, local.minute)
    hour = local.hour % 12 or 12
    return "%d:%02d %s" % (hour, local.minute, "AM" if local.hour < 12 else "PM")


def _when(start_ms, end_ms, zone_name, fmt="12h"):
    zone = _zone(zone_name)
    start = datetime.fromtimestamp(start_ms / 1000, zone)
    end = datetime.fromtimestamp(end_ms / 1000, zone)
    day = "%s, %d %s %d" % (WEEKDAY_NAMES[start.weekday()], start.day,
                            MONTH_NAMES[start.month - 1], start.year)
    hours = "%s – %s" % (_format_clock(start, fmt), _format_clock(end, fmt))
    label = str(zone_name or "UTC").replace("_", " ")
    return {"day": day, "time": hours, "zone": label, "label": "%s, %s (%s)" % (day, hours, label)}


def _local_ms(day, minutes, zone):
    extra, rest = divmod(minutes, 24 * 60)
    moment = datetime.combine(day + timedelta(days=extra), dtime(rest // 60, rest % 60), tzinfo=zone)
    return int(moment.timestamp() * 1000)


def _overlaps(busy, start, end):
    for first, last in busy:
        if first < end and last > start:
            return True
    return False


def _slots(settings, minutes, range_start, range_end, now, busy, day_counts=None):
    zone = _zone(settings.get("timezone"))
    step = _integer(settings.get("slotStepMinutes")) or minutes
    buffer_ms = _integer(settings.get("bufferMinutes")) * MINUTE_MS
    earliest = max(range_start, now + _integer(settings.get("minNoticeMinutes")) * MINUTE_MS)
    latest = min(range_end, now + _integer(settings.get("maxDaysAhead"), 60) * DAY_MS)
    if latest <= earliest:
        return []
    limit = _integer(settings.get("dailyLimit"))
    counts = day_counts or {}
    availability = settings.get("availability") or {}
    first_day = datetime.fromtimestamp(earliest / 1000, zone).date() - timedelta(days=1)
    last_day = datetime.fromtimestamp(latest / 1000, zone).date()
    result = []
    day = first_day
    while day <= last_day and len(result) < MAX_SLOTS:
        if limit and counts.get(day.isoformat(), 0) >= limit:
            day += timedelta(days=1)
            continue
        for start_clock, end_clock in availability.get(WEEKDAYS[day.weekday()]) or []:
            cursor = _minutes(start_clock)
            close = _minutes(end_clock)
            while cursor + minutes <= close and len(result) < MAX_SLOTS:
                start = _local_ms(day, cursor, zone)
                end = start + minutes * MINUTE_MS
                if (earliest <= start < latest
                        and not _overlaps(busy, start - buffer_ms, end + buffer_ms)):
                    result.append(start)
                cursor += step
        day += timedelta(days=1)
    return sorted(set(result))


def _add_months(day, months):
    total = day.year * 12 + day.month - 1 + months
    year, month = divmod(total, 12)
    try:
        return date(year, month + 1, day.day)
    except ValueError:
        return None


def _occurrences(start_ms, end_ms, recurrence, until, zone_name, low, high):
    duration = max(0, end_ms - start_ms)
    zone = _zone(zone_name)
    first = datetime.fromtimestamp(start_ms / 1000, zone)
    clock = first.time()
    low_day = datetime.fromtimestamp(max(start_ms, low - duration) / 1000, zone).date()
    result = []
    if recurrence in ("daily", "weekly"):
        period = 1 if recurrence == "daily" else 7
        index = max(0, (low_day - first.date()).days // period)
        step = lambda n: first.date() + timedelta(days=n * period)
    elif recurrence == "monthly":
        index = max(0, (low_day.year - first.year) * 12 + low_day.month - first.month - 1)
        step = lambda n: _add_months(first.date(), n)
    elif recurrence == "yearly":
        index = max(0, low_day.year - first.year - 1)
        step = lambda n: _add_months(first.date(), 12 * n)
    else:
        return [(start_ms, end_ms)] if start_ms < high and end_ms > low else []
    for _ in range(800):
        day = step(index)
        index += 1
        if day is None:
            continue
        begin = int(datetime.combine(day, clock, tzinfo=zone).timestamp() * 1000)
        if begin >= high or (until and begin > until):
            break
        if begin + duration > low:
            result.append((begin, begin + duration))
    return result


async def _calendar_busy(runtime, account_bi, low, high, skip_event=""):
    busy = []
    try:
        rows = await runtime.d1_all(
            "SELECT event_id,start_at,end_at,timezone,recurring,data FROM calendar_events "
            "WHERE all_day=0 AND (owner_bi=? OR event_id IN (SELECT event_id FROM "
            "calendar_attendees WHERE attendee_bi=? AND status IN ('accepted','tentative'))) "
            "AND ((recurring=0 AND start_at<? AND end_at>?) OR (recurring=1 AND start_at<?)) "
            "LIMIT 2000",
            account_bi, account_bi, high, low, high)
    except Exception:
        return busy
    for row in rows or []:
        if skip_event and str(row.get("event_id") or "") == skip_event:
            continue
        start, end = _integer(row.get("start_at")), _integer(row.get("end_at"))
        if not _integer(row.get("recurring")):
            busy.append((start, end))
            continue
        data = await runtime.open(row.get("data"))
        data = data if isinstance(data, dict) else {}
        busy.extend(_occurrences(start, end, str(data.get("recurrence") or "none"),
                                 _integer(data.get("recurrenceUntil")),
                                 str(row.get("timezone") or "UTC"), low, high))
    return busy


async def _busy(runtime, account_bi, settings, low, high, skip=None):
    skip = skip or {}
    pad = (_integer(settings.get("bufferMinutes")) + 240) * MINUTE_MS
    rows = await runtime.d1_all(
        "SELECT booking_id,start_at,end_at,calendar_event_id FROM booking_bookings "
        "WHERE account_bi=? AND status='confirmed' AND start_at<? AND end_at>? LIMIT 3000",
        account_bi, high + pad, low - pad)
    busy = [(_integer(row.get("start_at")), _integer(row.get("end_at")))
            for row in rows or [] if row.get("booking_id") != skip.get("booking_id")]
    if settings.get("checkCalendar"):
        busy.extend(await _calendar_busy(runtime, account_bi, low - pad, high + pad,
                                         skip.get("calendar_event_id", "")))
    return busy


async def _day_counts(runtime, account_bi, settings, low, high, skip_id=""):
    if not _integer(settings.get("dailyLimit")):
        return {}
    zone = _zone(settings.get("timezone"))
    rows = await runtime.d1_all(
        "SELECT booking_id,start_at FROM booking_bookings WHERE account_bi=? AND "
        "status='confirmed' AND start_at>=? AND start_at<? LIMIT 3000",
        account_bi, low - 2 * DAY_MS, high + 2 * DAY_MS)
    counts = {}
    for row in rows or []:
        if row.get("booking_id") == skip_id:
            continue
        day = datetime.fromtimestamp(_integer(row.get("start_at")) / 1000, zone).date().isoformat()
        counts[day] = counts.get(day, 0) + 1
    return counts


async def _available(runtime, account_bi, settings, minutes, start_at, skip=None):
    end_at = start_at + minutes * MINUTE_MS
    busy = await _busy(runtime, account_bi, settings, start_at - DAY_MS, end_at + DAY_MS, skip)
    counts = await _day_counts(runtime, account_bi, settings, start_at, end_at,
                               (skip or {}).get("booking_id", ""))
    return start_at in _slots(settings, minutes, start_at, start_at + 1, runtime.now(), busy, counts)


async def _page_row(runtime, account_bi):
    return await runtime.d1_first(
        "SELECT account_bi,account_name,slug,enabled,installed_at,updated_at,data "
        "FROM booking_pages WHERE account_bi=?", account_bi)


async def _slug_row(runtime, slug):
    return await runtime.d1_first(
        "SELECT account_bi,account_name,slug,enabled,installed_at,updated_at,data "
        "FROM booking_pages WHERE slug=?", slug)


async def _settings(runtime, row):
    stored = await runtime.open_for(row["account_bi"], row.get("data"))
    return _settings_data({}, stored, str(row.get("account_name") or ""))


def _stored(settings):
    return {key: settings[key] for key in STORED_KEYS}


async def _slug_problem(runtime, slug, account_bi, actor):
    if not SLUG_RE.fullmatch(slug or ""):
        return "invalid_slug"
    if slug in RESERVED_SLUGS and slug != actor:
        return "slug_reserved"
    owner = await _slug_row(runtime, slug)
    if owner and owner.get("account_bi") != account_bi:
        return "slug_taken"
    if slug != actor:
        lookup = getattr(runtime, "account", None)
        found = await lookup(slug) if lookup else ""
        found = found[0] if isinstance(found, (list, tuple)) else found
        if found and found != account_bi:
            return "slug_taken"
    return ""


async def _default_slug(runtime, account_bi, actor):
    base = actor if SLUG_RE.fullmatch(actor or "") else "meet"
    if not await _slug_problem(runtime, base, account_bi, actor):
        return base
    for _ in range(6):
        candidate = "%s-%s" % (base[:56], runtime.new_id()[:6])
        if not await _slug_problem(runtime, candidate, account_bi, actor):
            return candidate
    return "meet-" + runtime.new_id()[:12]


def _type(settings, type_id):
    for item in settings.get("types") or []:
        if item["id"] == type_id and item.get("enabled"):
            return item
    return None


def _public_type(item):
    return {
        "id": item["id"],
        "title": item["title"],
        "minutes": item["minutes"],
        "description": item["description"],
        "location": {"kind": item["location"]["kind"],
                     "label": LOCATION_LABELS.get(item["location"]["kind"], "")},
        "askPhone": item["location"]["kind"] == "phone" and not item["location"]["detail"],
    }


def _public_page(row, settings):
    return {
        "slug": row["slug"],
        "name": settings["displayName"],
        "initials": _initials(settings["displayName"]),
        "headline": settings["headline"],
        "welcome": settings["welcome"],
        "timezone": settings["timezone"],
        "timeFormat": settings["timeFormat"],
        "accent": settings["accent"],
        "types": [_public_type(item) for item in settings["types"] if item.get("enabled")],
    }


def _owner_settings(settings):
    return {key: settings[key] for key in STORED_KEYS}


def _location_text(location, guest_phone=""):
    kind, detail = location.get("kind"), location.get("detail") or ""
    if kind == "phone":
        return ("Phone call: " + detail) if detail else (
            ("Phone call to " + guest_phone) if guest_phone else "Phone call")
    if kind == "video":
        return ("Video call: " + detail) if detail else "Video call (link to follow)"
    if kind == "place":
        return detail or "In person"
    if kind == "custom":
        return detail
    return ""


async def _project_booking(runtime, row, data=None, origin=""):
    data = data if isinstance(data, dict) else await runtime.open_for(
        row["account_bi"], row.get("data"))
    result = {
        "id": str(row.get("booking_id") or ""),
        "status": str(row.get("status") or "confirmed"),
        "typeId": str(row.get("type_id") or ""),
        "typeTitle": str(data.get("typeTitle") or ""),
        "minutes": _integer(data.get("minutes")),
        "startAt": _integer(row.get("start_at")),
        "endAt": _integer(row.get("end_at")),
        "createdAt": _integer(row.get("created_at")),
        "cancelledAt": _integer(row.get("cancelled_at")),
        "cancelledBy": str(row.get("cancelled_by") or ""),
        "cancelReason": str(data.get("cancelReason") or ""),
        "guest": {
            "name": str(data.get("guestName") or ""),
            "email": str(data.get("guestEmail") or ""),
            "phone": str(data.get("guestPhone") or ""),
            "timezone": str(data.get("guestTimezone") or "UTC"),
            "notes": str(data.get("notes") or ""),
        },
        "location": data.get("location") if isinstance(data.get("location"), dict) else {},
        "calendarEventId": str(row.get("calendar_event_id") or ""),
        "rescheduledFrom": _integer(data.get("rescheduledFrom")),
    }
    if origin:
        result["manageUrl"] = _manage_link(origin, await _manage_token(runtime, result["id"]))
    return result


async def _bookings(runtime, account_bi, where, args, order, limit):
    rows = await runtime.d1_all(
        "SELECT " + BOOKING_COLUMNS + " FROM booking_bookings WHERE account_bi=? AND "
        + where + " ORDER BY " + order + " LIMIT ?", account_bi, *args, limit)
    return [await _project_booking(runtime, row) for row in rows or []]


async def _stats(runtime, account_bi, now):
    rows = await runtime.d1_all(
        "SELECT status,start_at,created_at,cancelled_at FROM booking_bookings WHERE "
        "account_bi=? AND (start_at>=? OR created_at>=?) LIMIT 5000",
        account_bi, now - 30 * DAY_MS, now - 30 * DAY_MS)
    stats = {"upcoming": 0, "next7Days": 0, "booked30Days": 0, "cancelled30Days": 0,
             "bookedMinutes7Days": 0}
    for row in rows or []:
        start, status = _integer(row.get("start_at")), row.get("status")
        if status == "confirmed" and start >= now:
            stats["upcoming"] += 1
            if start < now + 7 * DAY_MS:
                stats["next7Days"] += 1
        if _integer(row.get("created_at")) >= now - 30 * DAY_MS:
            stats["booked30Days"] += 1
        if status == "cancelled" and _integer(row.get("cancelled_at")) >= now - 30 * DAY_MS:
            stats["cancelled30Days"] += 1
    week = await runtime.d1_all(
        "SELECT start_at,end_at FROM booking_bookings WHERE account_bi=? AND status='confirmed' "
        "AND start_at>=? AND start_at<? LIMIT 1000", account_bi, now, now + 7 * DAY_MS)
    stats["bookedMinutes7Days"] = sum(
        (_integer(row.get("end_at")) - _integer(row.get("start_at"))) // MINUTE_MS
        for row in week or [])
    return stats


async def _overview(runtime, row, account_bi, actor):
    now = runtime.now()
    settings = await _settings(runtime, row)
    origin = _site_origin(runtime)
    upcoming = await _bookings(runtime, account_bi, "status='confirmed' AND end_at>=?", [now],
                               "start_at", MAX_LISTED)
    past = await _bookings(runtime, account_bi, "(status='cancelled' OR end_at<?) AND start_at>=?",
                           [now, now - 90 * DAY_MS], "start_at DESC", 100)
    return _response(runtime, {
        "ok": True,
        "installed": True,
        "actor": actor,
        "slug": row["slug"],
        "enabled": bool(_integer(row.get("enabled"))),
        "link": _page_link(origin, row["slug"]),
        "links": {item["id"]: _page_link(origin, row["slug"], item["id"])
                  for item in settings["types"]},
        "origin": origin,
        "settings": _owner_settings(settings),
        "options": {
            "durations": list(DURATIONS), "steps": list(STEPS), "buffers": list(BUFFERS),
            "notices": list(NOTICES), "horizons": list(HORIZONS), "locations": list(LOCATIONS),
            "accents": ACCENT_HEX, "timeFormats": list(TIME_FORMATS),
        },
        "upcoming": upcoming,
        "past": past,
        "stats": await _stats(runtime, account_bi, now),
    })


async def _install(runtime, account_bi, actor, email, data):
    existing = await _page_row(runtime, account_bi)
    if existing:
        return await _overview(runtime, existing, account_bi, actor)
    requested = _clean(data.get("slug"), 63).lower()
    slug = requested if requested and not await _slug_problem(
        runtime, requested, account_bi, actor) else await _default_slug(runtime, account_bi, actor)
    settings = _settings_data(data, None, actor, email)
    now = runtime.now()
    await runtime.d1_run(
        "INSERT INTO booking_pages(account_bi,account_name,slug,enabled,installed_at,updated_at,data) "
        "VALUES(?,?,?,?,?,?,?) ON CONFLICT(account_bi) DO NOTHING",
        account_bi, actor, slug, 1, now, now,
        await runtime.seal_for(account_bi, _stored(settings)))
    await runtime.audit(actor, "plugin.install", "account_plugin", actor + ":" + PLUGIN_ID)
    return await _overview(runtime, await _page_row(runtime, account_bi), account_bi, actor)


async def _save_page(runtime, row, account_bi, actor, data):
    current = await _settings(runtime, row)
    settings = _settings_data(data, current, actor)
    if "types" in data and not settings["types"]:
        return _response(runtime, {"error": "meeting_type_required"}, 400)
    for key in ("replyTo", "notifyEmail"):
        if _clean(data.get(key), 254) and not settings[key]:
            return _response(runtime, {"error": "invalid_email", "field": key}, 400)
    for supplied in data.get("types") if isinstance(data.get("types"), list) else []:
        location = supplied.get("location") if isinstance(supplied, dict) else None
        if (isinstance(location, dict) and location.get("kind") == "video"
                and _clean(location.get("detail"), 1000) and not _https(location.get("detail"))):
            return _response(runtime, {"error": "invalid_meeting_url",
                                       "type": _clean(supplied.get("title"), 80)}, 400)
    slug = row["slug"]
    if "slug" in data:
        slug = _clean(data.get("slug"), 63).lower()
        if slug != row["slug"]:
            problem = await _slug_problem(runtime, slug, account_bi, actor)
            if problem:
                return _response(runtime, {"error": problem}, 409 if problem == "slug_taken" else 400)
    enabled = row.get("enabled")
    if "enabled" in data:
        enabled = 1 if data.get("enabled") else 0
    try:
        await runtime.d1_run(
            "UPDATE booking_pages SET account_name=?,slug=?,enabled=?,updated_at=?,data=? "
            "WHERE account_bi=?",
            actor, slug, _integer(enabled), runtime.now(),
            await runtime.seal_for(account_bi, _stored(settings)), account_bi)
    except Exception as error:
        if "unique" in str(error).lower():
            return _response(runtime, {"error": "slug_taken"}, 409)
        raise
    await runtime.audit(actor, "booking.page.save", "booking_page", slug)
    return await _overview(runtime, await _page_row(runtime, account_bi), account_bi, actor)


def quote(value):
    return _quote(str(value or ""), safe="")


def _calendar_links(title, details, location, start_ms, end_ms):
    stamp = lambda ms: datetime.fromtimestamp(ms / 1000, timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    iso = lambda ms: datetime.fromtimestamp(ms / 1000, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    google = ("https://calendar.google.com/calendar/render?action=TEMPLATE&text=%s&dates=%s/%s"
              "&details=%s&location=%s") % (
        quote(title), stamp(start_ms), stamp(end_ms), quote(details[:1500]), quote(location))
    outlook = ("https://outlook.live.com/calendar/0/deeplink/compose?path=%%2Fcalendar%%2Faction"
               "%%2Fcompose&rru=addevent&subject=%s&startdt=%s&enddt=%s&body=%s&location=%s") % (
        quote(title), quote(iso(start_ms)), quote(iso(end_ms)), quote(details[:1500]),
        quote(location))
    return {"google": google, "outlook": outlook}


def _paragraphs(text):
    return "".join(
        '<p style="margin:0 0 12px">%s</p>' % escape(block.strip()).replace("\n", "<br>")
        for block in re.split(r"\n\s*\n", str(text or "").strip()) if block.strip())


def _email_html(accent, eyebrow, heading, intro, rows, buttons, footer, tone=""):
    sans = "-apple-system,BlinkMacSystemFont,'Segoe UI',Helvetica,Arial,sans-serif"
    color = "#dc2626" if tone == "cancelled" else ACCENT_HEX.get(accent, ACCENT_HEX["blue"])
    detail_rows = "".join(
        '<tr><td style="padding:10px 0;border-top:1px solid #ececf1;color:#6b6b76;font-size:13px;'
        'width:96px;vertical-align:top">%s</td><td style="padding:10px 0;border-top:1px solid '
        '#ececf1;font-size:15px;color:#16161d;vertical-align:top">%s</td></tr>' % (
            escape(label), value_html)
        for label, value_html in rows if value_html)
    button_html = "".join(
        '<a href="%s" style="display:inline-block;margin:0 8px 8px 0;padding:11px 18px;'
        'border-radius:10px;font-weight:600;font-size:14px;text-decoration:none;%s">%s</a>' % (
            escape(url, quote=True),
            ("background:%s;color:#ffffff" % color) if primary
            else "background:#f1f1f5;color:#16161d", escape(label))
        for label, url, primary in buttons if url)
    return (
        '<!doctype html><html><body style="margin:0;background:#f4f4f7">'
        '<table role="presentation" width="100%%" cellpadding="0" cellspacing="0" '
        'style="background:#f4f4f7;padding:32px 12px"><tr><td align="center">'
        '<table role="presentation" width="100%%" cellpadding="0" cellspacing="0" '
        'style="max-width:560px;background:#ffffff;border-radius:16px;overflow:hidden;'
        'font:400 15px/1.6 %(sans)s;color:#16161d">'
        '<tr><td style="height:6px;background:%(color)s"></td></tr>'
        '<tr><td style="padding:28px 32px 8px">'
        '<p style="margin:0 0 6px;font-size:12px;font-weight:700;letter-spacing:.08em;'
        'text-transform:uppercase;color:%(color)s">%(eyebrow)s</p>'
        '<h1 style="margin:0 0 14px;font:700 24px/1.25 %(sans)s;color:#16161d">%(heading)s</h1>'
        '%(intro)s</td></tr>'
        '<tr><td style="padding:4px 32px 8px"><table role="presentation" width="100%%" '
        'cellpadding="0" cellspacing="0">%(rows)s</table></td></tr>'
        '<tr><td style="padding:16px 32px 20px">%(buttons)s</td></tr>'
        '<tr><td style="padding:16px 32px 28px;border-top:1px solid #ececf1;font-size:12px;'
        'color:#8a8a96">%(footer)s</td></tr>'
        '</table></td></tr></table></body></html>'
    ) % {
        "sans": sans, "color": color, "eyebrow": escape(eyebrow), "heading": escape(heading),
        "intro": _paragraphs(intro), "rows": detail_rows, "buttons": button_html,
        "footer": escape(footer),
    }


def _email_text(heading, intro, rows, buttons, footer):
    lines = [heading, ""]
    if intro:
        lines += [intro, ""]
    lines += ["%s: %s" % (label, value) for label, value in rows if value]
    lines.append("")
    lines += ["%s: %s" % (label, url) for label, url, _primary in buttons if url]
    lines += ["", footer]
    return "\n".join(lines).strip()


def _link_html(url, label=""):
    return '<a href="%s" style="color:#2563eb">%s</a>' % (escape(url, quote=True), escape(label or url))


async def _mail_allowed(runtime, recipient, variables, recipient_limit):
    now = runtime.now()
    if not await _spend(runtime, "mail-booking:%s:%s" % (
            variables["forkmesh_booking_mail"].rsplit(":", 1)[-1], variables["forkmesh_booking_id"]),
            DAY_MS, MAIL_PER_BOOKING_DAILY, now):
        return False
    return await _spend(runtime, "mail-to:" + recipient.lower(), DAY_MS, recipient_limit, now)


async def _send(runtime, account_bi, recipient, reply_to, subject, message, variables,
                recipient_limit=MAIL_PER_GUEST_DAILY):
    sender = getattr(runtime, "send_mailtrap", None)
    if sender is None or not recipient:
        return {"accepted": False, "error": "mail_unavailable"}
    if not await _mail_allowed(runtime, recipient, variables, recipient_limit):
        return {"accepted": False, "error": "mail_limited"}
    try:
        result = await sender("", {
            "from": {"email": reply_to or "", "name": ""},
            "to": [{"email": recipient}],
            "subject": subject,
            "text": message["text"],
            "html": message["html"],
            "category": "Booking",
            "tracked": True,
            "organization": account_bi,
            "account": await runtime.blind("booking-mail:" + account_bi),
            "custom_variables": dict(variables, forkmesh_package=PLUGIN_ID),
        })
    except Exception as error:
        return {"accepted": False, "error": type(error).__name__}
    return result if isinstance(result, dict) else {"accepted": False, "error": "send_failed"}


def _message(accent, eyebrow, heading, intro, rows, buttons, footer, tone=""):
    html_rows = [(label, html if html is not None else escape(text).replace("\n", "<br>"))
                 for label, text, html in rows if text]
    text_rows = [(label, text) for label, text, _html in rows if text]
    return {
        "html": _email_html(accent, eyebrow, heading, intro, html_rows, buttons, footer, tone),
        "text": _email_text(heading, intro, text_rows, buttons, footer),
    }


def _detail_rows(booking, settings, zone_name, fmt, for_owner):
    when = _when(booking["startAt"], booking["endAt"], zone_name, fmt)
    location = booking["location"] or {}
    where = _location_text(location, booking["guest"]["phone"])
    where_html = None
    if location.get("kind") == "video" and location.get("detail"):
        where_html = "Video call: " + _link_html(location["detail"])
    rows = [
        ("What", "%s (%d min)" % (booking["typeTitle"], booking["minutes"]), None),
        ("When", "%s\n%s" % (when["day"], when["time"]), None),
        ("Time zone", when["zone"], None),
        ("Where", where, where_html),
    ]
    if for_owner:
        guest = booking["guest"]
        rows.insert(0, ("Guest", guest["name"], None))
        rows.insert(1, ("Email", guest["email"], _link_html("mailto:" + guest["email"], guest["email"])))
        if guest["phone"]:
            rows.insert(2, ("Phone", guest["phone"], None))
        if guest["timezone"] and guest["timezone"] != zone_name:
            guest_when = _when(booking["startAt"], booking["endAt"], guest["timezone"], fmt)
            rows.append(("Their time", guest_when["label"], None))
    else:
        rows.insert(0, ("With", settings["displayName"], None))
    if for_owner and booking["guest"]["notes"]:
        rows.append(("Notes", booking["guest"]["notes"], None))
    return rows


def _calendar_title(booking, settings, for_owner):
    if for_owner:
        return "%s with %s" % (booking["typeTitle"], booking["guest"]["name"])
    return "%s with %s" % (booking["typeTitle"], settings["displayName"])


def _greeting_name(name):
    first = str(name or "").split(" ")[0]
    return first if re.fullmatch(r"[^\W\d_][\w'-]{0,39}", first) else "there"


async def _email_booking(runtime, account_bi, settings, booking, kind, origin, owner_name):
    accent = settings["accent"]
    fmt = settings["timeFormat"]
    guest = booking["guest"]
    token = await _manage_token(runtime, booking["id"])
    manage = _manage_link(origin, token)
    page = _page_link(origin, settings.get("_slug") or "", booking["typeId"])
    footer = "Sent by ForkMesh on behalf of %s." % settings["displayName"]
    guest_when = _when(booking["startAt"], booking["endAt"], guest["timezone"], fmt)
    owner_when = _when(booking["startAt"], booking["endAt"], settings["timezone"], fmt)
    location = _location_text(booking["location"] or {}, guest["phone"])
    reason = booking.get("cancelReason") or ""
    results = {}
    guest_title = _calendar_title(booking, settings, False)
    owner_title = _calendar_title(booking, settings, True)
    links = _calendar_links(guest_title, "Booked with ForkMesh. Manage: " + manage, location,
                            booking["startAt"], booking["endAt"])
    if kind in ("confirmed", "rescheduled"):
        heading = "You're booked with %s" % settings["displayName"] if kind == "confirmed" else (
            "Your meeting with %s has moved" % settings["displayName"])
        intro = "Hi %s, your %s is %s for %s." % (
            _greeting_name(guest["name"]), booking["typeTitle"],
            "confirmed" if kind == "confirmed" else "now", guest_when["label"])
        buttons = [("Add to Google Calendar", links["google"], True),
                   ("Add to Outlook", links["outlook"], False),
                   ("Reschedule or cancel", manage, False)]
        subject = "%s: %s with %s on %s" % (
            "Confirmed" if kind == "confirmed" else "Rescheduled", booking["typeTitle"],
            settings["displayName"], guest_when["day"])
        eyebrow = "Booking confirmed" if kind == "confirmed" else "Booking rescheduled"
        tone = ""
    elif kind == "reminder":
        heading = "Coming up: %s with %s" % (booking["typeTitle"], settings["displayName"])
        intro = "A reminder that you are meeting %s on %s." % (settings["displayName"], guest_when["label"])
        buttons = [("Reschedule or cancel", manage, False)]
        subject = "Reminder: %s with %s on %s" % (booking["typeTitle"], settings["displayName"],
                                                  guest_when["day"])
        eyebrow = "Reminder"
        tone = ""
    else:
        by_owner = booking.get("cancelledBy") == "owner"
        heading = "Your meeting with %s was cancelled" % settings["displayName"]
        intro = ("%s cancelled this meeting." % settings["displayName"]) if by_owner else (
            "You cancelled this meeting.")
        if reason and by_owner:
            intro += "\n\n“%s”" % reason
        buttons = [("Book a new time", page, True)] if settings.get("_slug") else []
        subject = "Cancelled: %s with %s on %s" % (booking["typeTitle"], settings["displayName"],
                                                   guest_when["day"])
        eyebrow = "Booking cancelled"
        tone = "cancelled"
    rows = _detail_rows(booking, settings, guest["timezone"], fmt, False)
    results["guest"] = await _send(
        runtime, account_bi, guest["email"], settings.get("replyTo") or "", subject,
        _message(accent, eyebrow, heading, intro, rows, buttons, footer, tone),
        {"forkmesh_booking_id": booking["id"], "forkmesh_booking_mail": kind + ":guest"})
    if kind == "reminder" or not settings.get("emailOwner") or not settings.get("notifyEmail"):
        return results
    owner_rows = _detail_rows(booking, settings, settings["timezone"], fmt, True)
    owner_buttons = [("Open your bookings", _app_origin(runtime) + PAGE_PATH + "?booking="
                      + booking["id"], True)]
    owner_footer = "Your ForkMesh booking page sent this to you. Reply to write to %s." % guest["name"]
    if kind == "confirmed":
        owner_subject = "New booking: %s, %s" % (guest["name"], owner_when["label"])
        owner_heading = "%s booked %s" % (guest["name"], booking["typeTitle"])
        owner_intro = "It is on your ForkMesh calendar." if booking["calendarEventId"] else ""
        owner_eyebrow, owner_tone = "New booking", ""
    elif kind == "rescheduled":
        owner_subject = "Rescheduled: %s, now %s" % (guest["name"], owner_when["label"])
        owner_heading = "%s moved their meeting" % guest["name"]
        owner_intro = ""
        owner_eyebrow, owner_tone = "Booking rescheduled", ""
    else:
        owner_subject = "Cancelled: %s, %s" % (guest["name"], owner_when["label"])
        owner_heading = "%s's meeting was cancelled" % guest["name"]
        owner_intro = ("You cancelled it." if booking.get("cancelledBy") == "owner"
                       else "%s cancelled it." % guest["name"])
        if reason:
            owner_intro += "\n\n“%s”" % reason
        owner_eyebrow, owner_tone = "Booking cancelled", "cancelled"
    results["owner"] = await _send(
        runtime, account_bi, settings["notifyEmail"], guest["email"], owner_subject,
        _message(accent, owner_eyebrow, owner_heading, owner_intro, owner_rows, owner_buttons,
                 owner_footer, owner_tone),
        {"forkmesh_booking_id": booking["id"], "forkmesh_booking_mail": kind + ":owner",
         "forkmesh_sender": owner_name}, MAIL_PER_OWNER_ADDRESS_DAILY)
    return results


async def _notify_owner(runtime, owner, title, body, booking_id, dedupe):
    notify = getattr(runtime, "notify", None)
    if notify is None or not owner:
        return
    try:
        await notify(owner, "plugin_activity", title, body=body,
                     href="%s?booking=%s" % (PAGE_PATH, booking_id), dedupe=dedupe)
    except Exception:
        return


async def _calendar_add(runtime, row, settings, booking):
    if not settings.get("addToCalendar"):
        return ""
    event_id = runtime.new_id()
    now = runtime.now()
    guest = booking["guest"]
    location = booking["location"] or {}
    description = "\n".join(line for line in (
        "Booked through your ForkMesh booking page.",
        "Guest: %s <%s>" % (guest["name"], guest["email"]),
        ("Phone: " + guest["phone"]) if guest["phone"] else "",
        ("Notes: " + guest["notes"]) if guest["notes"] else "",
    ) if line)
    data = {
        "title": _calendar_title(booking, settings, True)[:200],
        "description": description[:10000],
        "location": _location_text(location, guest["phone"])[:500] if location.get("kind") != "video" else "",
        "meetingUrl": (location.get("detail") or "")[:1000] if location.get("kind") == "video" else "",
        "color": "",
        "reminders": [60, 10],
        "recurrence": "none",
        "recurrenceUntil": 0,
        "bookingId": booking["id"],
    }
    try:
        await runtime.d1_run(
            "INSERT INTO calendar_events(event_id,owner_bi,owner_name,org_bi,org_name,"
            "start_at,end_at,all_day,timezone,recurring,created_at,updated_at,data) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
            event_id, row["account_bi"], row["account_name"], "", "", booking["startAt"],
            booking["endAt"], 0, settings["timezone"], 0, now, now, await runtime.seal(data))
    except Exception:
        return ""
    return event_id


async def _calendar_move(runtime, event_id, start_at, end_at):
    if not event_id:
        return
    try:
        await runtime.d1_run(
            "UPDATE calendar_events SET start_at=?,end_at=?,updated_at=? WHERE event_id=?",
            start_at, end_at, runtime.now(), event_id)
    except Exception:
        return


async def _calendar_remove(runtime, account_bi, event_id):
    if not event_id:
        return
    try:
        await runtime.d1_run("DELETE FROM calendar_attendees WHERE event_id=?", event_id)
    except Exception:
        pass
    try:
        await runtime.d1_run("DELETE FROM calendar_events WHERE event_id=? AND owner_bi=?",
                             event_id, account_bi)
    except Exception:
        return


def _source_key(address):
    try:
        parsed = ipaddress.ip_address(address)
    except ValueError:
        return address
    if parsed.version == 4:
        return address
    return str(parsed.ipv4_mapped or ipaddress.ip_network(address + "/64", strict=False))


def _visitor_key(runtime):
    reader = getattr(runtime, "request_visitor", None)
    visitor = reader() if reader else {}
    return _source_key(str((visitor or {}).get("ip") or "").strip())


async def _spend(runtime, bucket, window_ms, limit, now):
    key = await runtime.blind("booking-rate:%s:%d" % (bucket, now // window_ms))
    row = await runtime.d1_first("SELECT count FROM booking_rate WHERE bucket=?", key)
    if row and _integer(row.get("count")) >= limit:
        return False
    await runtime.d1_run(
        "INSERT INTO booking_rate(bucket,count,expires_at) VALUES(?,1,?) "
        "ON CONFLICT(bucket) DO UPDATE SET count=count+1",
        key, (now // window_ms + 1) * window_ms)
    return True


def _guest_input(data, item):
    name = _clean(data.get("name"), 120)
    email = _email(data.get("email"))
    notes = _text(data.get("notes"), 2000)
    phone = _clean(data.get("phone"), 32)
    guest_zone = _timezone_name(data.get("timezone"))
    if _clean(data.get("website"), 200):
        return None, "invalid_booking"
    if not name:
        return None, "name_required"
    if not email:
        return None, "invalid_email"
    if phone and not PHONE_RE.fullmatch(phone):
        return None, "invalid_phone"
    if item["location"]["kind"] == "phone" and not item["location"]["detail"] and not phone:
        return None, "phone_required"
    return {"name": name, "email": email, "notes": notes, "phone": phone,
            "timezone": guest_zone}, ""


async def _book(runtime, row, settings, item, data):
    now = runtime.now()
    guest, error = _guest_input(data, item)
    if error:
        return _response(runtime, {"error": error}, 400)
    start_at = _integer(data.get("startAt"))
    if start_at <= 0:
        return _response(runtime, {"error": "invalid_time"}, 400)
    account_bi = row["account_bi"]
    guest_bi = await runtime.blind("booking-guest:" + guest["email"])
    open_row = await runtime.d1_first(
        "SELECT COUNT(*) AS n FROM booking_bookings WHERE account_bi=? AND guest_bi=? AND "
        "status='confirmed' AND start_at>?", account_bi, guest_bi, now)
    if _integer((open_row or {}).get("n")) >= MAX_UPCOMING_PER_GUEST:
        return _response(runtime, {"error": "too_many_bookings"}, 429)
    if not await _available(runtime, account_bi, settings, item["minutes"], start_at):
        return _response(runtime, {"error": "slot_unavailable"}, 409)
    visitor = _visitor_key(runtime)
    if visitor:
        if not await _spend(runtime, "visitor-hour:" + visitor, HOUR_MS, VISITOR_HOURLY_BOOKINGS, now):
            return _response(runtime, {"error": "rate_limited"}, 429)
        if not await _spend(runtime, "visitor-day:" + visitor, DAY_MS, VISITOR_DAILY_BOOKINGS, now):
            return _response(runtime, {"error": "rate_limited"}, 429)
    if not await _spend(runtime, "page-day:" + account_bi, DAY_MS, PAGE_DAILY_BOOKINGS, now):
        return _response(runtime, {"error": "page_busy"}, 429)
    booking_id = runtime.new_id()
    end_at = start_at + item["minutes"] * MINUTE_MS
    stored = {
        "typeTitle": item["title"], "minutes": item["minutes"], "location": item["location"],
        "guestName": guest["name"], "guestEmail": guest["email"], "guestPhone": guest["phone"],
        "guestTimezone": guest["timezone"], "notes": guest["notes"],
    }
    try:
        await runtime.d1_run(
            "INSERT INTO booking_bookings(booking_id,account_bi,type_id,status,start_at,end_at,"
            "guest_bi,calendar_event_id,reminded_at,cancelled_at,cancelled_by,created_at,"
            "updated_at,data) VALUES(?,?,?,'confirmed',?,?,?,'',0,0,'',?,?,?)",
            booking_id, account_bi, item["id"], start_at, end_at, guest_bi, now, now,
            await runtime.seal_for(account_bi, stored))
    except Exception as failure:
        if "unique" in str(failure).lower() or "constraint" in str(failure).lower():
            return _response(runtime, {"error": "slot_unavailable"}, 409)
        raise
    booking = await _project_booking(runtime, await _booking_row(runtime, booking_id), stored)
    event_id = await _calendar_add(runtime, row, settings, booking)
    if event_id:
        await runtime.d1_run("UPDATE booking_bookings SET calendar_event_id=? WHERE booking_id=?",
                             event_id, booking_id)
        booking["calendarEventId"] = event_id
    origin = _site_origin(runtime)
    settings = dict(settings, _slug=row["slug"])
    mail = await _email_booking(runtime, account_bi, settings, booking, "confirmed", origin,
                                row["account_name"])
    when = _when(start_at, end_at, settings["timezone"], settings["timeFormat"])
    await _notify_owner(runtime, row["account_name"],
                        "📅 %s booked %s" % (guest["name"], item["title"]),
                        when["label"], booking_id, "booking-new:" + booking_id)
    await runtime.audit("guest", "booking.create", "booking", booking_id)
    token = await _manage_token(runtime, booking_id)
    return _response(runtime, {
        "ok": True,
        "booking": await _guest_booking(runtime, booking, row, settings, token),
        "emailed": bool((mail.get("guest") or {}).get("accepted")),
    }, 201)


async def _guest_booking(runtime, booking, row, settings, token):
    location = booking["location"] or {}
    item = _type(settings, booking["typeId"])
    return {
        "token": token,
        "status": booking["status"],
        "typeId": booking["typeId"],
        "typeTitle": booking["typeTitle"],
        "minutes": booking["minutes"],
        "startAt": booking["startAt"],
        "endAt": booking["endAt"],
        "guestName": booking["guest"]["name"],
        "guestTimezone": booking["guest"]["timezone"],
        "location": {"kind": location.get("kind") or "none",
                     "label": LOCATION_LABELS.get(location.get("kind") or "none", ""),
                     "text": _location_text(location, booking["guest"]["phone"]),
                     "url": location.get("detail") if location.get("kind") == "video" else ""},
        "cancelledBy": booking["cancelledBy"],
        "cancelReason": booking["cancelReason"],
        "canChange": booking["status"] == "confirmed" and booking["startAt"] > runtime.now(),
        "canReschedule": bool(item) and bool(_integer(row.get("enabled"))),
        "title": _calendar_title(booking, settings, False),
        "page": _public_page(row, settings),
    }


async def _booking_row(runtime, booking_id):
    return await runtime.d1_first(
        "SELECT " + BOOKING_COLUMNS + " FROM booking_bookings WHERE booking_id=?", booking_id)


async def _cancel(runtime, row, page, settings, by, reason, notify_guest=True):
    now = runtime.now()
    data = await runtime.open_for(row["account_bi"], row.get("data"))
    data["cancelReason"] = _text(reason, 600)
    await runtime.d1_run(
        "UPDATE booking_bookings SET status='cancelled',cancelled_at=?,cancelled_by=?,"
        "updated_at=?,data=? WHERE booking_id=? AND status='confirmed'",
        now, by, now, await runtime.seal_for(row["account_bi"], data), row["booking_id"])
    await _calendar_remove(runtime, row["account_bi"], str(row.get("calendar_event_id") or ""))
    booking = await _project_booking(runtime, await _booking_row(runtime, row["booking_id"]))
    origin = _site_origin(runtime)
    settings = dict(settings, _slug=page["slug"] if page else "")
    if notify_guest or by == "guest":
        email_settings = settings if by == "guest" else dict(settings, emailOwner=False)
        await _email_booking(runtime, row["account_bi"], email_settings, booking, "cancelled",
                             origin, page["account_name"] if page else "")
    if by == "guest" and page:
        when = _when(booking["startAt"], booking["endAt"], settings["timezone"], settings["timeFormat"])
        await _notify_owner(runtime, page["account_name"],
                            "🚫 %s cancelled %s" % (booking["guest"]["name"], booking["typeTitle"]),
                            when["label"], booking["id"], "booking-cancel:" + booking["id"])
    return booking


async def _reschedule(runtime, row, page, settings, data):
    item = _type(settings, row["type_id"])
    if not item or not _integer(page.get("enabled")):
        return _response(runtime, {"error": "meeting_type_unavailable"}, 409)
    start_at = _integer(data.get("startAt"))
    if start_at <= 0:
        return _response(runtime, {"error": "invalid_time"}, 400)
    now = runtime.now()
    visitor = _visitor_key(runtime)
    if visitor and not await _spend(runtime, "visitor-change:" + visitor, HOUR_MS,
                                    VISITOR_HOURLY_CHANGES, now):
        return _response(runtime, {"error": "rate_limited"}, 429)
    skip = {"booking_id": row["booking_id"],
            "calendar_event_id": str(row.get("calendar_event_id") or "")}
    if not await _available(runtime, row["account_bi"], settings, item["minutes"], start_at, skip):
        return _response(runtime, {"error": "slot_unavailable"}, 409)
    stored = await runtime.open_for(row["account_bi"], row.get("data"))
    stored["rescheduledFrom"] = _integer(row.get("start_at"))
    stored["minutes"] = item["minutes"]
    stored["typeTitle"] = item["title"]
    end_at = start_at + item["minutes"] * MINUTE_MS
    try:
        await runtime.d1_run(
            "UPDATE booking_bookings SET start_at=?,end_at=?,reminded_at=0,updated_at=?,data=? "
            "WHERE booking_id=? AND status='confirmed'",
            start_at, end_at, now, await runtime.seal_for(row["account_bi"], stored),
            row["booking_id"])
    except Exception as failure:
        if "unique" in str(failure).lower() or "constraint" in str(failure).lower():
            return _response(runtime, {"error": "slot_unavailable"}, 409)
        raise
    await _calendar_move(runtime, str(row.get("calendar_event_id") or ""), start_at, end_at)
    booking = await _project_booking(runtime, await _booking_row(runtime, row["booking_id"]))
    origin = _site_origin(runtime)
    settings = dict(settings, _slug=page["slug"])
    await _email_booking(runtime, row["account_bi"], settings, booking, "rescheduled", origin,
                         page["account_name"])
    when = _when(start_at, end_at, settings["timezone"], settings["timeFormat"])
    await _notify_owner(runtime, page["account_name"],
                        "🔁 %s moved %s" % (booking["guest"]["name"], booking["typeTitle"]),
                        "Now " + when["label"], booking["id"],
                        "booking-move:%s:%d" % (booking["id"], start_at))
    return _response(runtime, {
        "ok": True,
        "booking": await _guest_booking(runtime, booking, page, settings,
                                        await _manage_token(runtime, booking["id"])),
    })


def _range(runtime, now):
    params = runtime.query_params() if hasattr(runtime, "query_params") else {}
    start = _integer(params.get("start"), now)
    end = _integer(params.get("end"), start + 35 * DAY_MS)
    start = max(start, now - DAY_MS)
    if end <= start:
        end = start + DAY_MS
    return start, min(end, start + MAX_SLOT_RANGE_MS)


async def _slot_list(runtime, row, settings, item, skip=None):
    now = runtime.now()
    start, end = _range(runtime, now)
    busy = await _busy(runtime, row["account_bi"], settings, start, end, skip)
    counts = await _day_counts(runtime, row["account_bi"], settings, start, end,
                               (skip or {}).get("booking_id", ""))
    slots = _slots(settings, item["minutes"], start, end, now, busy, counts)
    return _response(runtime, {
        "ok": True,
        "type": item["id"],
        "minutes": item["minutes"],
        "timezone": settings["timezone"],
        "start": start,
        "end": end,
        "bookableUntil": now + _integer(settings.get("maxDaysAhead"), 60) * DAY_MS,
        "slots": slots,
    })


async def _public_page_route(runtime, route, method, data):
    _, slug, type_id, action = route
    row = await _slug_row(runtime, slug)
    if not row:
        return _response(runtime, {"error": "page_not_found"}, 404)
    if not _integer(row.get("enabled")):
        return _response(runtime, {"error": "page_paused"}, 404)
    settings = await _settings(runtime, row)
    if not type_id:
        return _response(runtime, {"ok": True, "page": _public_page(row, settings)})
    item = _type(settings, type_id)
    if not item:
        return _response(runtime, {"error": "meeting_type_not_found"}, 404)
    if action == "slots":
        return await _slot_list(runtime, row, settings, item)
    return await _book(runtime, row, settings, item, data)


async def _manage_route(runtime, route, method, data):
    _, token, action = route
    booking_id = await _token_booking(runtime, token)
    row = await _booking_row(runtime, booking_id) if booking_id else None
    page = await _page_row(runtime, row["account_bi"]) if row else None
    if not row or not page:
        return _response(runtime, {"error": "booking_not_found"}, 404)
    settings = await _settings(runtime, page)
    if action == "slots":
        item = _type(settings, row["type_id"])
        if not item:
            return _response(runtime, {"error": "meeting_type_unavailable"}, 409)
        return await _slot_list(runtime, page, settings, item, {
            "booking_id": row["booking_id"],
            "calendar_event_id": str(row.get("calendar_event_id") or "")})
    changeable = row.get("status") == "confirmed" and _integer(row.get("start_at")) > runtime.now()
    if action == "cancel":
        if not changeable:
            return _response(runtime, {"error": "booking_locked"}, 409)
        booking = await _cancel(runtime, row, page, settings, "guest", data.get("reason"))
        return _response(runtime, {"ok": True, "booking": await _guest_booking(
            runtime, booking, page, settings, token)})
    if action == "reschedule":
        if not changeable:
            return _response(runtime, {"error": "booking_locked"}, 409)
        return await _reschedule(runtime, row, page, settings, data)
    booking = await _project_booking(runtime, row)
    return _response(runtime, {"ok": True, "booking": await _guest_booking(
        runtime, booking, page, settings, token)})


async def account_has_data(runtime, account_bi):
    row = await runtime.d1_first(
        "SELECT account_bi FROM booking_pages WHERE account_bi=?", account_bi)
    if row:
        return True
    return bool(await runtime.d1_first(
        "SELECT booking_id FROM booking_bookings WHERE account_bi=? LIMIT 1", account_bi))


async def export_account_data(runtime, account_bi, account_name=""):
    runtime = _vault(runtime)
    await _ensure_tables(runtime)
    row = await _page_row(runtime, account_bi)
    rows = await runtime.d1_all(
        "SELECT " + BOOKING_COLUMNS + " FROM booking_bookings WHERE account_bi=? "
        "ORDER BY start_at LIMIT 20000", account_bi)
    return {
        "plugin": PLUGIN_ID,
        "format": "forkmesh-booking-export-v1",
        "account": account_name,
        "exportedAt": runtime.now(),
        "slug": row["slug"] if row else "",
        "settings": _owner_settings(await _settings(runtime, row)) if row else {},
        "bookings": [await _project_booking(runtime, item) for item in rows or []],
    }


async def purge_account_data(runtime, account_bi, account_name=""):
    for table in ("booking_bookings", "booking_pages", "booking_keys"):
        await runtime.d1_run("DELETE FROM " + table + " WHERE account_bi=?", account_bi)
    if isinstance(runtime, _Vault):
        runtime.forget(account_bi)


async def _uninstall(runtime, account_bi, actor):
    had_data = await account_has_data(runtime, account_bi)
    export = await export_account_data(runtime, account_bi, actor) if had_data else None
    await purge_account_data(runtime, account_bi, actor)
    await runtime.audit(actor, "plugin.uninstall", "account_plugin", actor + ":" + PLUGIN_ID)
    return _response(runtime, {
        "ok": True, "uninstalled": True, "plugin": PLUGIN_ID, "hadData": had_data,
        "filename": "forkmesh-booking-%s.json" % actor, "export": export,
    })


async def deliver_reminders(runtime):
    runtime = _vault(runtime)
    await _ensure_tables(runtime)
    now = runtime.now()
    await runtime.d1_run("DELETE FROM booking_rate WHERE expires_at<?", now)
    rows = await runtime.d1_all(
        "SELECT " + BOOKING_COLUMNS + " FROM booking_bookings WHERE status='confirmed' "
        "AND reminded_at=0 AND start_at>? AND start_at<=? ORDER BY start_at LIMIT ?",
        now, now + REMINDER_LEAD_MS, REMINDER_BATCH)
    delivered = 0
    for row in rows or []:
        await runtime.d1_run(
            "UPDATE booking_bookings SET reminded_at=? WHERE booking_id=? AND reminded_at=0",
            now, row["booking_id"])
        if _integer(row.get("updated_at")) > _integer(row.get("start_at")) - REMINDER_LEAD_MS:
            continue
        page = await _page_row(runtime, row["account_bi"])
        if not page:
            continue
        settings = dict(await _settings(runtime, page), _slug=page["slug"])
        booking = await _project_booking(runtime, row)
        await _email_booking(runtime, row["account_bi"], settings, booking, "reminder",
                             _site_origin(runtime), page["account_name"])
        delivered += 1
    return {"delivered": delivered}


async def handle(runtime, path):
    runtime = _vault(runtime)
    route = _route(path)
    if route is None:
        return _response(runtime, {"error": "not_found"}, 404)
    method = runtime.method()
    allowed = _allowed(route)
    if not allowed:
        return _response(runtime, {"error": "not_found"}, 404)
    if method == "HEAD" and "GET" in allowed:
        method = "GET"
    if method not in allowed:
        return _response(runtime, {"error": "method_not_allowed"}, 405, ", ".join(allowed))
    public = route[0] in ("page", "manage")
    if method != "GET" and not public and not runtime.same_origin():
        return _response(runtime, {"error": "origin_not_allowed"}, 403)
    data = {}
    if method != "GET":
        data, error = await runtime.json_body(MAX_BODY_BYTES)
        if error:
            return _response(runtime, {"error": error}, 413 if error == "payload_too_large" else 400)
    await runtime.ensure_schema()
    await _ensure_tables(runtime)
    if route[0] == "page":
        return await _public_page_route(runtime, route, method, data)
    if route[0] == "manage":
        return await _manage_route(runtime, route, method, data)
    account_bi, actor, email = await _actor(runtime, data)
    if not account_bi or not actor:
        return _response(runtime, {"error": "invalid_session"}, 401)
    parts = route[1:]
    head = parts[0] if parts else ""
    if head == "install":
        return await _install(runtime, account_bi, actor, email, data)
    row = await _page_row(runtime, account_bi)
    if not row:
        if not parts:
            return _response(runtime, {"ok": True, "installed": False, "actor": actor,
                                       "suggestedSlug": await _default_slug(runtime, account_bi, actor),
                                       "origin": _site_origin(runtime)})
        return _response(runtime, {"error": "plugin_not_installed", "canInstall": True}, 409)
    if not parts:
        return await _overview(runtime, row, account_bi, actor)
    if head == "uninstall":
        return await _uninstall(runtime, account_bi, actor)
    if head == "export":
        return _response(runtime, {
            "ok": True, "filename": "forkmesh-booking-%s.json" % actor,
            "export": await export_account_data(runtime, account_bi, actor)})
    if head == "page":
        if method == "GET":
            return _response(runtime, {"ok": True, "settings": _owner_settings(
                await _settings(runtime, row))})
        return await _save_page(runtime, row, account_bi, actor, data)
    if head == "slug":
        slug = _clean(parts[1], 63).lower()
        problem = "" if slug == row["slug"] else await _slug_problem(runtime, slug, account_bi, actor)
        return _response(runtime, {"ok": True, "slug": slug, "available": not problem,
                                   "reason": problem})
    if head == "bookings" and len(parts) == 1:
        now = runtime.now()
        return _response(runtime, {"ok": True, "upcoming": await _bookings(
            runtime, account_bi, "status='confirmed' AND end_at>=?", [now], "start_at", MAX_LISTED)})
    booking_id = parts[1]
    if not ID_RE.fullmatch(booking_id):
        return _response(runtime, {"error": "booking_not_found"}, 404)
    booking = await _booking_row(runtime, booking_id)
    if not booking or booking.get("account_bi") != account_bi:
        return _response(runtime, {"error": "booking_not_found"}, 404)
    if booking.get("status") != "confirmed":
        return _response(runtime, {"error": "booking_locked"}, 409)
    settings = await _settings(runtime, row)
    await _cancel(runtime, booking, row, settings, "owner", data.get("reason"),
                  data.get("notifyGuest") is not False)
    await runtime.audit(actor, "booking.cancel", "booking", booking_id)
    return await _overview(runtime, await _page_row(runtime, account_bi), account_bi, actor)
