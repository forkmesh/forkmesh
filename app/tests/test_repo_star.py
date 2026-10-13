#!/usr/bin/env python3
"""Star/unstar a repo (GET count+state, POST/DELETE toggle).

Same AST-extraction harness as test_endpoint_session_auth.py: pull the
functions under test out of entry.py and stub the D1/crypto/JS primitives
they call, so the toggle's session-auth and repo_stars bookkeeping are
exercised without a live Worker/D1.
"""

import ast
import asyncio
import hmac
import re
import sys
from pathlib import Path

from worker_test_helpers import json_from_request_double


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

import urls  # noqa: E402

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
CATALOG = ENTRY.parent / "catalog.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8") + "\n" + CATALOG.read_text(encoding="utf-8")

FUNCS = {
    "repo_star_handler", "_public_repo_owner", "_repo_is_private",
    "clean_string", "method_name",
}


def _load_functions(extra_globals):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in FUNCS
    ]
    found = {node.name for node in selected}
    assert found == FUNCS, "missing functions: %s" % sorted(FUNCS - found)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    namespace.setdefault("bounded_json_request", json_from_request_double)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


class _Headers:
    def __init__(self, mapping=None):
        self._m = {str(k).lower(): v for k, v in (mapping or {}).items()}

    def get(self, name, default=None):
        return self._m.get(str(name).lower(), default)


class _Request:
    def __init__(self, method="GET", url="https://forkmesh.test/x", body=None,
                 headers=None):
        self.method = method
        self.url = url
        self._body = body
        self.headers = _Headers(headers)

    async def json(self):
        if self._body is None:
            raise ValueError("no body")
        return self._body


def _harness(accounts, repositories=None, stars=None):
    # Public serving now fails closed when the catalog row is absent, so the
    # ordinary harness includes the explicit public repository under test.
    repositories = list(
        repositories if repositories is not None else [
            {"key_bi": "bi:alice/proj", "is_private": 0}
        ]
    )  # {key_bi, is_private}
    stars = list(stars or [])                # {repo_bi, account_bi, created_at}
    now = [1_000_000_000]

    class _DateStub:
        @staticmethod
        def now():
            return now[0]

    def json_response(data, status=200, **_kwargs):
        return {"status": status, "data": data}

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value or "").strip().lower()

    async def _account_row(_env, name):
        key = str(name or "").strip().lower()
        rec = accounts.get(key)
        if rec is None:
            return "bi:" + key, None
        rec = dict(rec)
        rec.setdefault("name", key)
        return "bi:" + key, rec

    def _account_session_token(_env, name):
        return "test-session:" + str(name or "").strip().lower()

    async def _account_session_record(_env, request, data=None):
        payload = data if isinstance(data, dict) else {}
        token = str(payload.get("sessionToken") or "")
        if not token:
            auth = request.headers.get("authorization") or ""
            token = auth[7:] if auth.lower().startswith("bearer ") else ""
        prefix = "test-session:"
        name = token[len(prefix):] if token.startswith(prefix) else ""
        rec = accounts.get(name)
        if not rec or rec.get("status") != "active":
            return "", None
        record = dict(rec)
        record.setdefault("name", name)
        return "bi:" + name, record

    async def d1_first(_env, sql, *args):
        if "FROM repositories" in sql:
            key_bi = args[0]
            for r in repositories:
                if r["key_bi"] == key_bi:
                    return {"is_private": r.get("is_private", 0)}
            return None
        if "SELECT 1 AS yes FROM repo_stars" in sql:
            repo_bi, account_bi = args
            for s in stars:
                if s["repo_bi"] == repo_bi and s["account_bi"] == account_bi:
                    return {"yes": 1}
            return None
        if "SELECT COUNT(*) AS n FROM repo_stars" in sql:
            repo_bi = args[0]
            return {"n": sum(1 for s in stars if s["repo_bi"] == repo_bi)}
        raise AssertionError("unexpected d1_first: " + sql)

    async def d1_run(_env, sql, *args):
        if sql.startswith("INSERT OR IGNORE INTO repo_stars"):
            repo_bi, account_bi, created_at = args
            if not any(s["repo_bi"] == repo_bi and s["account_bi"] == account_bi
                       for s in stars):
                stars.append({"repo_bi": repo_bi, "account_bi": account_bi,
                              "created_at": created_at})
            return
        if sql.startswith("DELETE FROM repo_stars"):
            repo_bi, account_bi = args
            stars[:] = [s for s in stars
                       if not (s["repo_bi"] == repo_bi and s["account_bi"] == account_bi)]
            return
        raise AssertionError("unexpected d1_run: " + sql)

    ns = _load_functions({
        "Date": _DateStub,
        "json_response": json_response,
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "_account_row": _account_row,
        "_account_session_token": _account_session_token,
        "_account_session_record": _account_session_record,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "re": re,
        "hmac": hmac,
        "REPO_STAR_RE": urls.REPO_STAR_RE,
        "MAX_NODE_NAME": 63,
        "NODE_NAME_RE": re.compile(r"^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"),
        "ADMIN_SESSION_TTL_MS": 12 * 60 * 60 * 1000,
    })
    ns["_stars"] = stars
    return ns


