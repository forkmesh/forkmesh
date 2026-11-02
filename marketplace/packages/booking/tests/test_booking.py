import asyncio
from datetime import datetime
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
from zoneinfo import ZoneInfo


PACKAGE = Path(__file__).resolve().parents[1]
REPO = PACKAGE.parents[2]
sys.path.insert(0, str(PACKAGE / "content" / "worker"))
import booking  # noqa: E402


ACCOUNTS = {"alice": "alice@example.com", "bob": "bob@example.com"}
LONDON = ZoneInfo("Europe/London")
CALENDAR_SCHEMA = [
    """CREATE TABLE calendar_events (
        event_id TEXT PRIMARY KEY, owner_bi TEXT NOT NULL, owner_name TEXT NOT NULL,
        org_bi TEXT NOT NULL DEFAULT '', org_name TEXT NOT NULL DEFAULT '',
        start_at INTEGER NOT NULL, end_at INTEGER NOT NULL, all_day INTEGER NOT NULL DEFAULT 0,
        timezone TEXT NOT NULL DEFAULT 'UTC', recurring INTEGER NOT NULL DEFAULT 0,
        created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, data TEXT NOT NULL)""",
    """CREATE TABLE calendar_attendees (
        event_id TEXT NOT NULL, attendee_bi TEXT NOT NULL, attendee_name TEXT NOT NULL,
        status TEXT NOT NULL, created_at INTEGER NOT NULL, responded_at INTEGER NOT NULL)""",
]


def ms(year, month, day, hour=0, minute=0, zone=LONDON):
    return int(datetime(year, month, day, hour, minute, tzinfo=zone).timestamp() * 1000)


class Runtime:
    def __init__(self, installed=("alice",), clock=None):
        booking._SCHEMA_READY = False
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in CALENDAR_SCHEMA:
            self.db.execute(statement)
        self.actor = "alice"
        self.method_value = "GET"
        self.body = {}
        self.query = {}
        self.clock = clock or ms(2026, 11, 2, 8, 0)
        self.ids = 0
        self.secrets = 0
        self.audits = []
        self.mail = []
        self.pings = []
        self.ip = "203.0.113.9"
        self.mail_ok = True
        for name in installed:
            response = call(self.use("POST", actor=name, body={"timezone": "Europe/London"}),
                            "/api/booking/install")
            assert response["status"] == 200, response
        self.use("GET")

    def use(self, method, actor="alice", body=None, query=None):
        self.method_value, self.actor, self.body = method, actor, body or {}
        self.query = query or {}
        return self

    def method(self): return self.method_value
    def query_params(self): return dict(self.query)
    def same_origin(self): return True
    def request_origin(self): return "https://app.forkmesh.com"
    def request_visitor(self): return {"ip": self.ip, "agent": "test"}
    def now(self): return self.clock
    def new_id(self): self.ids += 1; return f"{self.ids:032x}"
    def response(self, payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": payload, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data=None):
        if self.actor not in ACCOUNTS:
            return "", None
        return "bi:" + self.actor, {"name": self.actor, "email": ACCOUNTS[self.actor],
                                    "email_verified": True}
    async def account(self, name):
        return ("bi:" + name, name) if name in ACCOUNTS or name == "carol" else ("", "")
    async def seal(self, value): return "sealed:" + json.dumps(value, sort_keys=True)
    async def open(self, value):
        text = str(value or "")
        return json.loads(text.removeprefix("sealed:")) if text.startswith("sealed:") else None
    def new_secret(self):
        self.secrets += 1
        return "key-%032x" % self.secrets
    async def seal_with(self, secret, value):
        return "sealed[%s]:%s" % (secret, json.dumps(value, sort_keys=True))
    async def open_with(self, secret, value):
        prefix = "sealed[%s]:" % secret
        text = str(value or "")
        return json.loads(text.removeprefix(prefix)) if text.startswith(prefix) else None
    async def blind(self, value):
        return hashlib.sha256(str(value).encode()).hexdigest()
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args); self.db.commit(); return cursor
    async def audit(self, *args, **kwargs): self.audits.append(args)
    async def send_mailtrap(self, token, payload):
        self.mail.append(payload)
        return {"accepted": self.mail_ok, "messageId": "m%d" % len(self.mail),
                "error": "" if self.mail_ok else "smtp_unavailable"}
    async def notify(self, recipient, kind, title, body="", href="", dedupe=""):
        self.pings.append({"to": recipient, "kind": kind, "title": title, "body": body,
                           "href": href, "dedupe": dedupe})
        return True


