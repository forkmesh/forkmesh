import ast
import asyncio
import json
import re
import sqlite3
import subprocess
import sys
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

from worker_test_helpers import json_from_request_double


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

import urls  # noqa: E402

ENTRY = ROOT / "src" / "entry.py"
SCHEMA = ROOT / "src" / "schema.py"
RUNTIME = ROOT / "src" / "app_runtime.py"
CONTENT = ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
HELPERS = ROOT / "public" / "dashboard" / "js" / "02-helpers.js"
MIGRATION = ROOT / "migrations" / "0287_issue_views.sql"
DOWN = ROOT / "migrations" / "down" / "0287_issue_views.sql"
DOC = ROOT.parent / "docs" / "app" / "issue-views.md"

FUNCS = {
    "issue_view_handler", "issue_views_lookup_handler",
    "_record_issue_view", "_issue_view_automated", "_issue_view_numbers",
    "_repo_viewer_key", "_repo_view_totals", "_public_repo_owner",
    "method_name", "safe_segment",
}
CONSTANTS = {
    "REPO_VIEW_WINDOW_MS", "REPO_VIEW_LOOKUP_CHUNK", "ISSUE_VIEW_LOOKUP_LIMIT",
    "ISSUE_VIEW_AUTOMATED_AGENTS",
}
DAY_MS = 24 * 60 * 60 * 1000
BROWSER = "Mozilla/5.0 (X11; Linux x86_64) Firefox/140.0"


def _load(extra):
    source = ENTRY.read_text(encoding="utf-8") + "\n" + (
        ROOT / "src" / "catalog.py").read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(ENTRY))
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
    conn.executescript(MIGRATION.read_text().split("INSERT INTO schema_meta")[0])
    known = ("alice/proj", "alice/other", "bob/secret")
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
        return key in private or key not in known

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
        "REPO_ISSUE_VIEW_RE": urls.REPO_ISSUE_VIEW_RE,
        "REPO_ISSUE_VIEWS_RE": urls.REPO_ISSUE_VIEWS_RE,
        "ROOM_NAME_RE": re.compile(r"^[A-Za-z0-9._-]+$"),
        "MAX_REPO_SEGMENT": 100,
        "unquote": unquote,
        "urlparse": urlparse,
        "parse_qs": parse_qs,
    })
    ns["_now"] = now
    ns["_conn"] = conn
    return ns


def _view(ns, number=7, owner="alice", repo="proj", session="",
          ip="203.0.113.5", agent=BROWSER, method="POST"):
    body = {"sessionToken": session} if session else {}
    headers = {"cf-connecting-ip": ip} if ip else {}
    if agent:
        headers["user-agent"] = agent
    request = _Request(
        method, "https://code.example.test/api/repo/%s/%s/issues/%d/view"
        % (owner, repo, number), body=body, headers=headers)
    return asyncio.run(ns["issue_view_handler"](
        object(), request, owner, repo, number))


def _lookup(ns, query, owner="alice", repo="proj"):
    request = _Request(
        "GET", "https://code.example.test/api/repo/%s/%s/issue-views?%s"
        % (owner, repo, query))
    return asyncio.run(ns["issue_views_lookup_handler"](
        object(), request, owner, repo))


def test_routes_match_the_issue_view_paths():
    match = urls.REPO_ISSUE_VIEW_RE.match("/api/repo/alice/proj/issues/12/view")
    assert match.groups() == ("alice", "proj", "12")
    assert not urls.REPO_ISSUE_VIEW_RE.match("/api/repo/alice/proj/issues/0/view")
    assert not urls.REPO_ISSUE_VIEW_RE.match("/api/repo/alice/proj/issues/x/view")
    assert urls.REPO_ISSUE_VIEWS_RE.match("/api/repo/alice/proj/issue-views")
    assert not urls.REPO_ISSUES_RE.match("/api/repo/alice/proj/issues/12/view")
    runtime = RUNTIME.read_text(encoding="utf-8")
    assert "REPO_ISSUE_VIEW_RE.match(url.path)" in runtime
    assert "issue_views_lookup_handler(" in runtime


