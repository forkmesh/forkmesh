#!/usr/bin/env python3
"""Docs feedback validation and storage contract checks."""

import ast
import asyncio
import hashlib
import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import admin_path  # noqa: E402


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
APP_RUNTIME = ROOT / "src" / "app_runtime.py"
SCHEMA = ROOT / "src" / "schema.py"
MIGRATION = ROOT / "migrations" / "0026_feedback.sql"

_WANT_FUNCS = ("_sanitize_feedback_text", "_feedback_fields")
_WANT_CONSTS = (
    "FEEDBACK_SOURCES",
    "FEEDBACK_VOTES",
    "FEEDBACK_NOTE_PATHS",
    "FEEDBACK_MAX_MESSAGE",
    "FEEDBACK_MAX_PATH",
)


def _load():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    body = []
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name in _WANT_FUNCS:
            body.append(node)
        elif isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in _WANT_CONSTS
            for t in node.targets
        ):
            body.append(node)
    module = ast.fix_missing_locations(ast.Module(body=body, type_ignores=[]))
    namespace = {}
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


_NS = _load()
_fields = _NS["_feedback_fields"]
_sanitize = _NS["_sanitize_feedback_text"]
MAX_MESSAGE = _NS["FEEDBACK_MAX_MESSAGE"]
MAX_PATH = _NS["FEEDBACK_MAX_PATH"]


def test_like_payload_is_normalized_without_message():
    out = _fields({
        "source": "docs",
        "vote": "like",
        "path": "/docs/",
        "message": "ignored for likes",
    })

    assert out == ("docs", "like", "/docs/", "")


def test_dislike_payload_accepts_optional_message():
    out = _fields({
        "source": "docs",
        "vote": "dislike",
        "path": "/docs/#install",
        "message": "Install steps need screenshots.",
    })

    assert out == (
        "docs",
        "dislike",
        "/docs/#install",
        "Install steps need screenshots.",
    )


def test_dislike_payload_accepts_empty_message():
    out = _fields({
        "source": "docs",
        "vote": "dislike",
        "path": "/docs/",
        "message": "",
    })

    assert out == ("docs", "dislike", "/docs/", "")


def test_unknown_source_or_vote_is_rejected():
    assert _fields({"source": "blog", "vote": "like", "path": "/docs/"}) is None
    assert _fields({"source": "docs", "vote": "meh", "path": "/docs/"}) is None
    assert _fields(None) is None
    assert _fields([]) is None


def test_world_lobby_feedback_has_a_fixed_private_path():
    assert _fields({
        "source": "world",
        "vote": "feedback",
        "path": "/someone/private?token=secret",
        "message": "The first-run controls need clearer labels.",
    }) == (
        "world",
        "feedback",
        "/world/#lobby-feedback",
        "The first-run controls need clearer labels.",
    )
    assert _fields({
        "source": "world",
        "vote": "feedback",
        "message": "",
    }) is None


def test_website_feedback_has_a_fixed_private_path():
    assert _fields({
        "source": "web",
        "vote": "feedback",
        "path": "/dashboard/settings?token=secret",
        "message": "The dashboard feedback dialog is easy to find.",
    }) == (
        "web",
        "feedback",
        "/dashboard#feedback",
        "The dashboard feedback dialog is easy to find.",
    )
    assert _fields({"source": "web", "vote": "feedback", "message": " "}) is None
    assert _fields({"source": "web", "vote": "like", "message": "hi"}) is None


def test_path_defaults_to_root_when_missing_or_external():
    assert _fields({"source": "docs", "vote": "like"}) == (
        "docs",
        "like",
        "/docs/",
        "",
    )
    assert _fields({
        "source": "docs",
        "vote": "like",
        "path": "https://example.com/docs",
    }) == ("docs", "like", "/docs/", "")


def test_feedback_path_rejects_queries_private_routes_and_unsafe_fragments():
    assert _fields({
        "source": "docs",
        "vote": "like",
        "path": "/docs/?search=private-term#install",
    }) == ("docs", "like", "/docs/", "")
    assert _fields({
        "source": "docs",
        "vote": "like",
        "path": "/alice/private-repo?token=secret",
    }) == ("docs", "like", "/docs/", "")
    assert _fields({
        "source": "docs",
        "vote": "like",
        "path": "/docs/#install<script>",
    }) == ("docs", "like", "/docs/", "")


