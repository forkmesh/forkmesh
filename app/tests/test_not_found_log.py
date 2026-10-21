#!/usr/bin/env python3
import ast
import asyncio
import hashlib
import hmac
import html
import importlib.util
import json
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import parse_qs, quote, urlencode, urlparse


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import schema  # noqa: E402

NOW = 1_789_560_000_000
HOUR = 3_600_000
CSRF = "token-123"
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


def _run(coro):
    return asyncio.run(coro)


class FakeResponse:
    def __init__(self, body, status=200, headers=None):
        self.body = body
        self.status = status
        self.headers = dict(headers or {})


class FakeRequest:
    def __init__(self, url, method="GET", body="", headers=None):
        self.url = url
        self.method = method
        self._body = body
        self.headers = dict(headers or {})

    async def text(self):
        return self._body


class Harness:
    def __init__(self, admins=("alice", "bob")):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for sql in schema.SCHEMA_STATEMENTS:
            try:
                self.db.execute(sql)
            except sqlite3.Error:
                pass
        for index, name in enumerate(admins):
            self.db.execute(
                "INSERT INTO users (user_bi, data, is_admin) VALUES (?,?,1)",
                ("bi-%d" % index, json.dumps({"name": name})))
        self.db.execute(
            "INSERT INTO users (user_bi, data, is_admin) VALUES (?,?,1)",
            ("bi-svc", json.dumps({"name": "forkbot",
                                   "service_account": True})))
        self.now = NOW
        self.pings = []
        self.audits = []
        self.shell = _load("admin_shell")
        self.log = _load("not_found_log")
        self.probe = _load("probe_ban")
        self.local_bans = {}
        runtime = _entry_defs("_admin_href", "json_response")
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

        async def encrypt_row(_env, value):
            return json.dumps(value)

        async def enqueue_notification(env, recipient, kind, title, **fields):
            harness.pings.append((recipient, kind, title, fields))
            return True

        async def audit(env, actor, action, target_type="", target="",
                        outcome="success", details=None):
            harness.audits.append(
                (actor, action, target_type, target, outcome, details))

        async def ensure_schema(_env):
            return None

        async def blind_index(_env, value):
            return hmac.new(b"test-key", str(value).strip().lower().encode(),
                            hashlib.sha256).hexdigest()

        async def bounded_json_request(request):
            return json.loads(await request.text())

        async def privacy_filter(_env, path):
            if path.startswith("/secret-owner/"):
                return "/private-or-unpublished-repository/page"
            return path

        clock = SimpleNamespace(now=lambda: harness.now)
        runtime.update({
            "Response": FakeResponse,
            "Date": clock,
            "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
            "decrypt_row": decrypt_row, "encrypt_row": encrypt_row,
            "ensure_schema": ensure_schema,
            "enqueue_notification": enqueue_notification,
            "bounded_json_request": bounded_json_request,
            "_privacy_safe_log_path": privacy_filter,
            "clean_string": lambda value, n: str(value or "")[:n],
            "valid_node_name": lambda name: bool(name) and name.isalnum(),
            "MAX_NODE_NAME": 39,
            "_html_escape": lambda value: html.escape(str(value)),
            "_admin_csrf_ok": lambda env, form: (
                form.get("csrf", [""])[0] == CSRF),
            "_audit_sensitive_action": audit,
            "method_name": lambda request: request.method,
            "parse_qs": parse_qs, "urlparse": urlparse,
            "_admin_shell": self.shell,
            "_probe_ban": self.probe,
            "_probe_ban_local": self.local_bans,
            "blind_index": blind_index,
        })
        runtime["json_response"].__globals__["Response"] = FakeResponse
        runtime["json_response"].__globals__["json"] = json
        self.shell._bind_runtime({"d1_first": d1_first, "Date": clock})
        self.log._bind_runtime(runtime)
        self.probe._bind_runtime(runtime)
        self.env = SimpleNamespace(
            ADMIN_PATH="ops-console",
            APP_ORIGIN="https://app.forkmesh.com",
            WWW_ORIGIN="https://forkmesh.com",
            WORLD_ORIGIN="https://world.forkmesh.com")

    def hit(self, path, site="app.forkmesh.com", method="GET", referrer="",
            times=1, client=None):
        for _ in range(times):
            assert _run(self.log.record(
                self.env, site, method, path, referrer, now=self.now,
                client=client))

    def rows(self):
        return [dict(r) for r in self.db.execute(
            "SELECT * FROM not_found_paths ORDER BY hits DESC, path")]

    def admin(self, query="view=not-found", method="GET", form=None):
        body = urlencode(form or {}, doseq=True)
        request = FakeRequest(
            "https://app.forkmesh.com/ops-console?" + query, method, body)
        return _run(self.log.admin_handler(
            self.env, request, parse_qs(query),
            '<input type="hidden" name="csrf" value="%s">' % CSRF,
            "admin=alice", "alice"))


