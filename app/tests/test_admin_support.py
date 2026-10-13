#!/usr/bin/env python3
"""The console's Support sections: mobile bug reports and site feedback.

Runs app/src/admin_support.py against an in-memory SQLite database built from
the real runtime DDL, with the Worker primitives it binds replaced by small
doubles and the real admin shell drawing the page.
"""

import ast
import asyncio
import html
import importlib.util
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlparse


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import schema  # noqa: E402

NOW = 1_789_560_000_000
DAY = 86_400_000
CSRF = "token-123"
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")


def _entry_defs(*names):
    namespace = {"quote": quote}
    for node in ast.parse(ENTRY_TEXT).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and (
                node.name in names):
            exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
    return namespace


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})


class FakeRequest:
    def __init__(self, url, method="GET", body="", cookie=""):
        self.url = url
        self.method = method
        self._body = body
        self.headers = {"cookie": cookie} if cookie else {}

    async def text(self):
        return self._body


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name + "_under_test", SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Harness:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for sql in schema.SCHEMA_STATEMENTS:
            try:
                self.db.execute(sql)
            except sqlite3.Error:
                pass
        self.audits = []
        self.shell = _load("admin_shell")
        self.support = _load("admin_support")
        runtime = _entry_defs("_admin_href")
        harness = self

        async def d1_all(_env, sql, *args):
            return [dict(r) for r in harness.db.execute(sql, args)]

        async def d1_first(_env, sql, *args):
            rows = await d1_all(_env, sql, *args)
            return rows[0] if rows else None

        async def d1_run(_env, sql, *args):
            harness.db.execute(sql, args)

        async def decrypt_row(_env, value):
            return json.loads(value)

        async def audit(env, actor, action, target_type="", target="",
                        outcome="success", details=None):
            harness.audits.append(
                (actor, action, target_type, target, outcome, details))

        runtime.update({
            "Response": FakeResponse,
            "Date": SimpleNamespace(now=lambda: NOW),
            "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
            "decrypt_row": decrypt_row,
            "_html_escape": lambda value: html.escape(str(value)),
            "_admin_csrf_ok": lambda env, form: (
                form.get("csrf", [""])[0] == CSRF),
            "_audit_sensitive_action": audit,
            "method_name": lambda request: request.method,
            "parse_qs": parse_qs, "urlparse": urlparse,
            "_admin_shell": self.shell,
        })
        self.shell._bind_runtime({
            "d1_first": d1_first, "Date": SimpleNamespace(now=lambda: NOW)})
        self.support._bind_runtime(runtime)
        self.env = SimpleNamespace(ADMIN_PATH="ops-console")

    def report(self, index, severity="normal", status="new", platform="ios",
               account="", **record):
        body = {"message": "Report %d" % index, "screen": "Chat",
                "osVersion": "iOS 18.6"}
        body.update(record)
        report_id = "%032x" % (index + 1)
        self.db.execute(
            "INSERT INTO bug_reports (report_id,reporter_bi,account,source,"
            "platform,app_version,severity,status,data,created_at) "
            "VALUES (?,?,?,?,?,?,?,?,?,?)",
            (report_id, "r%d" % index, account, "mobile", platform, "1.1.0+4",
             severity, status, json.dumps(body), NOW - index * 3_600_000))
        return report_id

    def feedback(self, source, vote, message="", path="/docs/", ts=NOW):
        self.db.execute(
            "INSERT INTO feedback (ts,source,vote,path,message,user_agent) "
            "VALUES (?,?,?,?,?,?)", (ts, source, vote, path, message, "browser"))

    def get(self, query):
        request = FakeRequest("https://app.example/ops-console?" + query)
        params = parse_qs(query, keep_blank_values=True)
        return asyncio.run(self.support.support_handler(
            self.env, request, params, '<input type="hidden" name="csrf" '
            'value="%s">' % CSRF, "admin=root", "root"))

    def post(self, query, body):
        request = FakeRequest(
            "https://app.example/ops-console?" + query, "POST", body)
        return asyncio.run(self.support.support_handler(
            self.env, request, parse_qs(query), "", "admin=root", "root"))

    def status(self, report_id):
        return self.db.execute(
            "SELECT status FROM bug_reports WHERE report_id=?",
            (report_id,)).fetchone()[0]