def test_a_reload_by_the_same_viewer_is_not_another_view():
    ns = _harness()
    first = _view(ns)["data"]
    assert first == {"ok": True, "counted": True, "number": 7, "views": 1,
                     "viewers": 1, "lastViewedAt": 10 * DAY_MS}
    ns["_now"][0] += 60_000
    again = _view(ns)["data"]
    assert again["counted"] is False and again["views"] == 1
    assert ns["_conn"].execute("SELECT hits FROM issue_views").fetchone()[0] == 2
    ns["_now"][0] += DAY_MS
    later = _view(ns)["data"]
    assert later["counted"] is True
    assert later["views"] == 2 and later["viewers"] == 1


def test_each_issue_counts_separately_and_viewers_are_not_linkable():
    ns = _harness()
    _view(ns, number=7)
    _view(ns, number=8)
    _view(ns, number=8, ip="", session="session:carol")
    totals = dict(ns["_conn"].execute(
        "SELECT number, views FROM issue_view_totals").fetchall())
    assert totals == {7: 1, 8: 2}
    keys = [row[0] for row in ns["_conn"].execute(
        "SELECT viewer_key FROM issue_views WHERE viewer_key LIKE '%203.0.113.5%'")]
    assert len(keys) == 2 and keys[0] != keys[1]
    assert all("issue-view-v1" in key for key in keys)


def test_automated_and_anonymous_clients_are_not_counted():
    ns = _harness()
    for agent in ("", "ExampleBot/2.1 (+https://example.com/bot)",
                  "curl/8.5.0", "python-requests/2.32", "Go-http-client/1.1",
                  "Mozilla/5.0 HeadlessChrome/120.0"):
        data = _view(ns, agent=agent)["data"]
        assert data["counted"] is False and data["views"] == 0, agent
    assert _view(ns, ip="")["data"]["counted"] is False
    assert ns["_conn"].execute("SELECT COUNT(*) FROM issue_views").fetchone()[0] == 0


def test_get_reads_without_counting():
    ns = _harness()
    data = _view(ns, method="GET")["data"]
    assert data["counted"] is False and data["views"] == 0
    _view(ns)
    assert _view(ns, method="GET")["data"]["views"] == 1


def test_private_and_unknown_repositories_are_not_tracked():
    ns = _harness(private=("bob/secret",))
    assert _view(ns, owner="bob", repo="secret")["status"] == 404
    assert _view(ns, owner="nobody", repo="none")["status"] == 404
    assert _lookup(ns, "numbers=1", owner="bob", repo="secret")["status"] == 404
    assert ns["_conn"].execute("SELECT COUNT(*) FROM issue_views").fetchone()[0] == 0


def test_lookup_returns_known_issues_and_caps_numbers():
    ns = _harness()
    _view(ns, number=3)
    _view(ns, number=5)
    _view(ns, number=5, ip="198.51.100.7")
    data = _lookup(ns, "numbers=3,5,9,x,0,-1,3")["data"]
    assert data["ok"] is True
    assert set(data["views"]) == {"3", "5"}
    assert data["views"]["5"]["views"] == 2
    numbers = ns["_issue_view_numbers"]([",".join(str(n) for n in range(1, 400))])
    assert len(numbers) == ns["ISSUE_VIEW_LOOKUP_LIMIT"]
    assert _lookup(ns, "numbers=" + ",".join(str(n) for n in range(1, 400)))[
        "data"]["views"] == {"3": data["views"]["3"], "5": data["views"]["5"]}