def test_hits_group_by_site_method_and_path():
    h = Harness()
    h.hit("/docs/missing", times=3)
    h.hit("/docs/missing", method="HEAD")
    h.hit("/docs/missing", site="WWW.Forkmesh.com.")
    h.hit("/api/notes/0f8fad5b-d9cb-469f-a165-70867728950e", method="POST")
    h.hit("/api/notes/9b2c7d14-1c71-4e36-8a6e-2b6f3f0b7a11", method="POST")

    rows = h.rows()
    assert [(r["site"], r["method"], r["path"], r["hits"]) for r in rows] == [
        ("app.forkmesh.com", "GET", "/docs/missing", 4),
        ("app.forkmesh.com", "POST", "/api/notes/[id]", 2),
        ("forkmesh.com", "GET", "/docs/missing", 1),
    ]
    assert rows[0]["first_seen"] == rows[0]["last_seen"] == NOW


def test_paths_and_referrers_are_normalized():
    log = Harness().log
    env = SimpleNamespace(APP_ORIGIN="https://app.forkmesh.com",
                          WWW_ORIGIN="https://forkmesh.com")
    assert log.normalize_path("no-slash") == ""
    assert log.normalize_path("/a\x00b/c d") == "/ab/cd"
    assert log.normalize_path("/x" * 400) == ("/x" * 400)[:log.PATH_MAX]
    assert log.normalize_path("/reset/abcdefghijklmnopqrstuvwx1") == "/reset/[id]"
    assert log.normalize_path("/blog/a-very-long-post-title-without-digits") == (
        "/blog/a-very-long-post-title-without-digits")
    assert log.normalize_site("evil host") == ""
    assert log.normalize_method("BREW") == "OTHER"
    assert log.referrer_label(
        env, "https://forkmesh.com/blog/post?utm=1#x") == "forkmesh.com/blog/post"
    assert log.referrer_label(
        env, "https://news.example/item?id=42") == "news.example"
    assert log.referrer_label(env, "javascript:alert(1)") == ""
    assert [log.source_label(p) for p in (
        "/api/x", "/alice/repo.git/info/refs", "/favicon.ico", "/docs/x")] == [
        "API", "Git", "File", "Page"]


def test_the_tenth_hit_pings_every_admin_once():
    h = Harness()
    h.hit("/old-link", referrer="forkmesh.com/blog", times=9)
    assert h.pings == []

    h.hit("/old-link")
    assert sorted(p[0] for p in h.pings) == ["alice", "bob"]
    recipient, kind, title, fields = h.pings[0]
    assert kind == "operational_alert"
    assert title == "404 reached 10 hits: /old-link"
    assert "GET app.forkmesh.com/old-link has answered 404 10 times." in fields["body"]
    assert "forkmesh.com/blog" in fields["body"]
    assert fields["href"] == "/ops-console?view=not-found"
    assert fields["meta"]["hits"] == 10
    assert h.rows()[0]["alerted_at"] == NOW

    h.hit("/old-link", times=20)
    assert len(h.pings) == 2


def test_alerts_are_capped_per_hour_and_resume_after():
    h = Harness(admins=("alice",))
    for index in range(h.log.ALERTS_PER_HOUR + 1):
        h.hit("/scan-%d" % index, times=10)
    assert len(h.pings) == h.log.ALERTS_PER_HOUR
    held = [r for r in h.rows() if r["alerted_at"] == 0]
    assert [r["path"] for r in held] == ["/scan-%d" % h.log.ALERTS_PER_HOUR]

    h.now += HOUR + 1
    h.hit("/scan-%d" % h.log.ALERTS_PER_HOUR)
    assert len(h.pings) == h.log.ALERTS_PER_HOUR + 1
    assert h.pings[-1][3]["meta"]["hits"] == 11


