#!/usr/bin/env python3
"""Repo tabs badge inbox items still awaiting the owner node's sync.

Signed submissions (issues/PRs/discussions/commit comments) sit encrypted in
the relay's per-repo inbox until the owner's desktop node drains them into
git. The website's tab counts only reflect the served mirror, so a fresh
submission was invisible; a content-free /pending endpoint now feeds amber
"+N pending" badges on the matching tabs.
"""

import ast
import asyncio
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "forkbot.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "fediverse_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "app_runtime.py").read_text(encoding="utf-8")
    + "\n\n\n"
)
URLS_TEXT = (ROOT / "src" / "urls.py").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()

FUNCS = {"clean_string", "method_name", "repo_pending_counts_handler"}


def _load():
    tree = ast.parse(
        CATALOG.read_text(encoding="utf-8") + "\n" + ENTRY_TEXT,
        filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {node.name for node in selected}
    assert found == FUNCS, "missing: %s" % sorted(FUNCS - found)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    ns = {}
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns


class _Request:
    method = "GET"
    url = "https://forkmesh.test/api/repo/o/r/pending"


def test_pending_endpoint_counts_all_three_collaboration_inboxes_in_one_round_trip():
    queries = []

    async def noop(*_a, **_k):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def d1_all(_env, sql, *args):
        queries.append(sql)
        assert args == ("bi:o/r",) * 3
        return [
            {"k": "issues", "c": 3},
            {"k": "pulls", "c": 0},
            {"k": "discussions", "c": 1},
        ]

    captured = {}

    def json_response(
            payload, status=200, cache_seconds=None, cache_control=None, **_kw):
        captured.update(payload)
        captured["_cache"] = cache_seconds
        captured["_cache_control"] = cache_control
        return {"status": status, "data": payload}

    # The badge must tally the SAME blind index the drains read, so the handler
    # resolves an organization alias to its backing node first (a no-op for the
    # plain node owner used here).
    async def ap_org_alias_owner(_env, owner, _repo):
        return owner

    edge = {"puts": [], "gets": []}

    async def edge_cache_match(key):
        edge["gets"].append(key)
        return None

    async def edge_cache_put(key, resp):
        edge["puts"].append(key)

    ns = _load()
    ns.update({
        "ensure_schema": noop, "blind_index": blind_index,
        "d1_all": d1_all, "json_response": json_response,
        "_ap_org_alias_owner": ap_org_alias_owner,
        "edge_cache_match": edge_cache_match,
        "edge_cache_put": edge_cache_put,
        "quote": lambda value: str(value),
    })
    resp = asyncio.run(
        ns["repo_pending_counts_handler"](object(), _Request(), "o", "r"))
    assert resp["status"] == 200
    assert captured["pending"] == {
        "issues": 3, "pulls": 0, "discussions": 1}
    # One UNION statement, not three queries.
    assert len(queries) == 1
    assert queries[0].count("UNION ALL") == 2
    # The whole fleet polls these badges, so the public 200 is edge-cached
    # for ten minutes (2026-08-06: repo/*/*/pending was the single largest
    # request group on the App API traffic chart). A 30s-stale badge
    # is an accepted trade; the private 404 path stays uncached.
    assert captured["_cache_control"] == "public, max-age=600"
    assert edge["gets"] and edge["puts"]
    assert edge["gets"][0] == edge["puts"][0]
    assert "/repo-pending/o/r" in edge["puts"][0]


def test_pending_route_is_wired():
    assert 'REPO_PENDING_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/pending$")' in URLS_TEXT
    assert "pending_match = REPO_PENDING_RE.match(url.path)" in ENTRY_TEXT
    assert "await repo_pending_counts_handler(self.env, request, owner, repo)" in ENTRY_TEXT


def test_dashboard_never_labels_numbered_collaboration_writes_pending():
    # The endpoint remains for older clients, but current web writes receive a
    # number in the POST response and render as normal issue/PR records.
    assert 'data-dashboard-repo-tab-pending="${tab}"' not in DASHBOARD_JS
    assert "function setRepoTabPending(tab, count)" not in DASHBOARD_JS
    assert "async function loadRepoPendingCounts(repo)" not in DASHBOARD_JS
    assert "Pending issue submission" not in DASHBOARD_JS
    assert '`+${formatCount(n)} pending`' not in DASHBOARD_JS
    assert "state.repoOptimisticRecords" in DASHBOARD_JS
    assert 'repoRecordUrl(repo, "issues", number, "conversation")' in DASHBOARD_JS
    assert 'repoRecordUrl(targetRepo, "pulls", number, "conversation")' in DASHBOARD_JS