def test_message_and_path_are_capped_and_control_scrubbed():
    out = _fields({
        "source": "docs",
        "vote": "dislike",
        "path": "/" + ("a" * (MAX_PATH + 50)),
        "message": "line\x00one\x07\ttab\nnl" + ("x" * (MAX_MESSAGE + 50)),
    })

    assert out is not None
    assert len(out[2]) <= MAX_PATH
    assert len(out[3]) <= MAX_MESSAGE
    assert "\x00" not in out[3]
    assert "\x07" not in out[3]
    assert "\t" in out[3]
    assert "\n" in out[3]


def test_feedback_text_sanitizer_handles_non_strings():
    assert _sanitize(None, 20) == ""
    assert _sanitize(12345, 20) == "12345"


def test_feedback_table_is_defined_in_lazy_schema():
    schema_text = SCHEMA.read_text(encoding="utf-8")

    assert "CREATE TABLE IF NOT EXISTS feedback" in schema_text
    assert "ip_hash TEXT" in schema_text
    assert "CREATE INDEX IF NOT EXISTS idx_feedback_ts ON feedback(ts)" in schema_text
    assert (
        "CREATE INDEX IF NOT EXISTS idx_feedback_source_vote "
        "ON feedback(source, vote, ts)"
    ) in schema_text


def test_feedback_migration_file_exists_with_matching_schema():
    assert MIGRATION.exists()
    text = MIGRATION.read_text(encoding="utf-8")

    assert "CREATE TABLE IF NOT EXISTS feedback" in text
    assert "ip_hash TEXT" in text
    assert "CREATE INDEX IF NOT EXISTS idx_feedback_ts ON feedback(ts)" in text
    assert "idx_feedback_source_vote" in text


def test_feedback_endpoint_is_registered():
    runtime_text = APP_RUNTIME.read_text(encoding="utf-8")

    assert 'if url.path in ("/api/feedback", "/api/feedback/"):' in runtime_text
    assert "return await feedback_handler(self.env, request)" in runtime_text


def test_feedback_handler_hashes_ip_and_never_stores_raw_ip():
    entry_text = ENTRY.read_text(encoding="utf-8")

    assert "async def feedback_handler(env, request):" in entry_text
    assert "ip = _client_source_bucket(_transient_client_address(request))" in entry_text
    assert 'headers.get("cf-connecting-ip")' in entry_text
    assert 'headers.get("x-forwarded-for")' in entry_text
    assert "ip_hash = await blind_index(env, ip) if ip else \"\"" in entry_text
    assert "user_agent = _generalized_client_category(request)" in entry_text
    handler = entry_text.split(
        "async def feedback_handler", 1)[1].split(
        "def _validate_security_report", 1)[0]
    assert "_sanitize_feedback_text(" not in handler.split(
        "user_agent =", 1)[1].split("try:", 1)[0]
    assert (
        "INSERT INTO feedback "
        "(ts, source, vote, path, message, ip_hash, user_agent)"
    ) in entry_text
    assert "SELECT ?,?,?,?,?,?,? WHERE (SELECT COUNT(*) FROM feedback) < ?" in entry_text
    assert "DELETE FROM feedback WHERE id NOT IN" not in entry_text