def test_old_and_excess_groups_are_pruned():
    h = Harness()
    h.log.MAX_GROUPS = 3
    h.log.PRUNE_EVERY_NEW_GROUPS = 1
    h.hit("/ancient")
    h.now += h.log.RETAIN_MS + 1
    for index in range(4):
        h.now += 1
        h.hit("/new-%d" % index)
    assert sorted(r["path"] for r in h.rows()) == ["/new-1", "/new-2", "/new-3"]


def test_a_response_404_is_recorded_privately():
    h = Harness()
    request = FakeRequest(
        "https://app.forkmesh.com/secret-owner/hidden?token=1",
        headers={"referer": "https://app.forkmesh.com/dashboard?tab=x",
                 "cf-connecting-ip": "203.0.113.7", "cf-ipcountry": "de",
                 "user-agent": "curl/8.5\n0"})
    url = urlparse(request.url)
    assert _run(h.log.record_response(h.env, request, url))
    [row] = h.rows()
    assert row["path"] == "/private-or-unpublished-repository/page"
    assert row["referrer"] == "app.forkmesh.com/dashboard"
    assert "token" not in json.dumps(row)
    assert json.loads(row["last_client"]) == {
        "ip": "203.0.113.7", "country": "DE", "ua": "curl/8.5 0"}


def test_the_last_visitor_is_kept_sealed_and_shown():
    h = Harness()
    h.hit("/gone", client={"ip": "198.51.100.1", "country": "US",
                           "ua": "First/1"})
    h.hit("/gone", client={"ip": "2001:db8::1", "country": "FR",
                           "ua": "<b>Second</b>/2"})
    h.hit("/quiet")
    rows = {row["path"]: row for row in h.rows()}
    assert json.loads(rows["/gone"]["last_client"])["ip"] == "2001:db8::1"
    assert rows["/quiet"]["last_client"] == ""

    page = h.admin().body
    assert "Last visitor" in page
    assert "\U0001F1EB\U0001F1F7 FR" in page and "2001:db8::1" in page
    assert "&lt;b&gt;Second&lt;/b&gt;/2" in page and "<b>Second" not in page
    assert "198.51.100.1" not in page


def test_visitor_fields_are_validated():
    log = Harness().log
    request = FakeRequest("https://app.forkmesh.com/x", headers={
        "cf-connecting-ip": "not-an-ip", "cf-ipcountry": "XX",
        "user-agent": "a" * 400})
    client = log.request_client(request)
    assert client["ip"] == "" and client["country"] == ""
    assert len(client["ua"]) == log.USER_AGENT_MAX
    assert log.normalize_ip("[2001:DB8::1]") == "2001:db8::1"
    assert log.country_flag("gb") == "\U0001F1EC\U0001F1E7"
    assert log.country_flag("T1") == ""


def test_the_www_relay_records_under_the_marketing_site_only():
    h = Harness()

    def relay(payload, method="POST"):
        return _run(h.log.relay_handler(h.env, FakeRequest(
            "https://app.forkmesh.com/api/not-found", method,
            json.dumps(payload))))

    response = relay({"path": "/blog/gone", "referrer": "https://x.example/a",
                      "site": "app.forkmesh.com", "userAgent": "Browser/1"})
    assert response.status == 200
    assert relay({"path": "nope"}).status == 400
    assert relay([1, 2]).status == 400
    assert relay({}, method="GET").status == 405
    [row] = h.rows()
    assert (row["site"], row["method"], row["path"], row["referrer"]) == (
        "forkmesh.com", "GET", "/blog/gone", "x.example")
    assert json.loads(row["last_client"]) == {"ua": "Browser/1"}


def test_admin_page_lists_groups_busiest_first_and_escapes():
    h = Harness()
    h.hit("/<script>alert(1)</script>", times=2)
    h.hit("/busy", times=12)
    h.hit("/api/gone", site="world.forkmesh.com")

    response = h.admin()
    page = response.body
    assert response.status == 200
    assert response.headers["cache-control"] == "no-store"
    assert "<script>alert(1)</script>" not in page
    assert "&lt;script&gt;alert(1)&lt;/script&gt;" in page
    assert page.index("/busy") < page.index("&lt;script&gt;") < page.index(
        "/api/gone")
    assert "Alerted" in page and "9 to go" in page
    assert 'aria-current="page"' in page and ">404s<" in page

    recent = h.admin("view=not-found&sort=recent").body
    assert "Most recent" in recent


