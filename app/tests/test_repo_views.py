#!/usr/bin/env python3

import ast
import asyncio
import re
import sqlite3
import sys
from pathlib import Path
from urllib.parse import unquote

from worker_test_helpers import json_from_request_double


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import urls  # noqa: E402
import instance_privacy  # noqa: E402

ENTRY = ROOT / "src" / "entry.py"
CATALOG = ROOT / "src" / "catalog.py"
SOURCE = ENTRY.read_text(encoding="utf-8") + "\n" + CATALOG.read_text(encoding="utf-8")
DASHBOARD_JS = ROOT / "public" / "dashboard" / "js"

FUNCS = {
    "repo_view_handler", "repo_views_lookup_handler", "_record_repo_view",
    "_repo_viewer_key", "_repo_view_totals", "_repo_view_lookup_keys",
    "_public_repo_owner", "method_name", "safe_segment",
    "_visitor_count_address",
}
CONSTANTS = {
    "REPO_VIEW_WINDOW_MS", "REPO_VIEW_LOOKUP_LIMIT", "REPO_VIEW_LOOKUP_CHUNK",
}
DAY_MS = 24 * 60 * 60 * 1000


def _load(extra):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name in FUNCS)
        or (isinstance(node, ast.Assign) and any(
            isinstance(t, ast.Name) and t.id in CONSTANTS for t in node.targets))
    ]
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    missing = (FUNCS | CONSTANTS) - set(namespace)
    assert not missing, sorted(missing)
    return namespace


class _Headers:
    def __init__(self, mapping=None):
        self._m = {str(k).lower(): v for k, v in (mapping or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method, url, body=None, headers=None):
        self.method = method
        self.url = url
        self._body = body
        self.headers = _Headers(headers)

    async def json(self):
        if self._body is None:
            raise ValueError("no body")
        return self._body


class _Visitors:
    @staticmethod
    def canonical_edge_address(value):
        return str(value or "").strip()

    @staticmethod
    def generalized_user_agent(value):
        return "browser" if value else "unknown"


def _harness(private=()):
    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row
    conn.executescript(
        (ROOT / "migrations" / "0234_repo_views.sql").read_text().split(
            "INSERT INTO schema_meta")[0])
    conn.execute(
        "CREATE TABLE repositories (key_bi TEXT PRIMARY KEY, "
        "is_private INTEGER NOT NULL DEFAULT 0, data TEXT)")
    for key in ("alice/proj", "alice/other", "bob/secret"):
        conn.execute(
            "INSERT INTO repositories (key_bi,is_private) VALUES (?,?)",
            ("bi:" + key, 1 if key in private else 0))
    now = [10 * DAY_MS]

    class _Date:
        @staticmethod
        def now():
            return now[0]

    async def d1_first(_env, sql, *args):
        row = conn.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in conn.execute(sql, args).fetchall()]

    async def d1_run(_env, sql, *args):
        conn.execute(sql, args)

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def _account_session_record(_env, request, data=None):
        token = str((data or {}).get("sessionToken") or "")
        if token.startswith("session:"):
            name = token[len("session:"):]
            return "bi:" + name, {"name": name}
        return "", None

    async def _repo_is_private(_env, owner, repo):
        key = "%s/%s" % (owner, repo)
        return key in private or key not in ("alice/proj", "alice/other", "bob/secret")

    async def _org_repo_node(_env, _owner, _repo):
        return ""

    async def ensure_schema(_env):
        return None

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    ns = _load({
        "Date": _Date,
        "d1_first": d1_first,
        "d1_all": d1_all,
        "d1_run": d1_run,
        "blind_index": blind_index,
        "_account_session_record": _account_session_record,
        "_repo_is_private": _repo_is_private,
        "_org_repo_node": _org_repo_node,
        "ensure_schema": ensure_schema,
        "json_response": json_response,
        "bounded_json_request": json_from_request_double,
        "world_visitor_metrics": _Visitors,
        "instance_privacy": instance_privacy,
        "REPO_VIEW_RE": urls.REPO_VIEW_RE,
        "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._-]+$"),
        "MAX_REPO_SEGMENT": 100,
        "unquote": unquote,
        "urlparse": __import__("urllib.parse").parse.urlparse,
    })
    ns["_now"] = now
    ns["_conn"] = conn
    return ns


def _view(ns, owner="alice", repo="proj", session="", ip="203.0.113.5"):
    body = {"sessionToken": session} if session else {}
    headers = {"cf-connecting-ip": ip, "user-agent": "Mozilla/5.0"} if ip else {}
    request = _Request(
        "POST", "https://forkmesh.test/api/repo/%s/%s/view" % (owner, repo),
        body=body, headers=headers)
    return asyncio.run(ns["repo_view_handler"](object(), request, owner, repo))