def test_schema_migration_down_and_purge_agree():
    schema = SCHEMA.read_text(encoding="utf-8")
    assert "CREATE TABLE IF NOT EXISTS issue_views (" in schema
    assert "CREATE TABLE IF NOT EXISTS issue_view_totals (" in schema
    conn = sqlite3.connect(":memory:")
    conn.execute("CREATE TABLE schema_meta (k TEXT PRIMARY KEY, v TEXT)")
    conn.executescript(MIGRATION.read_text())
    conn.executescript(DOWN.read_text())
    tables = {row[0] for row in conn.execute(
        "SELECT name FROM sqlite_master WHERE type='table'")}
    assert tables == {"schema_meta"}
    assert conn.execute("SELECT COUNT(*) FROM schema_meta").fetchone()[0] == 0
    entry = ENTRY.read_text(encoding="utf-8")
    assert '("issue_views", "repo_bi"),' in entry
    assert '("issue_view_totals", "repo_bi"),' in entry


def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
    start = match.start()
    cursor = text.index("{", text.index(")", start))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[start:cursor + 1]
        cursor += 1


def _node(body):
    content = CONTENT.read_text(encoding="utf-8")
    helpers = HELPERS.read_text(encoding="utf-8")
    script = """
const state = { issueViewTotals: {}, recordedIssueViews: new Set(), session: null };
const posted = [];
const nodes = [];
globalThis.document = { querySelectorAll() { return nodes; } };
globalThis.fetch = async (url, init) => {
  posted.push({ url, method: init?.method || "GET" });
  return { ok: true, json: async () => ({ ok: true, counted: true, views: 3, viewers: 2, lastViewedAt: 1 }) };
};
function repoPublicOwner(repo) { return repo.owner; }
function repoApiBase(repo) { return `/api/repo/${repo.owner}/${repo.name}`; }
let countFormatter = null;
%s
%s
%s
""" % (
        "\n".join(_function(helpers, name) for name in (
            "escapeHtml", "formatCount", "repositoryViewKey")),
        "\n".join(_function(content, name) for name in (
            "issueViewKey", "issueViewTotals", "issueViewsLabel", "issueViewsHtml",
            "issueRowViewsHtml", "showIssueViews", "recordIssueView")),
        body,
    )
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout)


def test_issue_page_records_one_view_and_shows_the_count():
    out = _node("""
const repo = { owner: "Alice", name: "proj" };
const before = issueViewsHtml(repo, 7);
recordIssueView(repo, 7);
recordIssueView(repo, 7);
recordIssueView({ owner: "alice", name: "secret", isPrivate: true }, 7);
await new Promise((resolve) => setTimeout(resolve, 0));
console.log(JSON.stringify({
  before, posted,
  after: issueViewsHtml(repo, 7),
  row: issueRowViewsHtml(repo, 7),
  single: issueViewsLabel({ views: 1 }),
  privateHtml: issueViewsHtml({ owner: "a", name: "b", isPrivate: true }, 7),
}));
""")
    assert 'data-repo-issue-views="7"' in out["before"] and " hidden" in out["before"]
    assert out["posted"] == [{"url": "/api/repo/Alice/proj/issues/7/view", "method": "POST"}]
    assert "3 views" in out["after"] and " hidden>" not in out["after"]
    assert "3 views" in out["row"]
    assert out["single"] == "1 view"
    assert out["privateHtml"] == ""


def test_dashboard_wiring_and_docs():
    content = CONTENT.read_text(encoding="utf-8")
    assert 'if (kind === "issues") recordIssueView(repo, number);' in content
    assert "${issueViewsHtml(repo, number)}" in content
    assert "${issueRowViewsHtml(state.selectedRepo, item.number)}" in content
    assert "loadIssueListViews(repo, numbers)" in content
    doc = DOC.read_text(encoding="utf-8")
    for needle in ("0287_issue_views.sql", "issue-view-v1",
                   "/api/repo/<owner>/<repo>/issues/<number>/view",
                   "ISSUE_VIEW_AUTOMATED_AGENTS"):
        assert needle in doc
