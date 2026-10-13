import asyncio
from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import chat_reads_api as api  # noqa: E402


DM = "direct:" + "a" * 32
CHANNEL = "private:" + "b" * 32


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        self.db.executescript(
            (ROOT / "migrations" / "0255_chat_read_marks.sql")
            .read_text(encoding="utf-8"))
        self.actor = ""
        self.request_method = "GET"
        self.request_data = {}
        self.clock = 1_800_000_000_000
        self.account_events = []

    def use(self, method, actor, data=None):
        self.request_method = method
        self.actor = actor
        self.request_data = data
        return self

    def method(self):
        return self.request_method

    def now(self):
        return self.clock

    def response(self, data, status=200, cache_control=None,
                 extra_headers=None):
        return {"status": status, "data": data,
                "headers": dict(extra_headers or {})}

    async def ensure_schema(self):
        return None

    async def json_body(self, _limit):
        if not isinstance(self.request_data, dict):
            return None, "invalid_json"
        return self.request_data, ""

    async def session(self, _data=None):
        if self.actor not in {"alice", "bob"}:
            return "", None
        return "bi:" + self.actor, {"name": self.actor}

    async def notify_account(self, owner, topic, started_by=""):
        self.account_events.append((owner, topic))

    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]

    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()


def call(runtime, method, actor, data=None):
    runtime.use(method, actor, data)
    return asyncio.run(api.handle(runtime, "/api/chat/reads"))


def test_marks_round_trip_and_ring_the_readers_other_devices():
    runtime = FakeRuntime()
    saved = call(runtime, "POST", "alice",
                 {"marks": {DM: 100, CHANNEL: 200, "#general": 300}})
    assert saved["status"] == 200
    assert saved["data"]["advanced"] is True
    assert runtime.account_events == [("alice", "chat-read")]
    listed = call(runtime, "GET", "alice")
    assert listed["data"]["marks"] == {
        DM: 100, CHANNEL: 200, "#general": 300}
    assert call(runtime, "GET", "bob")["data"]["marks"] == {}


def test_listing_reports_when_each_mark_was_saved():
    runtime = FakeRuntime()
    call(runtime, "POST", "alice", {"marks": {"#general": 300}})
    runtime.clock += 5_000
    call(runtime, "POST", "alice", {"marks": {DM: 100}})
    runtime.clock += 5_000
    call(runtime, "POST", "alice", {"marks": {"#general": 200}})
    listed = call(runtime, "GET", "alice")["data"]
    assert listed["readAt"] == {
        "#general": 1_800_000_000_000, DM: 1_800_000_005_000}


def test_marks_never_move_backwards_and_stale_posts_stay_silent():
    runtime = FakeRuntime()
    call(runtime, "POST", "alice", {"marks": {DM: 500}})
    runtime.account_events.clear()
    stale = call(runtime, "POST", "alice", {"marks": {DM: 400}})
    assert stale["data"]["advanced"] is False
    assert runtime.account_events == []
    assert call(runtime, "GET", "alice")["data"]["marks"] == {DM: 500}
    call(runtime, "POST", "alice", {"marks": {DM: 600}})
    assert call(runtime, "GET", "alice")["data"]["marks"] == {DM: 600}
    assert runtime.account_events == [("alice", "chat-read")]


def test_invalid_marks_and_sessions_are_refused():
    runtime = FakeRuntime()
    assert call(runtime, "GET", "mallory")["status"] == 401
    for marks in (
        {},
        {"room-without-hash": 1},
        {"direct:xyz": 1},
        {DM: "1"},
        {DM: True},
        {DM: 0},
        {DM: runtime.clock + api.READS_FUTURE_SLACK_MS + 1},
        {"#c%d" % n: 1 for n in range(api.READS_POST_MAX + 1)},
        [DM],
    ):
        result = call(runtime, "POST", "alice", {"marks": marks})
        assert result["status"] == 400, marks
    assert runtime.account_events == []
    assert call(runtime, "PUT", "alice")["status"] == 405


def test_an_account_keeps_only_its_most_recent_marks():
    runtime = FakeRuntime()
    total = api.READS_KEEP_MAX + 5
    for start in range(0, total, api.READS_POST_MAX):
        runtime.clock += 1
        batch = {"#c%d" % n: 10 for n in range(
            start, min(total, start + api.READS_POST_MAX))}
        call(runtime, "POST", "alice", {"marks": batch})
    count = runtime.db.execute(
        "SELECT COUNT(*) FROM chat_read_marks").fetchone()[0]
    assert count == api.READS_KEEP_MAX
    marks = call(runtime, "GET", "alice")["data"]["marks"]
    assert len(marks) == api.READS_LIST_MAX
    assert "#c%d" % (total - 1) in marks


def test_signed_digest_is_order_independent():
    assert api.marks_digest({DM: 1, CHANNEL: 2}) == api.marks_digest(
        {CHANNEL: 2, DM: 1})
    assert api.marks_digest({DM: 1}) != api.marks_digest({DM: 2})
