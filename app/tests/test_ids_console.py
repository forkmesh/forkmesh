#!/usr/bin/env python3
import ast
import asyncio
import html
import importlib.util
import json
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlparse


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
MIGRATIONS = ROOT / "migrations"
NOW = 1_789_560_000_000
HOUR = 3_600_000
DAY = 24 * HOUR
ENTRY_TEXT = (SRC / "entry.py").read_text(encoding="utf-8")


def _entry_defs(*names):
    namespace = {"quote": quote}
    for node in ast.parse(ENTRY_TEXT).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and (
                node.name in names):
            exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
    return namespace


def _load(name):
    spec = importlib.util.spec_from_file_location(
        name + "_under_test", SRC / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})


class FakeRequest:
    def __init__(self, url, method="GET"):
        self.url = url
        self.method = method
        self.headers = {}


class Harness:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            (MIGRATIONS / "0197_security_ids.sql").read_text(encoding="utf-8"))
        self.db.execute(
            "ALTER TABLE security_incidents ADD COLUMN email_state TEXT")
        self.db.executescript(
            (MIGRATIONS / "0243_security_incidents_created_index.sql")
            .read_text(encoding="utf-8").split("INSERT INTO schema_meta")[0])
        self.serial = 0
        self.shell = _load("admin_shell")
        self.ids = _load("ids_console")
        harness = self

        async def d1_all(_env, sql, *args):
            return [dict(r) for r in harness.db.execute(sql, args)]

        async def d1_first(_env, sql, *args):
            rows = await d1_all(_env, sql, *args)
            return rows[0] if rows else None

        clock = SimpleNamespace(now=lambda: NOW)
        runtime = _entry_defs("_admin_href")
        runtime.update({
            "Response": FakeResponse, "Date": clock,
            "d1_all": d1_all, "d1_first": d1_first,
            "_html_escape": lambda value: html.escape(str(value)),
            "method_name": lambda request: request.method,
            "urlparse": urlparse,
            "_admin_shell": self.shell,
        })
        self.shell._bind_runtime({"d1_first": d1_first, "Date": clock})
        self.ids._bind_runtime(runtime)
        self.env = SimpleNamespace(ADMIN_PATH="ops-console")

    def incident(self, node="n1", severity="info",
                 detector="ssh_tool_session", summary="tool session",
                 at=NOW, detail=None, delivered=False, email_state=None):
        self.serial += 1
        self.db.execute(
            "INSERT INTO security_incidents (id,node_id,created_ms,severity,"
            "detector,summary,detail,delivered_ms,email_state) "
            "VALUES (?,?,?,?,?,?,?,?,?)",
            ("inc-%d" % self.serial, node, at, severity, detector, summary,
             json.dumps(detail or {}), at + 1000 if delivered else None,
             email_state))

    def beat(self, node, at=NOW, state="normal"):
        self.db.execute(
            "INSERT INTO ids_heartbeats (node_id,beat_ms,state) VALUES (?,?,?)",
            (node, at, state))

    def page(self, query="view=ids", method="GET"):
        request = FakeRequest(
            "https://app.forkmesh.com/ops-console?" + query, method)
        return asyncio.run(self.ids.admin_handler(
            self.env, request, parse_qs(query), "", "admin=alice", "alice"))