def _user():
    return {"pubkey": "PK", "status": "active", "pass_hash": "h", "pass_salt": "s"}


def _bearer(ns, env, name):
    return {"authorization": "Bearer " + ns["_account_session_token"](env, name)}


def test_star_get_is_public_and_reports_viewer_state():
    accounts = {"alice": _user(), "mallory": _user()}
    ns = _harness(accounts, stars=[{"repo_bi": "bi:alice/proj",
                                    "account_bi": "bi:mallory", "created_at": 1}])
    env = object()
    url = "https://forkmesh.test/api/repo/alice/proj/star"

    anon = asyncio.run(ns["repo_star_handler"](env, _Request("GET", url), "alice", "proj"))
    assert anon["status"] == 200
    assert anon["data"] == {"ok": True, "count": 1, "starred": False}

    starrer = asyncio.run(ns["repo_star_handler"](
        env, _Request("GET", url, headers=_bearer(ns, env, "mallory")), "alice", "proj"))
    assert starrer["data"] == {"ok": True, "count": 1, "starred": True}


def test_star_get_404s_for_private_repo():
    accounts = {"alice": _user()}
    repos = [{"key_bi": "bi:alice/proj", "is_private": 1}]
    ns = _harness(accounts, repositories=repos)
    env = object()
    resp = asyncio.run(ns["repo_star_handler"](
        env, _Request("GET", "https://forkmesh.test/api/repo/alice/proj/star"),
        "alice", "proj"))
    assert resp["status"] == 404


def test_star_post_requires_session_and_is_idempotent():
    accounts = {"alice": _user(), "mallory": _user()}
    ns = _harness(accounts)
    env = object()
    url = "https://forkmesh.test/api/repo/alice/proj/star"

    # No session -> rejected, nothing recorded.
    anon = asyncio.run(ns["repo_star_handler"](
        env, _Request("POST", url, body={}), "alice", "proj"))
    assert anon["status"] == 401
    assert ns["_stars"] == []

    # A valid session can star, and starring twice doesn't double-count.
    first = asyncio.run(ns["repo_star_handler"](
        env, _Request("POST", url, body={
            "sessionToken": ns["_account_session_token"](env, "mallory")}),
        "alice", "proj"))
    assert first["status"] == 200
    assert first["data"] == {"ok": True, "count": 1, "starred": True}

    second = asyncio.run(ns["repo_star_handler"](
        env, _Request("POST", url, body={
            "sessionToken": ns["_account_session_token"](env, "mallory")}),
        "alice", "proj"))
    assert second["data"]["count"] == 1


def test_star_delete_unstars():
    accounts = {"alice": _user(), "mallory": _user()}
    ns = _harness(accounts, stars=[{"repo_bi": "bi:alice/proj",
                                    "account_bi": "bi:mallory", "created_at": 1}])
    env = object()
    url = "https://forkmesh.test/api/repo/alice/proj/star"

    resp = asyncio.run(ns["repo_star_handler"](
        env, _Request("DELETE", url, body={
            "sessionToken": ns["_account_session_token"](env, "mallory")}),
        "alice", "proj"))
    assert resp["data"] == {"ok": True, "count": 0, "starred": False}
    assert ns["_stars"] == []
