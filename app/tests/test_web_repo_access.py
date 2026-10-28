#!/usr/bin/env python3

import ast
import asyncio
import re
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
HOSTED = (SRC / "hosted_git.py").read_text(encoding="utf-8")
ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()


def _run(coro):
    return asyncio.run(coro)


async def _value(value):
    return value


def _lift(text, names, namespace):
    tree = ast.parse(text)
    nodes = [n for n in tree.body
             if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
             and n.name in names]
    assert {n.name for n in nodes} == set(names)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), "lifted", "exec"), namespace)
    return namespace


class _HostedRepos:
    def __init__(self, row):
        self.row = row
        self.set = []

    def enabled(self, _env):
        return True

    async def ensure_tables(self, _runtime):
        return None

    async def find_by_key(self, _runtime, _key):
        return self.row

    async def set_visibility(self, _runtime, repo_id, visibility):
        self.set.append((repo_id, visibility))


def _visibility_ns(actor="alice", hosted=True, record=None, org_admin=False):
    calls = {"catalog": [], "audits": [], "republished": [],
             "tombstones": [], "fork_edges": []}
    hosted_repos = _HostedRepos(
        {"id": "r1", "data": {"organization": "garden-club" if org_admin else ""}}
        if hosted else None)

    async def write_catalog(_env, rec, *_args):
        calls["catalog"].append(rec)

    async def audit(_env, who, action, kind, target, outcome, meta=None):
        calls["audits"].append((who, outcome, meta))

    ns = {
        "bounded_json_request": lambda request: _value(request.body),
        "_authed_account_name": lambda *_a: _value(actor),
        "_session_required_response": lambda: {"status": 401},
        "_error": lambda code, status, message="", **_k: {"status": status, "error": code},
        "json_response": lambda data, status=200, **_k: {"status": status, "data": data},
        "clean_string": lambda v, n: str(v or "")[:n],
        "MAX_NODE_NAME": 63,
        "_hosted_repos": hosted_repos,
        "_ReplicaRuntime": lambda env, request: object(),
        "ensure_schema": lambda _env: _value(None),
        "blind_index": lambda _env, v: _value("bi:" + v),
        "decrypt_row": lambda _env, v: _value(v),
        "encrypt_row": lambda _env, v: _value(v),
        "_account_owns_node": lambda _env, who, node: _value(who == node),
        "_org_row": lambda _env, org: _value(("org-bi", {"name": org})),
        "_org_permission": lambda *_a: _value("admin" if org_admin else "read"),
        "team_permission_rank": lambda p: {"read": 1, "write": 2, "maintain": 3, "admin": 4}.get(p, 0),
        "d1_first": lambda *_a: _value({"data": dict(record or {"owner": "alice", "name": "garden", "visibility": "public"})}),
        "Date": type("D", (), {"now": staticmethod(lambda: 1_790_000_000_000)}),
        "safe_catalog_record": lambda rec: rec,
        "_account_row": lambda _env, who: _value(("bi", {"name": who})),
        "_contribution_write_catalog_state": write_catalog,
        "_https_mirror_public_context_cache_clear": lambda: None,
        "edge_cache_delete": lambda _k: _value(None),
        "CATALOG_CACHE_KEY": "catalog",
        "_bump_catalog_rev": lambda _env: _value(None),
        "schedule_public_catalog_republish": lambda _env, o, n: _value(calls["republished"].append((o, n))),
        "_notify_cores": lambda *_a: _value(None),
        "_audit_sensitive_action": audit,
        "invalidate_public_catalog_snapshots": lambda _env, o, n: _value(
            calls["tombstones"].append((o, n))),
        "_index_repo_fork_edge": lambda _env, rec, prior, owner_bi, key_bi: _value(
            calls["fork_edges"].append(
                (prior.get("visibility"), rec["visibility"], key_bi))),
    }
    _lift(HOSTED, {"_change_visibility"}, ns)
    return ns, calls, hosted_repos


class _Request:
    def __init__(self, body):
        self.body = body


def test_the_owner_makes_a_hosted_repository_private():
    ns, calls, hosted = _visibility_ns()
    result = _run(ns["_change_visibility"](None, _Request(
        {"owner": "alice", "name": "garden", "visibility": "private"})))
    assert result["status"] == 200 and result["data"]["changed"] is True
    assert calls["catalog"][0]["visibility"] == "private"
    assert hosted.set == [("r1", "private")]
    assert calls["republished"] == [("alice", "garden")]
    assert calls["audits"][-1][1] == "success"
    assert calls["tombstones"] == [("alice", "garden")]
    assert calls["fork_edges"] == [("public", "private", "bi:alice/garden")]