def test_page_shows_node_health_and_window_stats():
    h = Harness()
    h.beat("n1")
    h.beat("n2", at=NOW - 10 * 60 * 1000)
    h.beat("n3", state="lockdown")
    h.incident("n3", "lockdown", "service_user_outside_unit",
               "forkmesh-app process outside its unit", at=NOW - HOUR,
               detail={"pid": 42, "lockdown": {"seal": "ok"}}, delivered=True,
               email_state="sent")
    h.incident("n1", "alert", "listener_unexpected", "tcp 9999 open",
               at=NOW - 2 * HOUR)
    h.incident("r1", "info", at=NOW - 3 * HOUR)
    h.incident("n1", "info", at=NOW - 30 * HOUR)
    h.db.execute(
        "INSERT INTO cluster_quarantine (id,node_id,kind,signer,created_ms,"
        "body,sig) VALUES ('q1','n3','self','n3',?, '{}','x')", (NOW - HOUR,))

    response = h.page()
    page = response.body
    assert response.status == 200
    assert response.headers["cache-control"] == "no-store"
    assert ">Intrusion detection<" in page and 'aria-current="page"' in page
    assert "1 / 3" in page
    assert "Silent" in page and "Lockdown" in page and "Normal" in page
    assert "Locked down" in page
    assert "No heartbeat" in page and ">r1<" in page
    assert "Incidents, last 24 hours" in page
    assert "3 incidents, 2 paged administrators" in page
    assert "IDS incidents, last 24 hours" in page
    assert "service_user_outside_unit" in page
    assert "Paged" in page and "email sent" in page
    assert "Pending" in page
    assert "&quot;pid&quot;: 42" in page
    assert "3 incidents, newest first" in page

    month = h.page("view=ids&range=month").body
    assert "4 incidents, newest first" in month
    assert "Incidents, last 30 days" in month


def test_filters_narrow_the_chart_groups_and_log():
    h = Harness()
    for hours in range(5):
        h.incident("n1", "info", "ssh_tool_session", at=NOW - hours * HOUR)
    h.incident("n2", "alert", "ssh_interactive_login", "root shell <b>",
               at=NOW - HOUR)

    grouped = h.page().body
    assert grouped.index("ssh_tool_session") < grouped.index(
        "ssh_interactive_login")
    assert "root shell &lt;b&gt;" in grouped and "root shell <b>" not in grouped
    assert "Frequency: 5 incidents" in grouped

    paged = h.page("view=ids&severity=paged").body
    assert "1 incident, newest first" in paged
    assert "ssh_tool_session" not in paged.split("ids-groups")[1]

    by_node = h.page("view=ids&group=node").body
    groups = by_node.split("ids-groups")[1].split("ids-log")[0]
    assert ">n1<" in groups and ">n2<" in groups and "Detector" not in groups

    node = h.page("view=ids&node=n2").body
    assert "1 incident, newest first" in node and "Node: n2" in node

    junk = h.page("view=ids&node=%3Cx%3E&severity=bogus&range=year").body
    assert "6 incidents, newest first" in junk and "&lt;x&gt;" not in junk


def test_log_pages_newest_first():
    h = Harness()
    for index in range(60):
        h.incident("n1", "info", "reboot", "boot %d" % index,
                   at=NOW - index * 60_000)
    first = h.page().body
    assert "Page 1 of 2" in first and "boot 0<" in first
    assert "boot 59<" not in first.split("ids-log")[1]
    second = h.page("view=ids&page=2").body
    assert "Page 2 of 2" in second and "boot 59<" in second


def test_page_is_read_only():
    h = Harness()
    assert h.page(method="POST").status == 405


def test_nav_badge_counts_recent_lockdowns_and_alerts():
    h = Harness()
    h.incident("n1", "alert", at=NOW - HOUR)
    badges = asyncio.run(h.shell.nav_badges(h.env))
    assert badges["ids"] == ("1", "warn")
    h.incident("n1", "lockdown", at=NOW - HOUR)
    h.incident("n1", "lockdown", at=NOW - 2 * DAY)
    badges = asyncio.run(h.shell.nav_badges(h.env))
    assert badges["ids"] == ("1", "bad")


def test_console_dispatches_the_ids_view_and_the_nav_links_it():
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    assert runtime.index("_ids_console.VIEW") < runtime.index(
        'if method_name(request) == "POST":\n            try:\n'
        '                form = clean_hidden_query(parse_qs(')
    assert '_ids_console = _LazyModule("ids_console")' in ENTRY_TEXT
    shell = _load("admin_shell")
    keys = [key for _group, items in shell.NAV for key, *_rest in items]
    assert "ids" in keys and "shield" in shell.ICONS
