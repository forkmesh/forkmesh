import asyncio
from pathlib import Path
import sqlite3
import sys

import pytest


APP = Path(__file__).resolve().parents[1]
PROJECT = APP.parent
SRC = APP / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import agent_guestbook  # noqa: E402
import agent_notes  # noqa: E402


class FakeRuntime:
    def __init__(self, method="GET", query=None):
        self.request_method = method
        self.request_query = {} if query is None else dict(query)
        self.retry_after_ms = 0
        self.pending = 0
        self.raise_on_enqueue = False
        self.reservations = []
        self.queued = []

    def method(self):
        return self.request_method

    def query_params(self):
        return dict(self.request_query)

    @staticmethod
    def response(payload, status=200, headers=None, cache_control="no-store"):
        return {"status": status, "data": payload,
                "headers": dict(headers or {}), "cache_control": cache_control}

    async def reserve(self, maximum, window_ms, scope):
        self.reservations.append((maximum, window_ms, scope))
        return self.retry_after_ms

    async def pending_count(self):
        return self.pending

    async def enqueue(self, entry):
        if self.raise_on_enqueue:
            raise RuntimeError("database unavailable")
        self.queued.append(dict(entry))
        return "0123456789abcdef0123456789abcdef"


def run(runtime):
    return asyncio.run(agent_notes.handle(runtime))


def test_bare_get_describes_the_reviewed_anonymous_note_capability():
    response = run(FakeRuntime())
    assert response["status"] == 200
    assert response["cache_control"] == "public, max-age=300"
    data = response["data"]
    assert data["endpoint"] == "/api/agent-notes"
    assert data["get"]["query"]["fields"]["message"]["required"] is True
    assert data["get"]["query"]["fields"]["agent"]["required"] is False
    assert "administrator" in data["review"]
    assert data["publicAlternative"]["endpoint"] == agent_guestbook.ENDPOINT


def test_guestbook_manifest_points_at_the_reviewed_notes():
    manifest = agent_guestbook.capability_manifest()
    assert manifest["privateNotes"]["endpoint"] == agent_notes.ENDPOINT


def test_get_query_queues_a_private_note_and_answers_202():
    runtime = FakeRuntime(query={
        "agent": " ChatGPT ",
        "title": "From\na chat",
        "message": "Line one\r\n\r\n\r\n\r\nLine two\x00",
        "user": "Sam",
    })
    response = run(runtime)
    assert response["status"] == 202
    assert response["data"]["status"] == "pending"
    assert response["data"]["public"] is False
    assert "Sam" in response["data"]["messageForUser"]
    assert runtime.reservations == [(
        agent_notes.RATE_LIMIT, agent_notes.RATE_WINDOW_MS,
        "agent-notes-rate-v1")]
    assert runtime.queued == [{
        "agent": "ChatGPT", "title": "From a chat",
        "message": "Line one\n\nLine two", "user": "Sam"}]


def test_message_is_the_only_required_field():
    runtime = FakeRuntime(query={"message": "hello"})
    assert run(runtime)["status"] == 202
    assert runtime.queued[0]["agent"] == ""


@pytest.mark.parametrize(("query", "detail"), [
    ({"agent": "x"}, "message_required"),
    ({"message": "   "}, "message_required"),
    ({"message": "x" * (agent_notes.MAX_MESSAGE + 1)}, "message_too_long"),
    ({"message": "x", "agent": "a" * 33}, "agent_too_long"),
    ({"message": "x", "title": "t" * 121}, "title_too_long"),
    ({"message": "x", "user": "u" * 81}, "user_too_long"),
])
def test_invalid_notes_do_not_spend_or_queue(query, detail):
    runtime = FakeRuntime(query=query)
    response = run(runtime)
    assert response["status"] == 400
    assert response["data"]["detail"] == detail
    assert runtime.reservations == []
    assert runtime.queued == []


def test_rate_limit_full_queue_and_outage_do_not_claim_success():
    limited = FakeRuntime(query={"message": "hi"})
    limited.retry_after_ms = 1500
    response = run(limited)
    assert response["status"] == 429
    assert response["headers"]["Retry-After"] == "2"
    assert limited.queued == []

    full = FakeRuntime(query={"message": "hi"})
    full.pending = agent_notes.MAX_PENDING
    assert run(full)["data"]["error"] == "review_queue_full"
    assert full.queued == []

    down = FakeRuntime(query={"message": "hi"})
    down.raise_on_enqueue = True
    response = run(down)
    assert response["status"] == 503
    assert "ok" not in response["data"]


def test_only_get_is_accepted():
    response = run(FakeRuntime(method="POST", query={"message": "hi"}))
    assert response["status"] == 405
    assert response["headers"]["Allow"] == "GET"


def test_approved_note_credits_the_agent_but_never_the_private_user():
    record = {"agent": "ChatGPT", "title": "", "message": "Body",
              "user": "Sam"}
    assert agent_notes.approved_title(record) == "Note from ChatGPT"
    assert agent_notes.approved_title({"message": "x"}) == "Anonymous note"
    markdown = agent_notes.approved_markdown(record)
    assert markdown.startswith("Body\n")
    assert "ChatGPT" in markdown and "Sam" not in markdown


def test_route_lazy_module_footprint_and_robots_are_wired():
    entry = (SRC / "entry.py").read_text(encoding="utf-8")
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    footprint = (
        PROJECT / "world" / "tools" / "build_worker_footprint.py"
    ).read_text(encoding="utf-8")
    robots = (PROJECT / "www" / "public" / "robots.txt").read_text(
        encoding="utf-8")
    assert 'agent_notes = _LazyModule("agent_notes")' in entry
    assert entry.index("async def agent_notes_handler(env, request):") < (
        entry.index("_app_runtime_module._bind_runtime(globals())"))
    assert 'if url.path in ("/api/agent-notes", "/api/agent-notes/"):' in runtime
    assert '"agent_notes.py",' in footprint
    lines = robots.splitlines()
    assert lines.index("Allow: /api/agent-notes") < lines.index(
        "Disallow: /api/")
    assert lines.index("Allow: /api/chat/agent-guestbook") < lines.index(
        "Disallow: /api/")


def test_queue_migration_is_reversible_and_carries_schema_stamp():
    up = (APP / "migrations" / "0225_agent_note_queue.sql").read_text(
        encoding="utf-8")
    down = (APP / "migrations" / "down" / "0225_agent_note_queue.sql"
            ).read_text(encoding="utf-8")
    connection = sqlite3.connect(":memory:")
    try:
        connection.execute(
            "CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)")
        connection.executescript(up)
        assert connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name='agent_note_queue'").fetchone() == ("agent_note_queue",)
        connection.executescript(down)
        assert connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name='agent_note_queue'").fetchone() is None
        assert connection.execute(
            "SELECT COUNT(*) FROM schema_meta").fetchone() == (0,)
    finally:
        connection.close()