def _load_handler(overrides):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = {"_enqueue_world_feedback_pings", "feedback_handler",
              "_client_source_bucket"}
    consts = {"FEEDBACK_PER_SOURCE_PER_HOUR", "FEEDBACK_SOURCE_RATE_KEY",
              "FEEDBACK_KEPT_PER_SOURCE"}
    body = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in wanted)
        or (isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in consts
            for t in node.targets))
    ]

    async def _stored(_env, sql, *_args):
        return {"id": 1} if "INSERT INTO feedback" in sql else {"n": 0}

    async def _no_budget(_env, _key, _limit):
        return None

    namespace = {
        "ipaddress": __import__("ipaddress"),
        "d1_first": _stored,
        "signup_rate_check": _no_budget,
        "Date": type("Date", (), {"now": staticmethod(lambda: 123456789)}),
        "FEEDBACK_MAX_BODY": 16 * 1024,
        "MAX_FEEDBACK": 5000,
        "MAX_NODE_NAME": 64,
        "FEEDBACK_NOTE_PATHS": _NS["FEEDBACK_NOTE_PATHS"],
        "hashlib": hashlib,
        "json": json,
        "_admin_path_rules": admin_path,
        **overrides,
    }
    module = ast.fix_missing_locations(ast.Module(body=body, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["feedback_handler"]


class _FeedbackRequest:
    def __init__(self, payload):
        self._payload = payload

    async def text(self):
        return json.dumps(self._payload)


@pytest.mark.parametrize(("source", "path", "title"), (
    ("world", "/world/#lobby-feedback", "New World feedback"),
    ("web", "/dashboard#feedback", "New website feedback"),
))
def test_feedback_note_is_recorded_then_pings_admins(source, path, title):
    calls = []

    async def ensure_schema(_env):
        calls.append(("schema",))

    async def blind_index(_env, value):
        assert value in ("203.0.113.4", "feedback-source-v1\n203.0.113.4")
        return "private-client-index"

    async def d1_run(_env, sql, *args):
        calls.append(("run", sql, args))

    async def d1_first(_env, sql, *args):
        calls.append(("first", sql, args))
        return {"id": 7} if "INSERT INTO feedback" in sql else {"n": 0}

    async def d1_all(_env, sql, *args):
        calls.append(("all", sql, args))
        return [{"data": "admin-record"}]

    async def decrypt_row(_env, value):
        assert value == "admin-record"
        return {"name": "operator"}

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        calls.append(("ping", recipient, kind, title, kwargs))
        return True

    handler = _load_handler({
        "method_name": lambda _request: "POST",
        "_feedback_fields": _fields,
        "_transient_client_address": lambda _request: "203.0.113.4",
        "_generalized_client_category": lambda _request: "desktop",
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "d1_run": d1_run,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda value: value == "operator",
        "enqueue_notification": enqueue_notification,
        "json_response": lambda payload, **kwargs: {**payload, **kwargs},
    })
    response = asyncio.run(handler(object(), _FeedbackRequest({
        "source": source,
        "vote": "feedback",
        "message": "Make the first-run controls clearer.",
    })))

    assert response == {"ok": True, "cache_control": "no-store"}
    insert = next(call for call in calls if call[0] == "first" and
                  "INSERT INTO feedback" in call[1])
    ping = next(call for call in calls if call[0] == "ping")
    assert calls.index(insert) < calls.index(ping)
    assert insert[2][1:5] == (
        source, "feedback", path,
        "Make the first-run controls clearer.",
    )
    assert ping[1:4] == ("operator", "pending_inbox", title)
    assert ping[4]["meta"] == {"feedbackSource": source}
    assert ping[4]["body"] == "Make the first-run controls clearer."
    assert ping[4]["source"] == "world-feedback"
    assert ping[4]["dedupe"].startswith("world-feedback:")


def test_feedback_storage_failure_is_not_reported_as_success():
    async def d1_run(_env, sql, *_args):
        if "INSERT INTO feedback" in sql:
            raise RuntimeError("database unavailable")

    async def d1_first(_env, sql, *_args):
        if "INSERT INTO feedback" in sql:
            raise RuntimeError("database unavailable")
        return {"n": 0}

    async def noop(*_args, **_kwargs):
        return None

    handler = _load_handler({
        "method_name": lambda _request: "POST",
        "_feedback_fields": _fields,
        "_transient_client_address": lambda _request: "",
        "_generalized_client_category": lambda _request: "desktop",
        "ensure_schema": noop,
        "blind_index": noop,
        "d1_run": d1_run,
        "d1_first": d1_first,
        "d1_all": noop,
        "decrypt_row": noop,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda _value: True,
        "enqueue_notification": noop,
        "json_response": lambda payload, **kwargs: {**payload, **kwargs},
    })
    response = asyncio.run(handler(object(), _FeedbackRequest({
        "source": "world",
        "vote": "feedback",
        "message": "Keep this form available for retry.",
    })))

    assert response == {
        "error": "feedback_unavailable",
        "status": 503,
        "cache_control": "no-store",
    }



def _stored_feedback():
    import sqlite3
    database = sqlite3.connect(":memory:")
    database.execute(
        "CREATE TABLE feedback (id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "ts INTEGER NOT NULL, source TEXT NOT NULL, vote TEXT NOT NULL, "
        "path TEXT NOT NULL, message TEXT, ip_hash TEXT, user_agent TEXT)")
    database.execute(
        "CREATE TABLE signup_rate (ip_bi TEXT PRIMARY KEY, count INTEGER, "
        "window_start_ts INTEGER)")
    address = {"value": ""}

    def _rows(cursor):
        names = [col[0] for col in cursor.description or []]
        return [dict(zip(names, row)) for row in cursor.fetchall()]

    async def d1_first(_env, sql, *args):
        rows = _rows(database.execute(sql, args))
        database.commit()
        return rows[0] if rows else None

    async def d1_run(_env, sql, *args):
        database.execute(sql, args)
        database.commit()

    async def noop(*_args, **_kwargs):
        return None

    async def blind_index(_env, value):
        return "bi:" + value

    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    rate = [node for node in tree.body
            if isinstance(node, ast.AsyncFunctionDef)
            and node.name == "signup_rate_check"]
    rate_ns = {
        "Date": type("Date", (), {"now": staticmethod(lambda: 123456789)}),
        "d1_first": d1_first,
        "SIGNUP_RATE_WINDOW_MS": 60 * 60 * 1000,
        "SIGNUP_MAX_PER_IP": 3,
        "json_response": lambda payload, **kwargs: {**payload, **kwargs},
    }
    exec(compile(ast.Module(body=rate, type_ignores=[]), str(ENTRY), "exec"),
         rate_ns)
    handler = _load_handler({
        "method_name": lambda _request: "POST",
        "_feedback_fields": _fields,
        "_transient_client_address": lambda _request: address["value"],
        "_generalized_client_category": lambda _request: "desktop",
        "ensure_schema": noop,
        "blind_index": blind_index,
        "signup_rate_check": rate_ns["signup_rate_check"],
        "d1_run": d1_run,
        "d1_first": d1_first,
        "d1_all": noop,
        "decrypt_row": noop,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "valid_node_name": lambda _value: True,
        "enqueue_notification": noop,
        "json_response": lambda payload, **kwargs: {**payload, **kwargs},
        "MAX_FEEDBACK": 20,
    })

    def post(source_address, message):
        address["value"] = source_address
        return asyncio.run(handler(object(), _FeedbackRequest({
            "source": "docs", "vote": "dislike", "path": "/docs/",
            "message": message})))

    return post, database


def test_one_address_gets_a_bounded_number_of_feedback_rows_per_hour():
    post, _database = _stored_feedback()
    answers = [post("2001:db8:44::%x" % (index + 1), "n%d" % index)
               for index in range(31)]
    assert answers[0] == {"ok": True, "cache_control": "no-store"}
    assert answers[-1]["status"] == 429
    assert answers[-1]["error"] == "rate_limited"


def test_a_feedback_flood_only_evicts_rows_of_heavy_sources():
    post, database = _stored_feedback()
    for index in range(5):
        assert post("198.51.100.%d" % (index + 1), "real %d" % index)["ok"]
    for index in range(30):
        post("203.0.113.66", "flood %d" % index)
    for index in range(10):
        post("2001:db8:55::%x" % (index + 1), "flood v6 %d" % index)
    assert post("192.0.2.9", "late real")["ok"]
    messages = [row[0] for row in database.execute(
        "SELECT message FROM feedback ORDER BY id")]
    assert len(messages) == 20
    for index in range(5):
        assert "real %d" % index in messages
    assert "late real" in messages


def test_a_table_full_of_light_sources_refuses_rather_than_evicts():
    post, database = _stored_feedback()
    for index in range(20):
        assert post("198.51.100.%d" % (index + 1), "real %d" % index)["ok"]
    refused = post("192.0.2.9", "one more")
    assert refused["status"] == 503
    messages = [row[0] for row in database.execute(
        "SELECT message FROM feedback ORDER BY id")]
    assert messages == ["real %d" % index for index in range(20)]