def call(runtime, path):
    return asyncio.run(booking.handle(runtime, path))


def owner(runtime, actor="alice"):
    return call(runtime.use("GET", actor=actor), "/api/booking")["data"]


def slots(runtime, slug="alice", type_id="30min", start=None, end=None):
    query = {}
    if start is not None:
        query = {"start": start, "end": end}
    response = call(runtime.use("GET", actor="", query=query), f"/api/booking/-/p/{slug}/{type_id}/slots")
    assert response["status"] == 200, response
    return response["data"]["slots"]


def book(runtime, start, slug="alice", type_id="30min", **extra):
    body = {"name": "Grace Hopper", "email": "grace@example.org", "timezone": "America/New_York",
            "notes": "Compilers", "startAt": start}
    body.update(extra)
    return call(runtime.use("POST", actor="", body=body), f"/api/booking/-/p/{slug}/{type_id}/book")


def test_install_creates_a_page_named_after_the_account():
    runtime = Runtime(installed=())
    data = owner(runtime)
    assert data["installed"] is False and data["suggestedSlug"] == "alice"
    call(runtime.use("POST", body={"timezone": "Europe/London"}), "/api/booking/install")
    data = owner(runtime)
    assert data["installed"] is True and data["slug"] == "alice"
    assert data["link"] == "https://forkmesh.com/meet?alice"
    assert data["links"]["30min"] == "https://forkmesh.com/meet?alice/30min"
    settings = data["settings"]
    assert settings["timezone"] == "Europe/London"
    assert settings["notifyEmail"] == "alice@example.com" and settings["replyTo"] == "alice@example.com"
    assert [item["id"] for item in settings["types"]] == ["15min", "30min"]
    assert settings["availability"]["mon"] == [["09:00", "17:00"]] and settings["availability"]["sat"] == []
    stored = runtime.db.execute("SELECT data FROM booking_pages").fetchone()[0]
    assert stored.startswith("sealed[key-") and "alice@example.com" not in stored.split("]:")[0]


def test_public_page_hides_owner_details():
    runtime = Runtime()
    call(runtime.use("PUT", body={"types": [{"id": "intro", "title": "Intro", "minutes": 30,
                                             "location": {"kind": "video",
                                                          "detail": "https://meet.example/secret"}}]}),
         "/api/booking/page")
    page = call(runtime.use("GET", actor=""), "/api/booking/-/p/alice")["data"]["page"]
    assert page["name"] == "alice" and page["initials"] == "A"
    assert page["types"] == [{"id": "intro", "title": "Intro", "minutes": 30, "description": "",
                              "location": {"kind": "video", "label": "Video call"}, "askPhone": False}]
    text = json.dumps(page)
    assert "secret" not in text and "alice@example.com" not in text
    assert call(runtime.use("GET", actor=""), "/api/booking/-/p/nobody")["status"] == 404


def test_slots_follow_availability_notice_and_time_zone():
    runtime = Runtime()
    found = slots(runtime, start=ms(2026, 11, 2), end=ms(2026, 11, 4))
    monday = [s for s in found if s < ms(2026, 11, 3)]
    assert monday[0] == ms(2026, 11, 2, 12, 0)
    assert monday[-1] == ms(2026, 11, 2, 16, 30)
    assert len(monday) == 10
    tuesday = [s for s in found if ms(2026, 11, 3) <= s < ms(2026, 11, 4)]
    assert tuesday[0] == ms(2026, 11, 3, 9, 0) and len(tuesday) == 16
    assert not slots(runtime, start=ms(2026, 11, 7), end=ms(2026, 11, 9))


