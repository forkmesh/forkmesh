#!/usr/bin/env python3
"""Private named control paths must not become repository-existence oracles."""

import ast
import asyncio
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
SOURCE = ENTRY.read_text(encoding="utf-8")
RUNTIME = ENTRY.with_name("app_runtime.py")
RUNTIME_SOURCE = RUNTIME.read_text(encoding="utf-8")


def _function(name, source=SOURCE, filename=ENTRY):
    tree = ast.parse(source, filename=str(filename))
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name == name:
                return node
    raise AssertionError("missing function: " + name)


class _Request:
    class _Headers:
        def get(self, _name, default=None):
            return default

    headers = _Headers()


def _load_context(row, *, actor="", owns=False, shared=False, basic=False):
    decrypt_calls = []

    async def ensure_schema(_env):
        return None

    async def blind_index(_env, value):
        return "bi:" + str(value).lower()

    async def d1_first(_env, _sql, *_args):
        return dict(row) if row is not None else None

    async def decrypt_row(_env, payload):
        decrypt_calls.append(payload)
        return payload

    async def authed(_env, _request):
        return actor

    async def account_owns(_env, _actor, _owner):
        return owns

    async def repo_shared(_env, _owner, _repo, _actor):
        return shared

    async def basic_ok(_env, _owner, _repo, _request):
        return basic

    def safe_segment(value):
        value = str(value or "")
        return value if value and "/" not in value else ""

    def clean_string(value, limit):
        return str(value or "")[:limit]

    def identity(record, owner, repo):
        return bool(
            isinstance(record, dict)
            and str(record.get("owner", "")).lower() == owner.lower()
            and str(record.get("name", "")).lower() == repo.lower()
        )

    namespace = {
        "ensure_schema": ensure_schema,
        "blind_index": blind_index,
        "d1_first": d1_first,
        "decrypt_row": decrypt_row,
        "_authed_account_name": authed,
        "_account_owns_node": account_owns,
        "_repo_shared_with": repo_shared,
        "_basic_auth_view_ok": basic_ok,
        "_catalog_record_matches_identity": identity,
        "safe_segment": safe_segment,
        "clean_string": clean_string,
        "MAX_NODE_NAME": 63,
    }
    module = ast.fix_missing_locations(ast.Module(
        body=[_function("_repository_access_context")], type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_repository_access_context"], decrypt_calls


def _record(visibility):
    return {
        "owner": "alice",
        "name": "secret",
        "visibility": visibility,
    }


def test_explicit_public_repository_resolves_without_authentication():
    context, decrypt_calls = _load_context({
        "owner_bi": "bi:alice",
        "is_private": 0,
        "data": _record("public"),
    })
    result = asyncio.run(context(None, _Request(), "alice", "secret"))
    assert result["visibility"] == "public"
    assert len(decrypt_calls) == 1


def test_private_and_missing_repositories_share_a_hidden_result():
    private_context, private_decrypts = _load_context({
        "owner_bi": "bi:alice",
        "is_private": 1,
        "data": _record("private"),
    })
    missing_context, missing_decrypts = _load_context(None)
    private = asyncio.run(
        private_context(None, _Request(), "alice", "secret"))
    missing = asyncio.run(
        missing_context(None, _Request(), "alice", "secret"))
    assert private is None
    assert missing is None
    # The private catalog payload is not decrypted before the blind-index ACL.
    assert private_decrypts == []
    assert missing_decrypts == []


def test_owner_or_explicit_share_can_resolve_private_repository():
    row = {
        "owner_bi": "bi:alice",
        "is_private": 1,
        "data": _record("private"),
    }
    owner_context, owner_decrypts = _load_context(
        row, actor="alice", owns=True)
    shared_context, shared_decrypts = _load_context(
        row, actor="bob", shared=True)
    owner = asyncio.run(
        owner_context(None, _Request(), "alice", "secret"))
    shared = asyncio.run(
        shared_context(None, _Request(), "alice", "secret"))
    assert owner["visibility"] == "private"
    assert shared["visibility"] == "private"
    assert len(owner_decrypts) == len(shared_decrypts) == 1


def test_stale_public_flag_cannot_expose_signed_private_record():
    context, decrypt_calls = _load_context({
        "owner_bi": "bi:alice",
        "is_private": 0,
        "data": _record("private"),
    })
    assert asyncio.run(
        context(None, _Request(), "alice", "secret")) is None
    assert len(decrypt_calls) == 1


def test_room_and_bounty_routes_gate_before_sensitive_storage_access():
    route = ast.unparse(_function("_route", RUNTIME_SOURCE, RUNTIME))
    room = route[route.index("room = room_key_from_path"):]
    access_at = room.index("_repository_access_context")
    durable_at = room.index("FORKMESH_MAINNODE_ROOM.idFromName")
    assert access_at < durable_at
    assert "_private_replica_not_found" in room[access_at:durable_at]

    bounty = ast.unparse(_function("bounties_handler"))
    access_at = bounty.index("_repository_access_context")
    key_at = bounty.index("_bounty_bi")
    load_at = bounty.index("_load_bounty")
    assert access_at < key_at < load_at
    assert "_private_replica_not_found" in bounty[:key_at]
