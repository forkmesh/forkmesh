#!/usr/bin/env python3

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
HANDLERS = ROOT / "src" / "route_handlers.py"
LIMIT = 50


class Store:
    def __init__(self, rows):
        self.rows = list(rows)
        self.queries = []

    async def d1_all(self, _env, sql, *args):
        self.queries.append((sql, args))
        org_bi, repo, statuses, limit = args[0], args[1], args[2:-1], args[-1]
        rows = [row for row in self.rows
                if row["org_bi"] == org_bi and row["repo"] == repo]
        if "status NOT IN" in sql:
            rows = [row for row in rows if row["status"] not in statuses]
        else:
            rows = [row for row in rows if row["status"] in statuses]
        return sorted(rows, key=lambda row: -row["updated_at"])[:limit]


def _load(store):
    tree = ast.parse(HANDLERS.read_text(encoding="utf-8"),
                     filename=str(HANDLERS))
    wanted = {"org_agent_bots_handler", "_account_agent_scope_rows"}
    constants = {"ACCOUNT_AGENT_MAX_SESSIONS", "ACCOUNT_AGENT_MAX_ACTIVE",
                 "ACCOUNT_AGENT_ACTIVE_STATUSES"}
    selected = [
        node for node in tree.body
        if (isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
            and node.name in wanted)
        or (isinstance(node, ast.Assign)
            and any(isinstance(target, ast.Name) and target.id in constants
                    for target in node.targets))
    ]
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))

    async def noop(*_args, **_kwargs):
        return None

    async def context(*_args, **_kwargs):
        return {
            "org": "jett", "orgBi": "bi:jett", "repo": "forkmesh",
            "actor": "jett",
            "scope": "owner", "role": "owner", "engineeringAccess": True,
            "accessBasis": "owner",
        }, None

    namespace = {
        "ORG_AGENT_MAX_SESSIONS": LIMIT,
        "ORG_AGENT_PROVIDERS": ("claude-code",),
        "ORG_AGENT_MODEL_ALIASES": {"claude-code": {}},
        "ensure_schema": noop,
        "method_name": lambda request: request.method,
        "d1_all": store.d1_all,
        "decrypt_row": lambda _env, value: asyncio.sleep(0, result=value),
        "_owner_agent_signed_triple": lambda _request: False,
        "_org_agent_member_context": context,
        "_org_agent_session_targets": lambda *_a: asyncio.sleep(
            0, result=[]),
        "_org_agent_session_projection": lambda row, _record: {
            "id": row["session_id"], "status": row["status"]},
        "_org_agent_hide_private_runner": (
            lambda projection, row, record, viewer: projection),
        "json_response": lambda data, status=200, **_kwargs: {
            "status": status, "data": data},
    }
    exec(compile(module, str(HANDLERS), "exec"), namespace)
    return namespace


def _row(session_id, status, updated, repo="forkmesh", scope="owner"):
    return {
        "session_id": session_id,
        "org_bi": "bi:jett",
        "repo": repo,
        "target_node": "mirror9",
        "provider": "claude-code",
        "status": status,
        "data": {"org": "jett", "repo": repo, "scope": scope},
        "created_at": 1,
        "updated_at": updated,
        "completed_at": 0,
    }


def _get(rows):
    store = Store(rows)
    handler = _load(store)["org_agent_bots_handler"]
    response = asyncio.run(handler(
        object(), SimpleNamespace(method="GET"), "jett", "forkmesh",
        owner_scope=True))
    assert response["status"] == 200
    return response["data"], store


def test_finished_runs_are_listed_while_more_runs_are_active_than_the_bound():
    active = [_row("q%d" % n, "running", 1000 + n) for n in range(LIMIT + 5)]
    done = [_row("d%d" % n, "completed", 10 + n) for n in range(3)]
    data, _store = _get(active + done)
    ids = [session["id"] for session in data["sessions"]]
    assert {"d0", "d1", "d2"} <= set(ids)
    assert all("q%d" % n in ids for n in range(LIMIT + 5))
    assert ids[0] == "q%d" % (LIMIT + 4)
    assert data["finishedBefore"] == 0


def test_a_cut_finished_list_says_where_it_stopped():
    done = [_row("d%d" % n, "failed", 100 + n) for n in range(LIMIT + 7)]
    data, store = _get(done)
    assert len(data["sessions"]) == LIMIT
    assert data["finishedBefore"] == 100 + 7
    assert all(args[1] == "forkmesh" for _sql, args in store.queries)
    whole, _store = _get(done[:LIMIT])
    assert whole["finishedBefore"] == 0


def test_other_repositories_and_scopes_stay_out():
    rows = [
        _row("mine", "completed", 5),
        _row("other", "completed", 6, repo="notes"),
        _row("org", "completed", 7, scope="organization"),
    ]
    data, _store = _get(rows)
    assert [session["id"] for session in data["sessions"]] == ["mine"]