def test_slots_skip_calendar_events_and_existing_bookings():
    runtime = Runtime()
    runtime.db.execute(
        "INSERT INTO calendar_events VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        ("e1", "bi:alice", "alice", "", "", ms(2026, 11, 3, 10, 0), ms(2026, 11, 3, 11, 0), 0,
         "Europe/London", 0, 0, 0, "sealed:{}"))
    runtime.db.execute(
        "INSERT INTO calendar_events VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        ("e2", "bi:alice", "alice", "", "", ms(2026, 10, 6, 14, 0), ms(2026, 10, 6, 15, 0), 0,
         "Europe/London", 1, 0, 0, 'sealed:{"recurrence": "weekly", "recurrenceUntil": 0}'))
    runtime.db.execute(
        "INSERT INTO calendar_events VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)",
        ("e3", "bi:alice", "alice", "", "", ms(2026, 11, 3, 0, 0), ms(2026, 11, 4, 0, 0), 1,
         "Europe/London", 0, 0, 0, "sealed:{}"))
    found = set(slots(runtime, start=ms(2026, 11, 3), end=ms(2026, 11, 4)))
    assert ms(2026, 11, 3, 10, 0) not in found and ms(2026, 11, 3, 10, 30) not in found
    assert ms(2026, 11, 3, 9, 30) in found and ms(2026, 11, 3, 11, 0) in found
    assert ms(2026, 11, 3, 14, 0) not in found and ms(2026, 11, 3, 14, 30) not in found
    assert book(runtime, ms(2026, 11, 3, 12, 0))["status"] == 201
    found = set(slots(runtime, start=ms(2026, 11, 3), end=ms(2026, 11, 4)))
    assert ms(2026, 11, 3, 12, 0) not in found
    call(runtime.use("PUT", body={"checkCalendar": False, "bufferMinutes": 30}), "/api/booking/page")
    found = set(slots(runtime, start=ms(2026, 11, 3), end=ms(2026, 11, 4)))
    assert ms(2026, 11, 3, 10, 0) in found
    assert ms(2026, 11, 3, 11, 30) not in found and ms(2026, 11, 3, 12, 30) not in found
    assert ms(2026, 11, 3, 11, 0) in found and ms(2026, 11, 3, 13, 0) in found


def test_booking_emails_both_sides_and_lands_on_the_calendar():
    runtime = Runtime()
    start = ms(2026, 11, 3, 15, 0)
    response = book(runtime, start)
    assert response["status"] == 201, response
    result = response["data"]["booking"]
    assert result["status"] == "confirmed" and result["canChange"] is True
    assert result["title"] == "Meeting with alice" and response["data"]["emailed"] is True
    guest_mail, owner_mail = runtime.mail
    assert guest_mail["to"] == [{"email": "grace@example.org"}]
    assert guest_mail["from"]["email"] == "alice@example.com"
    assert guest_mail["organization"] == "bi:alice"
    assert guest_mail["subject"] == "Confirmed: Meeting with alice on Tuesday, 3 November 2026"
    assert "10:00 AM – 10:30 AM" in guest_mail["text"] and "America/New York" in guest_mail["text"]
    assert "https://forkmesh.com/meet?b=" + result["token"] in guest_mail["text"]
    assert "calendar.google.com" in guest_mail["html"]
    assert owner_mail["to"] == [{"email": "alice@example.com"}]
    assert owner_mail["from"]["email"] == "grace@example.org"
    assert owner_mail["subject"].startswith("New booking: Grace Hopper, Tuesday, 3 November 2026, 3:00 PM")
    assert "https://app.forkmesh.com/dashboard/booking?booking=" in owner_mail["text"]
    assert "Compilers" in owner_mail["text"]
    event = runtime.db.execute("SELECT * FROM calendar_events WHERE event_id NOT IN ('e1')").fetchone()
    assert event["owner_bi"] == "bi:alice" and event["start_at"] == start
    assert json.loads(event["data"].removeprefix("sealed:"))["title"] == "Meeting with Grace Hopper"
    assert runtime.pings[0]["to"] == "alice" and runtime.pings[0]["kind"] == "plugin_activity"
    data = owner(runtime)
    assert data["upcoming"][0]["guest"]["email"] == "grace@example.org"
    assert data["stats"]["upcoming"] == 1 and data["stats"]["bookedMinutes7Days"] == 30
    stored = runtime.db.execute("SELECT data FROM booking_bookings").fetchone()[0]
    assert stored.startswith("sealed[key-")