def test_clearing_a_group_is_csrf_checked_and_audited():
    h = Harness()
    h.hit("/busy", times=3)
    group = {"action": "clear_not_found", "site": "app.forkmesh.com",
             "method": "GET", "path": "/busy"}

    blocked = h.admin(method="POST", form=dict(group, csrf="wrong"))
    assert blocked.status == 303
    assert "notice=csrf" in blocked.headers["location"]
    assert len(h.rows()) == 1

    cleared = h.admin(method="POST", form=dict(group, csrf=CSRF))
    assert cleared.headers["location"].startswith(
        "/ops-console?admin=alice&view=not-found")
    assert "notice=cleared" in cleared.headers["location"]
    assert h.rows() == []
    assert h.audits == [(
        "alice", "admin.console_clear_not_found", "not_found_paths",
        "app.forkmesh.com GET /busy", "success", {"hits": 3})]

    again = h.admin(method="POST", form=dict(group, csrf=CSRF))
    assert "notice=missing" in again.headers["location"]


def test_console_dispatches_the_404_view_and_the_nav_links_it():
    runtime = (SRC / "app_runtime.py").read_text(encoding="utf-8")
    assert runtime.index("_not_found_log.VIEW") < runtime.index(
        'if method_name(request) == "POST":\n            try:\n'
        '                form = clean_hidden_query(parse_qs(')
    assert "status == 404" in runtime and "record_response" in runtime
    shell = _load("admin_shell")
    keys = [key for _group, items in shell.NAV for key, *_rest in items]
    assert keys.index("not-found") == keys.index("errors") + 1


def _hours(h):
    return [tuple(r) for r in h.db.execute(
        "SELECT path, hour, hits FROM not_found_hours ORDER BY path, hour")]


def test_hits_are_counted_by_hour_and_old_hours_pruned():
    h = Harness()
    hour = NOW // HOUR
    h.hit("/gone", times=2)
    h.now += HOUR
    h.hit("/gone")
    h.hit("/other")
    assert _hours(h) == [
        ("/gone", hour, 2), ("/gone", hour + 1, 1), ("/other", hour + 1, 1)]

    h.now += h.log.HOURS_RETAINED * HOUR
    h.hit("/later")
    assert _hours(h) == [
        ("/later", hour + 1 + h.log.HOURS_RETAINED, 1)]


def test_admin_page_charts_the_last_24_hours_per_group():
    h = Harness()
    h.now -= 30 * HOUR
    h.hit("/old-busy", times=40)
    h.now = NOW - 3 * HOUR
    h.hit("/fresh", times=4)
    h.now = NOW
    h.hit("/fresh", times=2)

    page = h.admin().body
    assert "Previous 24 hours" in page and "6 hits by hour" in page
    assert 'class="hours"' in page
    assert "current hour: 2 hits" in page and "3 hours ago: 4 hits" in page
    assert "24-hour frequency: 6 hits" in page
    assert "24-hour frequency: 0 hits" in page
    assert page.index("/old-busy") < page.index("/fresh")

    today = h.admin("view=not-found&sort=today").body
    assert "Busiest today" in today
    assert today.index("/fresh") < today.index("/old-busy")

    cleared = h.admin(method="POST", form={
        "action": "clear_not_found", "site": "app.forkmesh.com",
        "method": "GET", "path": "/fresh", "csrf": CSRF})
    assert "notice=cleared" in cleared.headers["location"]
    assert [row[0] for row in _hours(h)] == ["/old-busy"]