def test_making_a_hosted_repository_public_writes_no_tombstone():
    ns, calls, _hosted = _visibility_ns(record={
        "owner": "alice", "name": "garden", "visibility": "private"})
    result = _run(ns["_change_visibility"](None, _Request(
        {"owner": "alice", "name": "garden", "visibility": "public"})))
    assert result["data"]["changed"] is True
    assert calls["tombstones"] == []
    assert calls["fork_edges"] == [("private", "public", "bi:alice/garden")]


def test_strangers_and_node_held_repositories_are_refused():
    ns, calls, hosted = _visibility_ns(actor="mallory")
    refused = _run(ns["_change_visibility"](None, _Request(
        {"owner": "alice", "name": "garden", "visibility": "private"})))
    assert refused["status"] == 403 and hosted.set == [] and calls["catalog"] == []
    ns, _calls, _hosted = _visibility_ns(hosted=False)
    node = _run(ns["_change_visibility"](None, _Request(
        {"owner": "alice", "name": "garden", "visibility": "private"})))
    assert node == {"status": 409, "error": "node_held"}
    ns, _calls, _hosted = _visibility_ns()
    bad = _run(ns["_change_visibility"](None, _Request(
        {"owner": "alice", "name": "garden", "visibility": "secret"})))
    assert bad["status"] == 400


def test_an_organization_admin_may_change_an_organization_repository():
    ns, _calls, hosted = _visibility_ns(actor="bob", org_admin=True)
    result = _run(ns["_change_visibility"](None, _Request(
        {"owner": "alice", "name": "garden", "visibility": "private"})))
    assert result["status"] == 200 and hosted.set == [("r1", "private")]


def test_an_unchanged_visibility_writes_nothing():
    ns, calls, hosted = _visibility_ns(record={"visibility": "private"})
    result = _run(ns["_change_visibility"](None, _Request(
        {"owner": "alice", "name": "garden", "visibility": "private"})))
    assert result["data"]["changed"] is False
    assert calls["catalog"] == [] and hosted.set == []


def test_collaborators_are_managed_with_the_owners_session():
    handler = ENTRY[ENTRY.index("async def shares_handler"):]
    handler = handler[:handler.index("\nasync def ", 10)]
    assert handler.count('globals().get("_shares_session_owner")') == 2
    ns = _lift(ENTRY, {"_shares_session_owner"}, {
        "_authed_account_name": lambda _env, _req, data: _value(data.get("who", "")),
        "_account_owns_node": lambda _env, who, node: _value(who == node),
    })
    check = ns["_shares_session_owner"]
    assert _run(check(None, None, {"who": "alice"}, "alice")) is True
    assert _run(check(None, None, {"who": "bob"}, "alice")) is False
    assert _run(check(None, None, {}, "alice")) is False
    assert "FROM hosted_repos " in handler and "not recipient and not core_hosted" in handler


def test_collaborators_read_core_hosted_private_repositories():
    allowed = HOSTED[HOSTED.index("async def _hosted_read_allowed"):]
    allowed = allowed[:allowed.index("\nasync def ")]
    assert 'globals().get("_repo_shared_with")' in allowed
    assert "await shared_with(\n                env, owner, repo, account)" in allowed


def test_the_settings_tab_offers_access_controls_to_the_owner():
    match = re.search(r"\n  function renderRepoAccessSettings\(", DASHBOARD_JS)
    render = DASHBOARD_JS[match.start():DASHBOARD_JS.index("\n  }\n", match.start())]
    assert "if (!sessionOwnsRepo(repo)) return \"\";" in render
    assert "data-repo-visibility-change" in render and "data-repo-collaborator-form" in render
    assert "const hosted = repoIsCoreHosted(repo);" in render
    assert '"/api/hosted-repositories/visibility"' in DASHBOARD_JS
    assert "`${repoApiBase(repo)}/shares`" in DASHBOARD_JS
    for selector in ("[data-repo-collaborator-form]", "[data-repo-collaborator-remove]",
                     "[data-repo-visibility-change]"):
        assert 'event.target.closest("%s")' % selector in DASHBOARD_JS
    assert "void loadRepoCollaborators(state.selectedRepo);" in DASHBOARD_JS