def test_a_slot_cannot_be_booked_twice_or_outside_hours():
    runtime = Runtime()
    start = ms(2026, 11, 3, 15, 0)
    assert book(runtime, start)["status"] == 201
    assert book(runtime, start, email="other@example.org")["data"]["error"] == "slot_unavailable"
    assert book(runtime, start + 15 * 60000, email="o2@example.org")["status"] == 409
    assert book(runtime, ms(2026, 11, 3, 20, 0))["status"] == 409
    assert book(runtime, ms(2026, 11, 2, 9, 0))["status"] == 409
    assert book(runtime, ms(2026, 11, 3, 9, 0), email="nope")["data"]["error"] == "invalid_email"
    assert book(runtime, ms(2026, 11, 3, 9, 0), name="")["data"]["error"] == "name_required"
    assert book(runtime, ms(2026, 11, 3, 9, 0), website="spam")["status"] == 400
    assert book(runtime, ms(2026, 11, 3, 9, 0), type_id="nope")["status"] == 404


def test_abuse_limits_per_guest_and_visitor():
    runtime = Runtime()
    for hour in (9, 10, 11):
        assert book(runtime, ms(2026, 11, 3, hour, 0))["status"] == 201
    assert book(runtime, ms(2026, 11, 3, 12, 0))["data"]["error"] == "too_many_bookings"
    for index, hour in enumerate((12, 13, 14)):
        assert book(runtime, ms(2026, 11, 3, hour, 0), email=f"g{index}@example.org")["status"] == 201
    assert book(runtime, ms(2026, 11, 3, 15, 0), email="late@example.org")["data"]["error"] == "rate_limited"
    runtime.ip = "198.51.100.4"
    assert book(runtime, ms(2026, 11, 3, 15, 0), email="late@example.org")["status"] == 201


def test_guest_reschedules_and_cancels_with_the_private_link():
    runtime = Runtime()
    token = book(runtime, ms(2026, 11, 3, 15, 0))["data"]["booking"]["token"]
    assert call(runtime.use("GET", actor=""), "/api/booking/-/b/" + token[:-1] + "0")["status"] == 404
    view = call(runtime.use("GET", actor=""), "/api/booking/-/b/" + token)["data"]["booking"]
    assert view["guestName"] == "Grace Hopper" and "email" not in json.dumps(view["location"])
    free = call(runtime.use("GET", actor="", query={"start": ms(2026, 11, 3), "end": ms(2026, 11, 4)}),
                "/api/booking/-/b/" + token + "/slots")["data"]["slots"]
    assert ms(2026, 11, 3, 15, 0) in free
    runtime.mail.clear()
    moved = call(runtime.use("POST", actor="", body={"startAt": ms(2026, 11, 4, 11, 0)}),
                 "/api/booking/-/b/" + token + "/reschedule")
    assert moved["status"] == 200 and moved["data"]["booking"]["startAt"] == ms(2026, 11, 4, 11, 0)
    assert runtime.mail[0]["subject"].startswith("Rescheduled: Meeting with alice on Wednesday")
    event = runtime.db.execute("SELECT start_at FROM calendar_events").fetchone()
    assert event["start_at"] == ms(2026, 11, 4, 11, 0)
    runtime.mail.clear()
    cancelled = call(runtime.use("POST", actor="", body={"reason": "Clash"}),
                     "/api/booking/-/b/" + token + "/cancel")
    assert cancelled["data"]["booking"]["status"] == "cancelled"
    assert cancelled["data"]["booking"]["canChange"] is False
    assert [mail["subject"].split(":")[0] for mail in runtime.mail] == ["Cancelled", "Cancelled"]
    assert "Clash" in runtime.mail[1]["text"]
    assert runtime.db.execute("SELECT COUNT(*) FROM calendar_events").fetchone()[0] == 0
    assert call(runtime.use("POST", actor="", body={}), "/api/booking/-/b/" + token + "/cancel")["status"] == 409
    assert ms(2026, 11, 4, 11, 0) in slots(runtime, start=ms(2026, 11, 4), end=ms(2026, 11, 5))


