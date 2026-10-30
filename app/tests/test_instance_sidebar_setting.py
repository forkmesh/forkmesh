"""Settings → Top repositories: the admin form behind the "sidebar" instance setting."""

import asyncio
import json
import re
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import app_runtime  # noqa: E402
import instance_sidebar  # noqa: E402
import schema  # noqa: E402

PUBLIC = ROOT / "public"


@pytest.fixture
def rt(monkeypatch):
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    for statement in schema.SCHEMA_STATEMENTS:
        if re.search(r"CREATE TABLE IF NOT EXISTS instance_settings\b", statement):
            db.execute(statement)
    audits = []

    async def d1_first(env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(env, sql, *args):
        db.execute(sql, args)

    async def session(env, request, data=None):
        token = (data or {}).get("sessionToken") or request.bearer
        return ("bi", {"name": token}) if token else ("", None)

    async def is_admin(env, name):
        return name == "alice"

    async def body(request, max_bytes=None):
        if request.body is None:
            raise ValueError("invalid json")
        return request.body

    async def audit(env, actor, action, *rest):
        audits.append((actor, action, rest[-1]))

    def json_response(data, status=200, cache_control=None, **_):
        return SimpleNamespace(status=status, data=data, cache_control=cache_control)

    for name, value in {
        "d1_first": d1_first, "d1_run": d1_run,
        "_account_session_record": session, "_is_admin": is_admin,
        "bounded_json_request": body, "json_response": json_response,
        "_audit_sensitive_action": audit, "instance_sidebar": instance_sidebar,
        "method_name": lambda request: request.method,
        "Date": SimpleNamespace(now=lambda: 1_000), "json": json,
    }.items():
        monkeypatch.setattr(app_runtime, name, value, raising=False)
    runtime = SimpleNamespace(env=SimpleNamespace())
    runtime._instance_sidebar_setting = (
        lambda: app_runtime.Default._instance_sidebar_setting(runtime))
    return SimpleNamespace(db=db, runtime=runtime, audits=audits)


def call(rt, method="GET", account="", sidebar=None, body=...):
    if body is ...:
        body = {"sessionToken": account, "sidebar": sidebar} if method == "POST" else None
    request = SimpleNamespace(method=method, bearer=account, body=body)
    return asyncio.run(app_runtime.Default._instance_sidebar_route(rt.runtime, request))


def stored(rt):
    row = rt.db.execute("SELECT doc FROM instance_settings WHERE k='sidebar'").fetchone()
    return json.loads(row[0]) if row else None


def test_only_an_admin_reads_or_writes_the_setting(rt):
    assert call(rt).status == 401
    assert call(rt, account="bob").status == 403
    assert call(rt, "POST", "bob", {"pinned": ["widgets"], "forks": 5}).status == 403
    assert call(rt, "POST", "", {"pinned": ["widgets"], "forks": 5}).status == 401
    assert stored(rt) is None
    read = call(rt, account="alice")
    assert read.data == {"ok": True, "sidebar": {"pinned": [], "forks": 3},
                         "limits": {"pinned": 10, "forks": 50}}
    assert read.cache_control == "no-store"


def test_a_valid_write_is_stored_normalized_and_audited(rt):
    reply = call(rt, "POST", "alice", {"pinned": [" Widgets ", "acme/tools", "widgets", ""], "forks": 20})
    assert reply.status == 200
    assert reply.data["sidebar"] == {"pinned": ["widgets", "acme/tools"], "forks": 20}
    assert stored(rt) == reply.data["sidebar"]
    assert call(rt, account="alice").data["sidebar"] == reply.data["sidebar"]
    assert rt.audits == [("alice", "instance.sidebar", reply.data["sidebar"])]
    cleared = call(rt, "POST", "alice", {"pinned": [], "forks": 0})
    assert stored(rt) == cleared.data["sidebar"] == {"pinned": [], "forks": 0}


@pytest.mark.parametrize("sidebar, error", [
    (None, "invalid_sidebar"),
    ({"pinned": "widgets", "forks": 3}, "invalid_pinned"),
    ({"pinned": ["a/b/c"], "forks": 3}, "invalid_pinned"),
    ({"pinned": ["bad name"], "forks": 3}, "invalid_pinned"),
    ({"pinned": ["<script>"], "forks": 3}, "invalid_pinned"),
    ({"pinned": [7], "forks": 3}, "invalid_pinned"),
    ({"pinned": ["r%d" % n for n in range(11)], "forks": 3}, "too_many_pinned"),
    ({"pinned": [], "forks": 51}, "invalid_forks"),
    ({"pinned": [], "forks": -1}, "invalid_forks"),
    ({"pinned": [], "forks": "5"}, "invalid_forks"),
    ({"pinned": [], "forks": True}, "invalid_forks"),
    ({"pinned": [], "forks": 2.5}, "invalid_forks"),
])
def test_an_invalid_write_is_refused_and_changes_nothing(rt, sidebar, error):
    call(rt, "POST", "alice", {"pinned": ["widgets"], "forks": 20})
    reply = call(rt, "POST", "alice", sidebar)
    assert (reply.status, reply.data) == (400, {"error": error})
    assert stored(rt) == {"pinned": ["widgets"], "forks": 20}


def test_bad_json_is_refused(rt):
    assert call(rt, "POST", "alice", body=None).data == {"error": "invalid_json"}


def test_reading_a_hand_edited_row_stays_lenient():
    assert instance_sidebar.parse({"pinned": ["Widgets", "widgets", "x"] + ["r%d" % n for n in range(20)],
                                   "forks": 999}) == {
        "pinned": ["widgets", "x"] + ["r%d" % n for n in range(8)], "forks": 50}
    assert instance_sidebar.parse("junk") == {"pinned": [], "forks": 3}
    assert instance_sidebar.parse({"pinned": "widgets", "forks": "x"}) == {"pinned": [], "forks": 3}


def test_the_route_and_the_settings_section_are_wired():
    runtime = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
    assert 'url.path == "/api/instance/sidebar"' in runtime
    assert '"sidebar": await self._instance_sidebar_setting()' in runtime
    view = (PUBLIC / "dashboard" / "partials" / "views" / "settings.html").read_text(encoding="utf-8")
    nav = view.split("data-instance-theme-nav", 1)[1].split("</div>", 1)[0]
    assert 'data-settings-section-link="instance-sidebar"' in nav
    assert 'data-settings-section="instance-sidebar"' in view
    account = (PUBLIC / "dashboard" / "js" / "04-account.js").read_text(encoding="utf-8")
    assert '"instance-theme", "instance-sidebar"' in account
    assert 'if (activeSection === "instance-sidebar") initInstanceSidebarSection();' in account
    bundle = (PUBLIC / "dashboard.js").read_text(encoding="utf-8")
    assert "async function initInstanceSidebarSection()" in bundle
    page = (PUBLIC / "dashboard" / "settings" / "index.html").read_text(encoding="utf-8")
    assert "data-instance-sidebar-form" in page
