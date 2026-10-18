"""Calendar authorization, invitation, RSVP, and reminder contracts."""

import asyncio
import json
import sqlite3
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import calendar_api
import dashboard_shell


ROOT = Path(__file__).resolve().parents[1]


class Runtime:
    def __init__(self, db, actor="alice", method="GET", body=None,
                 url="https://forkmesh.test/api/calendar", now=1_800_000_000_000):
        self.db = db
        self.actor = actor
        self._method = method
        self.body = body or {}
        self.url = url
        self.clock = now
        self.notifications = []
        self.audits = []
        self.accounts = {name: "bi:" + name for name in ("alice", "bob", "charlie")}

    def method(self):
        return self._method

    def now(self):
        return self.clock

    def new_id(self):
        return "a" * 32 if not self.d1_first_sync(
            "SELECT 1 AS one FROM calendar_events WHERE event_id=?", "a" * 32
        ) else "b" * 32

    def query(self, name):
        return (parse_qs(urlparse(self.url).query).get(name) or [""])[0]

    async def json(self):
        return self.body

    async def session(self, _data=None):
        return self.accounts.get(self.actor, ""), (
            {"name": self.actor} if self.actor in self.accounts else None)

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    def d1_first_sync(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_first(self, sql, *args):
        return self.d1_first_sync(sql, *args)

    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return cursor

    @staticmethod
    def changes(cursor):
        return cursor.rowcount

    async def seal(self, value):
        return "sealed:" + json.dumps(value, sort_keys=True)

    async def open(self, value):
        return json.loads(str(value).removeprefix("sealed:"))

    async def account(self, name):
        return self.accounts.get(name, "")

    async def org(self, name, actor):
        role = await self.org_role(name, actor)
        return role, "org:" + name if role else ""

    async def org_role(self, name, actor):
        row = self.d1_first_sync(
            "SELECT role FROM org_members WHERE org_bi=? AND member_bi=?",
            "org:" + name, "bi:" + actor)
        return str((row or {}).get("role") or "")

    async def notify(self, recipient, kind, title, **kwargs):
        self.notifications.append((recipient, kind, title, kwargs))
        return True

    async def audit(self, *values):
        self.audits.append(values)

    @staticmethod
    def response(payload, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "payload": payload,
                "cache": cache_control, "headers": extra_headers or {}}


def database():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript((ROOT / "migrations" / "0130_calendars.sql").read_text())
    db.executescript("""
      CREATE TABLE orgs (org_bi TEXT PRIMARY KEY, name TEXT, data TEXT, created_at INTEGER);
      CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT, name TEXT,
                                created_at INTEGER, PRIMARY KEY(org_bi, member_bi));
      INSERT INTO orgs VALUES ('org:acme','acme','sealed:{}',1);
      INSERT INTO org_members VALUES ('org:acme','bi:alice','owner','alice',1);
      INSERT INTO org_members VALUES ('org:acme','bi:charlie','member','charlie',1);
    """)
    return db


def run(awaitable):
    return asyncio.run(awaitable)


def event_body(**changes):
    body = {
        "title": "Planning",
        "startAt": 1_800_086_400_000,
        "endAt": 1_800_090_000_000,
        "timezone": "America/New_York",
        "scope": "personal",
        "calendar": "personal",
        "attendees": ["bob"],
        "reminders": [1440, 60, 10],
    }
    body.update(changes)
    return body


def test_personal_event_is_private_but_invitee_can_read_and_rsvp():
    db = database()
    alice = Runtime(db, method="POST", body=event_body())
    created = run(calendar_api.handle(alice, "/api/calendar"))
    assert created["status"] == 201
    assert db.execute("SELECT data FROM calendar_events").fetchone()[0].startswith("sealed:")
    assert [(item[0], item[1]) for item in alice.notifications] == [
        ("bob", "calendar_invitation")]

    bob = Runtime(db, actor="bob", url=(
        "https://forkmesh.test/api/calendar?start=1800000000000&end=1800200000000"))
    listing = run(calendar_api.handle(bob, "/api/calendar"))
    assert [item["title"] for item in listing["payload"]["events"]] == ["Planning"]

    charlie = Runtime(db, actor="charlie", url=bob.url)
    hidden = run(calendar_api.handle(charlie, "/api/calendar"))
    assert hidden["payload"]["events"] == []

    bob._method = "POST"
    bob.body = {"status": "accepted"}
    response = run(calendar_api.handle(
        bob, "/api/calendar/" + "a" * 32 + "/rsvp"))
    assert response["payload"]["status"] == "accepted"
    assert db.execute("SELECT status FROM calendar_attendees").fetchone()[0] == "accepted"


def test_organization_event_is_visible_only_to_members():
    db = database()
    alice = Runtime(db, method="POST", body=event_body(
        scope="organization", calendar="acme", attendees=[]))
    response = run(calendar_api.handle(alice, "/api/calendar"))
    assert response["status"] == 201
    query = "https://forkmesh.test/api/calendar?start=1800000000000&end=1800200000000"
    charlie = Runtime(db, actor="charlie", url=query)
    charlie_listing = run(calendar_api.handle(charlie, "/api/calendar"))
    assert len(charlie_listing["payload"]["events"]) == 1
    assert charlie_listing["payload"]["calendars"] == [
        {"id": "personal", "name": "My calendar", "scope": "personal",
         "role": "owner"},
        {"id": "acme", "name": "acme", "scope": "organization",
         "role": "member", "members": ["alice", "charlie"]},
    ]
    bob = Runtime(db, actor="bob", url=query)
    assert run(calendar_api.handle(bob, "/api/calendar"))["payload"]["events"] == []


def test_organization_guests_and_removed_members_get_no_insider_access():
    db = database()
    db.execute("INSERT INTO org_members VALUES ('org:acme','bi:bob','guest','bob',1)")
    db.commit()
    query = "https://forkmesh.test/api/calendar?start=1800000000000&end=1800200000000"
    charlie = Runtime(db, actor="charlie", method="POST", body=event_body(
        scope="organization", calendar="acme", attendees=[]))
    assert run(calendar_api.handle(charlie, "/api/calendar"))["status"] == 201
    event_path = "/api/calendar/" + "a" * 32

    bob = Runtime(db, actor="bob", url=query)
    listing = run(calendar_api.handle(bob, "/api/calendar"))["payload"]
    assert listing["events"] == []
    assert [item["id"] for item in listing["calendars"]] == ["personal"]
    assert run(calendar_api.handle(bob, event_path))["status"] == 404
    bob._method = "POST"
    bob.body = event_body(scope="organization", calendar="acme", attendees=[])
    assert run(calendar_api.handle(bob, "/api/calendar"))["status"] == 403

    db.execute("UPDATE org_members SET role='auditor' WHERE member_bi='bi:bob'")
    db.commit()
    assert run(calendar_api.handle(bob, "/api/calendar"))["status"] == 403

    db.execute("DELETE FROM org_members WHERE member_bi='bi:charlie'")
    db.commit()
    charlie = Runtime(db, actor="charlie", method="PATCH", url=query,
                      body={"meetingUrl": "https://phish.example"})
    assert run(calendar_api.handle(charlie, event_path))["status"] in (403, 404)
    charlie._method = "GET"
    assert run(calendar_api.handle(charlie, "/api/calendar"))["payload"]["events"] == []


def test_missing_calendar_selection_saves_to_the_private_calendar():
    db = database()
    alice = Runtime(db, method="POST", body=event_body(
        scope="organization", calendar="", attendees=[]))

    response = run(calendar_api.handle(alice, "/api/calendar"))

    assert response["status"] == 201
    assert response["payload"]["event"]["calendar"] == "personal"
    assert response["payload"]["event"]["scope"] == "personal"
    row = db.execute(
        "SELECT org_bi,org_name FROM calendar_events").fetchone()
    assert tuple(row) == ("", "")
    invalid = Runtime(db, method="POST", body=event_body(
        scope="unknown", calendar="", attendees=[]))
    assert run(calendar_api.handle(invalid, "/api/calendar"))["status"] == 400


def test_write_body_must_be_a_json_object():
    db = database()
    listed = Runtime(db, method="POST", body=["not", "an", "object"])
    response = run(calendar_api.handle(listed, "/api/calendar"))
    assert response["status"] == 400
    assert response["payload"] == {"error": "invalid_json"}


def test_due_reminders_are_exactly_once_for_owner_and_non_declined_invitees():
    db = database()
    start = 1_800_086_400_000
    alice = Runtime(db, method="POST", body=event_body(startAt=start))
    run(calendar_api.handle(alice, "/api/calendar"))
    cron = Runtime(db, now=start - 60 * 60 * 1000)
    first = run(calendar_api.deliver_reminders(cron))
    assert first == {"ok": True, "delivered": 2}
    assert {(item[0], item[1]) for item in cron.notifications} == {
        ("alice", "calendar_reminder"), ("bob", "calendar_reminder")}
    second = run(calendar_api.deliver_reminders(cron))
    assert second == {"ok": True, "delivered": 0}


def test_weekly_reminder_preserves_the_event_wall_time():
    db = database()
    first = 1_800_086_400_000
    alice = Runtime(db, method="POST", body=event_body(
        startAt=first, recurrence="weekly", attendees=[]))
    run(calendar_api.handle(alice, "/api/calendar"))
    cron = Runtime(db, now=first + 7 * 86400000 - 10 * 60000)
    assert run(calendar_api.deliver_reminders(cron)) == {
        "ok": True, "delivered": 1}
    row = db.execute(
        "SELECT occurrence_at,offset_minutes FROM calendar_reminder_deliveries"
    ).fetchone()
    assert tuple(row) == (first + 7 * 86400000, 10)


def test_web_and_desktop_surfaces_expose_all_calendar_views_and_defaults():
    assert "calendar" not in dashboard_shell.PAGES
    manifest = json.loads((ROOT.parent / "marketplace" / "packages" /
                           "calendar" / "manifest.json").read_text(
                               encoding="utf-8"))
    page = manifest["contributions"][0]["config"]
    assert page["route"] == "/dashboard/calendar"
    view = (ROOT.parent / "marketplace" / "packages" / "calendar" /
            "content" / "web" / "view.html").read_text(encoding="utf-8")
    script = (ROOT / "public" / "dashboard" / "js" /
              "09-calendar.js").read_text(encoding="utf-8")
    for mode in ("month", "week", "day", "agenda"):
        assert f'data-calendar-mode="{mode}"' in view
    for offset in ("1440", "60", "10"):
        assert f'data-calendar-reminder value="{offset}"' in view
    for control in (
            "data-calendar-timezone", "data-calendar-attendees",
            "data-calendar-attendee-suggestions", 'data-lucide="calendar-plus"'):
        assert control in view
    assert '<option value="personal">My calendar · private</option>' in view
    for contract in (
            "calendarWallTime(", "calendarInputInZone(",
            '"/api/accounts/users"', "calendar.members || []",
            "organization: members.has", 'calendar.id === "personal"',
            "REPOSITORY_COMMITS_CALENDAR", "REPOSITORY_MERGES_CALENDAR",
            "repositoryCalendarEvent(", 'all: "1"', "readOnly: true"):
        assert contract in script
    assert "/api/calendar" in script

    desktop = (ROOT.parent / "desktop" / "src" /
               "MainWindowCalendar.cpp").read_text(encoding="utf-8")
    desktop_repository_calendar = (ROOT.parent / "desktop" / "src" /
                                   "RepositoryCalendar.cpp").read_text(encoding="utf-8")
    for label in ("Month", "Week", "Day", "Agenda"):
        assert f'QStringLiteral("{label}")' in desktop
    for contract in (
            "QTimeZone::availableTimeZoneIds()", "QCompleter",
            'QStringLiteral("/api/accounts/users")',
            'item.value(QStringLiteral("members")).toArray()',
            "calendarRequestProof(method, path, &resource)",
            "agentModelMatchesProvider(",
            'setOcticon(add, "calendar", 14)',
            'calendar->addItem(QStringLiteral("My calendar · private")',
            "saveGuard->setEnabled(true)",
            '{QStringLiteral("timezone"), timezone->currentData().toString()}',
            "refreshRepositoryCalendarEvents()", "rebuildCalendarList()",
            'QStringLiteral("Commits")', 'QStringLiteral("Merges")'):
        assert contract in desktop
    assert "repositoryCalendarEvents(" in desktop_repository_calendar
    assert 'QStringLiteral("--branches")' in desktop_repository_calendar
    resources = (ROOT.parent / "desktop" / "resources" /
                 "icons.qrc").read_text(encoding="utf-8")
    assert 'alias="calendar.svg"' in resources