def test_owner_cancel_tells_only_the_guest():
    runtime = Runtime()
    book(runtime, ms(2026, 11, 3, 15, 0))
    booking_id = owner(runtime)["upcoming"][0]["id"]
    runtime.mail.clear()
    assert call(runtime.use("POST", actor="bob", body={}), f"/api/booking/bookings/{booking_id}/cancel")["status"] == 409
    response = call(runtime.use("POST", body={"reason": "Sick"}), f"/api/booking/bookings/{booking_id}/cancel")
    assert response["status"] == 200 and response["data"]["upcoming"] == []
    assert len(runtime.mail) == 1 and runtime.mail[0]["to"] == [{"email": "grace@example.org"}]
    assert "alice cancelled this meeting" in runtime.mail[0]["text"]
    assert response["data"]["past"][0]["cancelledBy"] == "owner"


def test_slugs_are_unique_and_cannot_take_another_account_name():
    runtime = Runtime(installed=("alice", "bob"))
    assert call(runtime.use("PUT", actor="bob", body={"slug": "alice"}), "/api/booking/page")["data"]["error"] == "slug_taken"
    assert call(runtime.use("PUT", actor="bob", body={"slug": "carol"}), "/api/booking/page")["data"]["error"] == "slug_taken"
    assert call(runtime.use("PUT", actor="bob", body={"slug": "Bad Slug"}), "/api/booking/page")["data"]["error"] == "invalid_slug"
    assert call(runtime.use("PUT", actor="bob", body={"slug": "admin"}), "/api/booking/page")["data"]["error"] == "slug_reserved"
    saved = call(runtime.use("PUT", actor="bob", body={"slug": "bob-chats"}), "/api/booking/page")["data"]
    assert saved["slug"] == "bob-chats" and saved["link"].endswith("/meet?bob-chats")
    assert call(runtime.use("GET", actor="alice"), "/api/booking/slug/bob-chats")["data"]["available"] is False
    assert call(runtime.use("GET", actor="alice"), "/api/booking/slug/fresh")["data"]["available"] is True


def test_settings_are_cleaned():
    runtime = Runtime()
    saved = call(runtime.use("PUT", body={
        "timezone": "Not/AZone", "accent": "neon", "minNoticeMinutes": 7, "dailyLimit": 999,
        "availability": {"mon": [["10:00", "12:00"], ["11:00", "13:00"], ["17:00", "09:00"]],
                         "tue": [{"start": "08:00", "end": "24:00"}]},
        "types": [{"title": "Deep dive", "minutes": 61}, {"title": "Deep dive", "minutes": 45},
                  {"title": ""}],
    }), "/api/booking/page")["data"]["settings"]
    assert saved["timezone"] == "Europe/London" and saved["accent"] == "blue"
    assert saved["minNoticeMinutes"] == 240 and saved["dailyLimit"] == 50
    assert saved["availability"]["mon"] == [["10:00", "13:00"]]
    assert saved["availability"]["tue"] == [["08:00", "24:00"]]
    assert [(item["id"], item["minutes"]) for item in saved["types"]] == [("deep-dive", 30), ("deep-dive-2", 45)]
    assert call(runtime.use("PUT", body={"types": []}), "/api/booking/page")["data"]["error"] == "meeting_type_required"
    bad = call(runtime.use("PUT", body={"types": [{"title": "X", "location": {"kind": "video", "detail": "javascript:alert(1)"}}]}),
               "/api/booking/page")
    assert bad["data"]["error"] == "invalid_meeting_url"
    assert call(runtime.use("PUT", body={"notifyEmail": "not-an-email"}), "/api/booking/page")["data"]["error"] == "invalid_email"


def test_daily_limit_and_paused_page():
    runtime = Runtime()
    call(runtime.use("PUT", body={"dailyLimit": 1}), "/api/booking/page")
    assert book(runtime, ms(2026, 11, 3, 9, 0))["status"] == 201
    assert slots(runtime, start=ms(2026, 11, 3), end=ms(2026, 11, 4)) == []
    call(runtime.use("PUT", body={"enabled": False}), "/api/booking/page")
    assert call(runtime.use("GET", actor=""), "/api/booking/-/p/alice")["data"]["error"] == "page_paused"