def test_only_the_support_views_are_handled_here():
    h = Harness()
    assert h.get("view=users") is None
    assert h.get("view=overview") is None


def test_bug_inbox_lists_new_reports_newest_first_and_opens_the_first():
    h = Harness()
    first = h.report(0, "crash", account="ada-lovelace",
                     message="App closes on the repo screen",
                     contact="ada@example.com", device="Pixel 9")
    h.report(1, "high")
    h.report(2, "low", status="closed")
    response = h.get("view=bugs")
    page = response.body

    assert response.status == 200
    assert response.headers["cache-control"] == "no-store"
    assert "<title>Bug reports · ForkMesh admin</title>" in page
    assert '<a class="ab-item on" href="/ops-console?view=bugs"' in page
    items = re.findall(r'<div class="msg">([^<]*)</div>', page)
    assert items == ["App closes on the repo screen", "Report 1"]
    # Tabs count every status; the closed report is not listed under New.
    assert re.search(r'aria-current="page">New <span class="count">2</span>',
                     page)
    assert '>Closed <span class="count">1</span>' in page
    # The first report is open: message, contact, device and account link.
    assert '<div class="quote">App closes on the repo screen</div>' in page
    assert "<dd>ada@example.com</dd>" in page
    assert "<dd>Pixel 9</dd>" in page
    assert "view=users&amp;user=ada-lovelace" in page
    assert '<input type="hidden" name="report" value="%s">' % first in page
    assert 'name="bug_status" value="new" aria-pressed="true"' in page
    assert 'name="csrf" value="token-123"' in page
    assert "<script" not in page.lower()


def test_a_report_links_the_issue_it_was_filed_as():
    h = Harness()
    h.report(0, issueRepo="forkmesh/forkmesh", issueNumber=712)
    h.report(1)
    filed = h.get("view=bugs").body
    assert ('<dd><a href="/forkmesh/forkmesh/issues/712">'
            'forkmesh/forkmesh#712</a></dd>') in filed
    unfiled = h.get("view=bugs&report=%032x" % 2).body
    assert '<dd><span class="muted">not filed</span></dd>' in unfiled


def test_bug_inbox_filters_by_status_severity_and_platform():
    h = Harness()
    h.report(0, "crash", platform="android")
    h.report(1, "crash", platform="ios")
    h.report(2, "low", platform="ios")
    h.report(3, "high", status="triaged", platform="ios")

    page = h.get("view=bugs&severity=crash&platform=ios").body
    assert re.findall(r'<div class="msg">([^<]*)</div>', page) == ["Report 1"]

    triaged = h.get("view=bugs&status=triaged").body
    assert re.findall(r'<div class="msg">([^<]*)</div>', triaged) == [
        "Report 3"]

    empty = h.get("view=bugs&status=closed&severity=low").body
    assert "No closed reports match these filters." in empty


def test_a_named_report_opens_even_when_it_is_not_on_the_page():
    h = Harness()
    for index in range(3):
        h.report(index)
    closed = h.report(9, status="closed", message="Old closed report")
    page = h.get("view=bugs&report=%s" % closed).body
    assert '<div class="quote">Old closed report</div>' in page


def test_reporter_text_is_escaped_everywhere():
    h = Harness()
    hostile = '<img src=x onerror=alert(1)>"'
    h.report(0, message=hostile, screen=hostile, contact=hostile)
    page = h.get("view=bugs").body
    assert "<img src=x" not in page
    assert "&lt;img src=x onerror=alert(1)&gt;" in page