def test_an_exception_stops_logging_a_deleted_path():
    h = Harness()
    h.hit("/blog/removed-post", times=3)
    h.hit("/blog/removed-post", method="POST")
    h.hit("/blog/removed-post", site="forkmesh.com")
    rule = {"action": "ignore_not_found", "site": "app.forkmesh.com",
            "path": "/blog/removed-post"}

    blocked = h.admin(method="POST", form=dict(rule, csrf="wrong"))
    assert "notice=csrf" in blocked.headers["location"]
    assert len(h.rows()) == 3

    added = h.admin(method="POST", form=dict(rule, csrf=CSRF))
    assert "notice=ignored" in added.headers["location"]
    assert [(r["site"], r["method"]) for r in h.rows()] == [
        ("forkmesh.com", "GET")]
    assert _hours(h) == [("/blog/removed-post", NOW // HOUR, 1)]
    assert h.audits[-1] == (
        "alice", "admin.console_ignore_not_found", "not_found_ignores",
        "app.forkmesh.com /blog/removed-post", "success",
        {"prefix": False, "groups": 2, "hits": 4})

    h.now += HOUR
    assert not _run(h.log.record(
        h.env, "app.forkmesh.com", "GET", "/blog/removed-post", "",
        now=h.now))
    h.hit("/blog/removed-post/comments")
    [suppressed] = h.db.execute(
        "SELECT hits, last_seen FROM not_found_ignores").fetchall()
    assert tuple(suppressed) == (1, h.now)
    page = h.admin().body
    assert "Exceptions" in page and "Remove" in page
    assert "/blog/removed-post/comments" in page

    removed = h.admin(method="POST", form={
        "action": "unignore_not_found", "site": "app.forkmesh.com",
        "path": "/blog/removed-post", "csrf": CSRF})
    assert "notice=unignored" in removed.headers["location"]
    assert h.audits[-1][1:] == (
        "admin.console_unignore_not_found", "not_found_ignores",
        "app.forkmesh.com /blog/removed-post", "success", {"suppressed": 1})
    h.hit("/blog/removed-post")
    assert "notice=rule_missing" in h.admin(method="POST", form={
        "action": "unignore_not_found", "site": "app.forkmesh.com",
        "path": "/blog/removed-post", "csrf": CSRF}).headers["location"]


def test_a_prefix_exception_covers_everything_under_it():
    h = Harness()
    h.hit("/alice/old-repo/blob/main/README.md")
    h.hit("/alice/old-repo")
    h.hit("/alice/old-repo-two")

    def add(path, prefix="1", site="app.forkmesh.com"):
        form = {"action": "ignore_not_found", "site": site, "path": path,
                "csrf": CSRF}
        if prefix:
            form["prefix"] = prefix
        return h.admin(method="POST", form=form).headers["location"]

    assert "notice=rule_invalid" in add("/")
    assert "notice=rule_invalid" in add("relative")
    assert "notice=rule_invalid" in add("/x", site="bad host")
    assert "notice=ignored" in add("/alice/old-repo/")
    assert [r["path"] for r in h.rows()] == ["/alice/old-repo-two"]
    assert [tuple(r) for r in h.db.execute(
        "SELECT path, prefix FROM not_found_ignores")] == [
        ("/alice/old-repo", 1)]

    h.hit("/alice/old-repo-two")
    assert not _run(h.log.record(
        h.env, "app.forkmesh.com", "GET", "/alice/old-repo/issues/4", "",
        now=h.now))
    assert [(r["path"], r["hits"]) for r in h.rows()] == [
        ("/alice/old-repo-two", 2)]
    assert "and below" in h.admin().body


def test_exceptions_are_capped_and_seen_by_other_processes_within_a_minute():
    h = Harness()
    other = Harness()
    other.db = h.db

    def other_records(path, now):
        return _run(other.log.record(
            other.env, "app.forkmesh.com", "GET", path, "", now=now))

    h.log.MAX_IGNORES = 1
    form = {"action": "ignore_not_found", "site": "app.forkmesh.com",
            "csrf": CSRF}
    h.hit("/a")
    assert other_records("/b", NOW)
    assert "notice=ignored" in h.admin(
        method="POST", form=dict(form, path="/b")).headers["location"]
    assert "notice=rule_full" in h.admin(
        method="POST", form=dict(form, path="/a")).headers["location"]
    assert "notice=ignored" in h.admin(
        method="POST", form=dict(form, path="/b", prefix="1")).headers[
        "location"]
    assert not _run(h.log.record(
        h.env, "app.forkmesh.com", "GET", "/b/c", "", now=h.now))

    assert other_records("/b", NOW + 1000)
    assert not other_records("/b", NOW + other.log.IGNORE_CACHE_MS)