def test_phone_meetings_ask_for_the_guest_number():
    runtime = Runtime()
    call(runtime.use("PUT", body={"types": [{"id": "call", "title": "Call", "minutes": 15,
                                             "location": {"kind": "phone"}}]}), "/api/booking/page")
    assert book(runtime, ms(2026, 11, 3, 9, 0), type_id="call")["data"]["error"] == "phone_required"
    response = book(runtime, ms(2026, 11, 3, 9, 0), type_id="call", phone="+1 212 555 0100")
    assert response["status"] == 201
    assert response["data"]["booking"]["location"]["text"] == "Phone call to +1 212 555 0100"


def test_reminder_emails_once_a_day_before():
    runtime = Runtime()
    book(runtime, ms(2026, 11, 4, 10, 0))
    book(runtime, ms(2026, 11, 2, 15, 0), email="soon@example.org")
    runtime.mail.clear()
    runtime.clock = ms(2026, 11, 3, 10, 30)
    result = asyncio.run(booking.deliver_reminders(runtime))
    assert result == {"delivered": 1}
    assert runtime.mail[0]["subject"].startswith("Reminder: Meeting with alice on Wednesday")
    assert asyncio.run(booking.deliver_reminders(runtime)) == {"delivered": 0}


def test_export_uninstall_and_purge():
    runtime = Runtime()
    book(runtime, ms(2026, 11, 3, 9, 0))
    exported = call(runtime.use("GET"), "/api/booking/export")["data"]["export"]
    assert exported["format"] == "forkmesh-booking-export-v1" and len(exported["bookings"]) == 1
    removed = call(runtime.use("POST"), "/api/booking/uninstall")["data"]
    assert removed["uninstalled"] is True and removed["export"]["slug"] == "alice"
    for table in ("booking_pages", "booking_bookings", "booking_keys"):
        assert runtime.db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0] == 0
    assert call(runtime.use("GET", actor=""), "/api/booking/-/p/alice")["status"] == 404


def test_routes_methods_and_session():
    runtime = Runtime()
    assert call(runtime.use("GET", actor=""), "/api/booking")["status"] == 401
    assert call(runtime.use("DELETE"), "/api/booking/page")["status"] == 405
    assert call(runtime.use("GET"), "/api/booking/nothing")["status"] == 404
    assert call(runtime.use("GET", actor=""), "/api/booking/-/p/alice/30min/book")["status"] == 405


def test_recurring_occurrences_cover_daily_weekly_monthly():
    start, end = ms(2026, 1, 31, 9, 0), ms(2026, 1, 31, 10, 0)
    monthly = booking._occurrences(start, end, "monthly", 0, "Europe/London",
                                   ms(2026, 2, 1), ms(2026, 6, 1))
    assert [datetime.fromtimestamp(a / 1000, LONDON).strftime("%m-%d %H") for a, _ in monthly] == [
        "03-31 09", "05-31 09"]
    daily = booking._occurrences(start, end, "daily", ms(2026, 2, 2, 23, 0), "Europe/London",
                                 ms(2026, 2, 1), ms(2026, 2, 10))
    assert len(daily) == 2


def test_manifest_matches_the_worker():
    manifest = json.loads((PACKAGE / "manifest.json").read_text())
    assert manifest["worker"] == {"module": "booking", "api": booking.PREFIX, "outboundOrigins": []}
    assert manifest["dataLifecycle"]["scope"] == "account"
    routes = {item["socket"]: item["config"]["route"] for item in manifest["contributions"]}
    assert routes == {"dashboard.page": booking.PAGE_PATH, "www.page": booking.PUBLIC_PATH}
    catalog = json.loads((REPO / "marketplace" / "catalog.json").read_text())
    entry = next(item for item in catalog["packages"] if item["id"] == "booking")
    assert entry["version"] == manifest["version"]


def test_foreign_origin_never_reaches_the_emailed_links():
    runtime = Runtime()
    runtime.request_origin = lambda: "https://evil.example"
    result = book(runtime, ms(2026, 11, 3, 15, 0))["data"]["booking"]
    guest_mail, owner_mail = runtime.mail
    assert "evil.example" not in guest_mail["html"] + guest_mail["text"]
    assert "evil.example" not in owner_mail["html"] + owner_mail["text"]
    assert "https://forkmesh.com/meet?b=" + result["token"] in guest_mail["text"]
    assert "https://app.forkmesh.com/dashboard/booking?booking=" in owner_mail["text"]
    runtime.public_origin = lambda: "https://book.example"
    runtime.mail.clear()
    book(runtime, ms(2026, 11, 3, 16, 0), email="ada@example.org")
    assert "https://book.example/meet?b=" in runtime.mail[0]["text"]