def test_triage_status_changes_are_csrf_checked_audited_and_redirected():
    h = Harness()
    report = h.report(0)

    blocked = h.post("view=bugs&status=new", "action=set_bug_status&report="
                     + report + "&bug_status=triaged&csrf=wrong")
    assert blocked.status == 303
    assert "notice=csrf" in blocked.headers["location"]
    assert h.status(report) == "new"
    assert h.audits == []

    saved = h.post("view=bugs&status=new", "action=set_bug_status&report="
                   + report + "&bug_status=triaged&csrf=" + CSRF)
    assert saved.status == 303
    assert saved.headers["location"] == (
        "/ops-console?admin=root&view=bugs&report=%s&status=new"
        "&notice=status_saved" % report)
    assert h.status(report) == "triaged"
    assert h.audits == [("root", "admin.console_set_bug_status", "bug_report",
                         report, "success", {"from": "new", "to": "triaged"})]

    again = h.post("view=bugs", "action=set_bug_status&report=" + report
                   + "&bug_status=triaged&csrf=" + CSRF)
    assert "notice=status_unchanged" in again.headers["location"]
    assert len(h.audits) == 1


def test_triage_refuses_unknown_statuses_and_missing_reports():
    h = Harness()
    report = h.report(0)
    bogus = h.post("view=bugs", "action=set_bug_status&report=" + report
                   + "&bug_status=deleted&csrf=" + CSRF)
    assert "notice=invalid_status" in bogus.headers["location"]
    assert h.status(report) == "new"

    missing = h.post("view=bugs", "action=set_bug_status&report="
                     + "f" * 32 + "&bug_status=closed&csrf=" + CSRF)
    assert "notice=report_missing" in missing.headers["location"]
    assert h.audits[-1][4] == "failed"

    shaped = h.post("view=bugs", "action=set_bug_status&report=../x"
                    "&bug_status=closed&csrf=" + CSRF)
    assert "notice=report_missing" in shaped.headers["location"]

    notice = h.get("view=bugs&notice=status_saved").body
    assert "Report status saved." in notice


def test_feedback_totals_filters_and_escapes():
    h = Harness()
    h.feedback("docs", "like", ts=NOW - 3)
    h.feedback("docs", "like", ts=NOW - 2)
    h.feedback("docs", "dislike", "Step 3 is <b>wrong</b>",
               "/docs/#ssh", ts=NOW - 1)
    h.feedback("world", "feedback", "Kiosk text is tiny", "/world/#lobby-feedback",
               ts=NOW)

    page = h.get("view=feedback").body
    assert "<title>Docs &amp; World feedback · ForkMesh admin</title>" in page
    assert re.search(r'<div class="l">Helpful<svg.*?<div class="n">2</div>',
                     page)
    assert "33% of docs votes" in page
    assert "Step 3 is &lt;b&gt;wrong&lt;/b&gt;" in page
    assert "Showing 1–4 of 4" in page

    world = h.get("view=feedback&source=world").body
    assert "Kiosk text is tiny" in world
    assert "Step 3 is" not in world
    assert "Showing 1–1 of 1" in world



def test_console_dispatches_support_before_its_own_post_actions():
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    start = runtime.index("    async def _admin(self, request):")
    admin = runtime[start:runtime.index("\n    async def _route(", start)]
    support = admin.index(
        'if params.get("view", [""])[0] in ("bugs", "feedback", "agent-notes"):')
    assert admin.index("if not await _check_admin_page_auth(") < support
    assert admin.index("csrf_field = (") < support
    assert support < admin.index('if method_name(request) == "POST":')
    assert '_admin_support = _LazyModule("admin_support")' in ENTRY_TEXT
    assert ('admin_support_handler = _admin_support.export("support_handler")'
            in ENTRY_TEXT)


def _agent_note_harness():
    h = Harness()
    ids = iter("%032x" % (0xabc0 + n) for n in range(100))

    async def encrypt_row(_env, value):
        return json.dumps(value)

    async def blind_index(_env, value):
        return "bi:" + value

    h.support._bind_runtime({
        "encrypt_row": encrypt_row, "blind_index": blind_index,
        "_ap_uuid": lambda: next(ids)})
    return h


