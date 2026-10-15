import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace
import time


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
NOW = 1_788_000_000_000


class _Request:
    def __init__(self, method="GET"):
        self.method = method


def _handler(rows=(), private=False):
    wanted = {"_utc_day_key", "repo_traffic_handler"}
    selected = [
        node for node in ast.parse(ENTRY.read_text(encoding="utf-8")).body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in wanted
    ]
    calls = []

    async def d1_all(_env, sql, *args):
        calls.append((" ".join(sql.split()), args))
        return list(rows)

    async def ensure_schema(_env):
        return None

    async def is_private(_env, _owner, _repo):
        return private

    def json_response(data, status=200, cache_control=None):
        return {"status": status, "data": data, "cache_control": cache_control}

    namespace = {
        "Date": SimpleNamespace(now=lambda: NOW),
        "time": time,
        "method_name": lambda request: request.method,
        "ensure_schema": ensure_schema,
        "_repo_is_private": is_private,
        "d1_all": d1_all,
        "json_response": json_response,
    }
    module = ast.fix_missing_locations(ast.Module(selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["repo_traffic_handler"], calls


def test_repo_traffic_returns_a_complete_thirty_day_clone_series():
    handler, calls = _handler(rows=(
        {"day": "2026-08-01", "clones": 4},
        {"day": "2026-08-29", "clones": 2},
    ))

    response = asyncio.run(handler(object(), _Request(), "Alice", "Project"))

    assert response["status"] == 200
    assert response["cache_control"] == "public, max-age=60"
    assert response["data"]["owner"] == "alice"
    assert response["data"]["repo"] == "project"
    assert len(response["data"]["days"]) == 30
    assert response["data"]["days"][1] == {"day": "2026-08-01", "clones": 4}
    assert response["data"]["days"][-1] == {"day": "2026-08-29", "clones": 2}
    assert response["data"]["totalClones"] == 6
    assert len(calls) == 1
    assert "FROM product_clone_daily" in calls[0][0]
    assert calls[0][1] == ("alice", "project", "2026-07-31", "2026-08-29")


def test_repo_traffic_keeps_private_repositories_and_non_gets_out_of_the_query():
    handler, calls = _handler(private=True)

    private_response = asyncio.run(handler(object(), _Request(), "alice", "project"))
    method_response = asyncio.run(handler(object(), _Request("POST"), "alice", "project"))

    assert private_response == {
        "status": 404,
        "data": {"error": "not_found"},
        "cache_control": "no-store",
    }
    assert method_response == {
        "status": 405,
        "data": {"error": "method_not_allowed"},
        "cache_control": None,
    }
    assert calls == []