def test_guest_mail_carries_no_anonymous_text():
    runtime = Runtime()
    token = book(runtime, ms(2026, 11, 3, 15, 0), notes="Visit https://evil.example/login",
                 name="https://evil.example/login Grace")["data"]["booking"]["token"]
    guest_mail, owner_mail = runtime.mail
    assert "evil.example" not in guest_mail["html"] + guest_mail["text"]
    assert "Hi there," in guest_mail["text"]
    assert "Visit https://evil.example/login" in owner_mail["text"]
    runtime.mail.clear()
    call(runtime.use("POST", actor="", body={"reason": "Pay at https://evil.example"}),
         "/api/booking/-/b/" + token + "/cancel")
    guest_mail, owner_mail = runtime.mail
    assert "Pay at" not in guest_mail["html"] + guest_mail["text"]
    assert "Pay at" in owner_mail["text"]


def test_owner_reason_still_reaches_the_guest():
    runtime = Runtime()
    book(runtime, ms(2026, 11, 3, 15, 0))
    runtime.mail.clear()
    booking_id = owner(runtime)["upcoming"][0]["id"]
    call(runtime.use("POST", body={"reason": "I am ill"}), f"/api/booking/bookings/{booking_id}/cancel")
    assert "I am ill" in runtime.mail[0]["text"]


def test_mail_is_capped_per_booking_and_per_recipient():
    runtime = Runtime()
    token = book(runtime, ms(2026, 11, 3, 9, 0))["data"]["booking"]["token"]
    runtime.mail.clear()
    moved = 0
    for hour in range(10, 16):
        for minute in (0, 30):
            runtime.ip = "198.51.100.%d" % moved
            answer = call(runtime.use("POST", actor="", body={"startAt": ms(2026, 11, 4, hour, minute)}),
                          "/api/booking/-/b/" + token + "/reschedule")
            assert answer["status"] == 200, answer
            moved += 1
    sent = [mail["to"][0]["email"] for mail in runtime.mail]
    assert moved == 12
    assert sent.count("grace@example.org") == booking.MAIL_PER_BOOKING_DAILY - 1
    assert sent.count("alice@example.com") == booking.MAIL_PER_BOOKING_DAILY - 1

    other = Runtime()
    other.mail.clear()
    hits = 0
    for index in range(booking.MAIL_PER_GUEST_DAILY + 2):
        other.db.execute("DELETE FROM booking_bookings")
        other.ip = "192.0.2.%d" % index
        answer = book(other, ms(2026, 11, 3 + index % 3, 9 + index % 8, 0))
        assert answer["status"] == 201, answer
        hits += answer["data"]["emailed"]
    assert hits == booking.MAIL_PER_GUEST_DAILY


def test_anonymous_mail_does_not_spend_the_owner_allowance():
    runtime = Runtime()
    book(runtime, ms(2026, 11, 3, 15, 0))
    account = runtime.mail[0]["account"]
    assert account != "bi:alice" and len(account) == 64
    assert runtime.mail[0]["organization"] == "bi:alice"


def test_ipv6_visitors_share_one_budget_per_64():
    runtime = Runtime()
    runtime.db.execute("DELETE FROM booking_rate")
    for index, hour in enumerate((9, 10, 11, 12, 13, 14)):
        runtime.ip = "2001:db8:1:2::%x" % (index + 1)
        assert book(runtime, ms(2026, 11, 3, hour, 0), email=f"g{index}@example.org")["status"] == 201
    runtime.ip = "2001:db8:1:2::ff"
    assert book(runtime, ms(2026, 11, 3, 15, 0), email="late@example.org")["data"]["error"] == "rate_limited"
    runtime.ip = "2001:db8:1:3::1"
    assert book(runtime, ms(2026, 11, 3, 15, 0), email="late@example.org")["status"] == 201