def _lookup(ns, repos):
    request = _Request(
        "POST", "https://forkmesh.test/api/repo-views", body={"repos": repos})
    return asyncio.run(ns["repo_views_lookup_handler"](object(), request))


def test_routes_match_the_view_paths():
    assert urls.REPO_VIEW_RE.match("/api/repo/alice/proj/view").groups() == (
        "alice", "proj")
    assert urls.REPO_VIEWS_RE.match("/api/repo-views")
    assert not urls.REPO_VIEW_RE.match("/api/repo/alice/proj/views")


def test_a_reload_by_the_same_viewer_is_not_another_unique_view():
    ns = _harness()
    first = _view(ns)["data"]
    assert first == {"ok": True, "counted": True, "views": 1, "viewers": 1,
                     "lastViewedAt": 10 * DAY_MS}
    ns["_now"][0] += 60_000
    again = _view(ns)["data"]
    assert again["counted"] is False
    assert again["views"] == 1 and again["viewers"] == 1
    assert again["lastViewedAt"] == 10 * DAY_MS
    hits = ns["_conn"].execute("SELECT hits FROM repo_views").fetchone()[0]
    assert hits == 2


def test_a_returning_viewer_counts_again_after_a_day_but_stays_one_viewer():
    ns = _harness()
    _view(ns)
    ns["_now"][0] += DAY_MS
    data = _view(ns)["data"]
    assert data["counted"] is True
    assert data["views"] == 2 and data["viewers"] == 1
    assert data["lastViewedAt"] == 11 * DAY_MS


def test_distinct_viewers_each_count_once():
    ns = _harness()
    _view(ns, ip="203.0.113.5")
    _view(ns, ip="198.51.100.7")
    _view(ns, ip="", session="session:carol")
    data = _view(ns, ip="203.0.113.9", session="session:carol")["data"]
    assert data["views"] == 3 and data["viewers"] == 3


def test_a_request_with_no_identity_is_not_counted():
    ns = _harness()
    data = _view(ns, ip="")["data"]
    assert data["counted"] is False and data["views"] == 0


def test_private_and_unknown_repositories_are_not_tracked():
    ns = _harness(private=("bob/secret",))
    assert _view(ns, owner="bob", repo="secret")["status"] == 404
    assert _view(ns, owner="nobody", repo="none")["status"] == 404
    assert ns["_conn"].execute("SELECT COUNT(*) FROM repo_views").fetchone()[0] == 0


def test_lookup_returns_totals_for_known_public_keys_only():
    ns = _harness()
    _view(ns, repo="proj")
    ns["_now"][0] += 5_000
    _view(ns, repo="other", ip="198.51.100.7")
    ns["_conn"].execute(
        "INSERT INTO repo_view_totals (repo_bi,views,viewers,last_viewed_at) "
        "VALUES ('bi:bob/secret',4,4,1)")
    ns["_conn"].execute(
        "UPDATE repositories SET is_private=1 WHERE key_bi='bi:bob/secret'")
    data = _lookup(ns, ["Alice/Proj", "alice/other", "alice/other",
                        "bob/secret", "bad", "x/y/z", 7])["data"]
    assert data["ok"] is True
    assert set(data["views"]) == {"alice/proj", "alice/other"}
    assert data["views"]["alice/other"]["lastViewedAt"] > (
        data["views"]["alice/proj"]["lastViewedAt"])


def test_lookup_caps_and_chunks_the_keys():
    ns = _harness()
    keys = ns["_repo_view_lookup_keys"](
        ["o/r%d" % i for i in range(500)])
    assert len(keys) == ns["REPO_VIEW_LOOKUP_LIMIT"]
    assert _lookup(ns, ["o/r%d" % i for i in range(500)])["data"] == {
        "ok": True, "views": {}}


def test_home_sorts_top_repositories_by_most_recent_unique_view():
    explorer = (DASHBOARD_JS / "05-repo-list-explorer.js").read_text("utf-8")
    entries = explorer[explorer.index("function homeRepositoryEntries()"):]
    entries = entries[:entries.index("function homeRepositoryRow(")]
    assert ".sort((a, b) => homeRepositoryLastViewedAt(b) - homeRepositoryLastViewedAt(a))" in entries
    render = explorer[explorer.index("function renderHomeRepositories()"):]
    assert render.index("refreshHomeRepositoryViews(all)") < render.index(
        "HOME_TOP_REPOSITORY_LIMIT")
    assert 'fetch("/api/repo-views"' in explorer
    detail = (DASHBOARD_JS / "08-repo-detail-network.js").read_text("utf-8")
    assert "recordRepositoryView(repo);" in detail
    content = (DASHBOARD_JS / "06-repo-content.js").read_text("utf-8")
    assert "`${repoApiBase(repo)}/view`" in content