def _queue_note(h, index, status="pending", **record):
    body = {"agent": "ChatGPT", "title": "Idea %d" % index,
            "message": "Note <b>%d</b>" % index, "user": "Sam"}
    body.update(record)
    entry_id = "%032x" % (index + 1)
    h.db.execute(
        "INSERT INTO agent_note_queue (entry_id,status,created_at,"
        "reviewed_at,note_id,data) VALUES (?,?,?,0,'',?)",
        (entry_id, status, NOW - index * 60_000, json.dumps(body)))
    return entry_id


def test_agent_note_queue_lists_pending_notes_escaped():
    h = _agent_note_harness()
    first = _queue_note(h, 0)
    _queue_note(h, 1, status="rejected")
    page = h.get("view=agent-notes").body
    assert "Agent notes" in page
    assert "Idea 0" in page and "Idea 1" not in page
    assert "Note &lt;b&gt;0&lt;/b&gt;" in page and "<b>0</b>" not in page
    assert 'name="entry" value="%s"' % first in page
    assert 'value="approve"' in page and 'value="reject"' in page
    assert "Idea 1" in h.get("view=agent-notes&status=rejected").body


def test_approving_an_agent_note_publishes_one_public_note_once():
    h = _agent_note_harness()
    entry = _queue_note(h, 0)
    blocked = h.post("view=agent-notes", "action=review_agent_note&entry="
                     + entry + "&decision=approve&csrf=wrong")
    assert "notice=csrf" in blocked.headers["location"]
    saved = h.post("view=agent-notes", "action=review_agent_note&entry="
                   + entry + "&decision=approve&csrf=" + CSRF)
    assert "notice=note_approved" in saved.headers["location"]
    row = h.db.execute(
        "SELECT status, note_id FROM agent_note_queue WHERE entry_id=?",
        (entry,)).fetchone()
    assert row["status"] == "approved"
    note = h.db.execute(
        "SELECT owner_bi, visibility, published_at, data FROM notes "
        "WHERE note_id=?", (row["note_id"],)).fetchone()
    assert note["owner_bi"] == "bi:root"
    assert note["visibility"] == "public"
    assert note["published_at"] == NOW
    data = json.loads(note["data"])
    assert data["title"] == "Idea 0"
    assert data["markdown"].startswith("Note <b>0</b>")
    assert "ChatGPT" in data["markdown"] and "Sam" not in data["markdown"]
    assert h.db.execute(
        "SELECT COUNT(*) FROM note_versions WHERE note_id=?",
        (row["note_id"],)).fetchone()[0] == 1
    again = h.post("view=agent-notes", "action=review_agent_note&entry="
                   + entry + "&decision=approve&csrf=" + CSRF)
    assert "notice=note_reviewed" in again.headers["location"]
    assert h.db.execute("SELECT COUNT(*) FROM notes").fetchone()[0] == 1
    assert [a[1] for a in h.audits] == ["admin.console_review_agent_note"]


def test_rejecting_an_agent_note_publishes_nothing():
    h = _agent_note_harness()
    entry = _queue_note(h, 0)
    bad = h.post("view=agent-notes", "action=review_agent_note&entry="
                 + entry + "&decision=publish&csrf=" + CSRF)
    assert "notice=note_invalid" in bad.headers["location"]
    saved = h.post("view=agent-notes", "action=review_agent_note&entry="
                   + entry + "&decision=reject&csrf=" + CSRF)
    assert "notice=note_rejected" in saved.headers["location"]
    assert h.db.execute(
        "SELECT status FROM agent_note_queue WHERE entry_id=?",
        (entry,)).fetchone()[0] == "rejected"
    assert h.db.execute("SELECT COUNT(*) FROM notes").fetchone()[0] == 0
    missing = h.post("view=agent-notes", "action=review_agent_note&entry="
                     + "f" * 32 + "&decision=reject&csrf=" + CSRF)
    assert "notice=note_missing" in missing.headers["location"]
