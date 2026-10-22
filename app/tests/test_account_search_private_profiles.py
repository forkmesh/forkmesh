#!/usr/bin/env python3

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")


def _load(namespace):
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    wanted = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name in ("_account_kind", "_is_public_roster_member")
    ]
    runtime = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef)
        and node.name == "_WorldCommunityRuntime")
    search = next(
        node for node in runtime.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "search_accounts")
    scope = dict(namespace)
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=wanted + [search], type_ignores=[])),
        str(ENTRY), "exec"), scope)
    return scope


def _search(rows, query="a", limit=10):
    async def d1_all(_env, _sql, *_args):
        return rows

    async def decrypt_row(_env, data):
        return data

    scope = _load({
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "clean_string": lambda value, n=240: str(value or "")[:n],
        "valid_node_name": lambda name: bool(name) and name.isalnum(),
        "MAX_NODE_NAME": 64,
    })
    owner = SimpleNamespace(env=SimpleNamespace())
    return asyncio.run(scope["search_accounts"](owner, query, limit))


def test_account_search_leaves_out_private_profiles():
    rows = [
        {"username": "alice", "data": {
            "name": "alice", "status": "active", "kind": "user"}},
        {"username": "anna", "data": {
            "name": "anna", "status": "active", "kind": "user",
            "profile_private": True}},
        {"username": "abe", "data": {
            "name": "abe", "status": "quarantined", "kind": "user"}},
        {"username": "anode", "data": {
            "name": "anode", "status": "active", "kind": "node"}},
    ]
    assert _search(rows) == ["alice"]

