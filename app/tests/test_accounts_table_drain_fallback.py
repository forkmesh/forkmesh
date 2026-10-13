#!/usr/bin/env python3
"""Identity lookups resolve via the users/nodes tables (adhoc #20).

The legacy accounts table was dropped by migration 0042: the users/nodes
tables are the only store for account records. These tests pin the read side:
name lookups must resolve users first, then nodes (login, profile, admin
status), and return None when the record exists in neither.
"""

import ast
import asyncio
from pathlib import Path

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"

FUNCS = {"_account_identity_rec_by_bi", "_account_row", "_is_admin",
         "_is_admin_flag"}


def _load(extra):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [n for n in tree.body
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                and n.name in FUNCS]
    assert {n.name for n in selected} == FUNCS, "missing functions"
    mod = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    ns = dict(extra)
    # Per-isolate memo the hot-path helpers reference (module-level in
    # entry.py; a fresh one per load keeps tests isolated).
    ns.setdefault("_MIRRORED_ACCOUNT_BIS", set())
    exec(compile(mod, str(ENTRY), "exec"), ns)
    return ns


def _harness(users=None, nodes=None):
    users = users or {}         # user_bi -> {data, email_bi, is_admin}
    nodes = nodes or {}         # node_bi -> {data, name}

    async def blind_index(_env, value):
        return "bi:" + str(value)

    async def decrypt_row(_env, stored, key=None):
        return dict(stored) if isinstance(stored, dict) else stored

    async def encrypt_row(_env, obj):
        return dict(obj)

    async def d1_first(_env, sql, *args):
        key = args[0] if args else None
        if "FROM users WHERE user_bi" in sql:
            return users.get(key)
        if "FROM nodes WHERE node_bi" in sql:
            return nodes.get(key)
        raise AssertionError("unexpected d1_first: " + sql)

    return _load({
        "blind_index": blind_index,
        "decrypt_row": decrypt_row,
        "encrypt_row": encrypt_row,
        "d1_first": d1_first,
    })


def test_account_row_resolves_users_table():
    ns = _harness(users={"bi:alice": {"data": {"name": "alice", "kind": "user"}}})
    name_bi, rec = asyncio.run(ns["_account_row"](object(), "alice"))
    assert name_bi == "bi:alice"
    assert rec == {"name": "alice", "kind": "user"}


def test_account_row_resolves_nodes_table():
    ns = _harness(nodes={"bi:mirror1": {"data": {"name": "mirror1", "kind": "node"}}})
    _, rec = asyncio.run(ns["_account_row"](object(), "mirror1"))
    assert rec == {"name": "mirror1", "kind": "node"}


def test_account_row_prefers_users_over_nodes():
    ns = _harness(
        users={"bi:alice": {"data": {"name": "alice", "src": "users"}}},
        nodes={"bi:alice": {"data": {"name": "alice", "src": "nodes"}}},
    )
    _, rec = asyncio.run(ns["_account_row"](object(), "alice"))
    assert rec["src"] == "users"


def test_account_row_returns_none_when_nowhere():
    ns = _harness()
    name_bi, rec = asyncio.run(ns["_account_row"](object(), "ghost"))
    assert name_bi == "bi:ghost" and rec is None


def test_is_admin_reads_users_table():
    ns = _harness(users={
        "bi:alice": {"is_admin": 1,
                     "data": {"name": "alice", "totp_enrolled": True}},
        "bi:bob": {"is_admin": 1, "data": {"name": "bob"}},
    })
    assert asyncio.run(ns["_is_admin"](object(), "alice")) is True
    assert asyncio.run(ns["_is_admin"](object(), "bob")) is False
    assert asyncio.run(ns["_is_admin_flag"](object(), "bob")) is True
    assert asyncio.run(ns["_is_admin"](object(), "nobody")) is False
