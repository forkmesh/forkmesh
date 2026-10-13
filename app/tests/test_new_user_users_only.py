#!/usr/bin/env python3
"""Account saves persist only via the users/nodes tables (issue #172).

_save_account is the single choke point every account write flows through.
The legacy accounts table was dropped by migration 0042, so a save of any
record kind must go solely through _mirror_account_identity_tables — no SQL
against the accounts table, for users or nodes.
"""

import ast
import asyncio
from pathlib import Path

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"

FUNCS = {"_save_account", "_account_kind"}


def _load(extra):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [n for n in tree.body
                if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                and n.name in FUNCS]
    assert {n.name for n in selected} == FUNCS, "missing functions"
    mod = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    ns = dict(extra)
    exec(compile(mod, str(ENTRY), "exec"), ns)
    return ns


def _harness():
    calls = {"sql": [], "mirrors": []}

    def clean_string(value, _n, *_a, **_k):
        return str(value or "")

    async def encrypt_row(_env, obj):
        return dict(obj)

    async def d1_first(_env, sql, *args):
        calls["sql"].append(sql)
        return None

    async def d1_run(_env, sql, *args):
        calls["sql"].append(sql)

    async def _mirror_account_identity_tables(_env, name_bi, rec, **_k):
        calls["mirrors"].append((name_bi, rec.get("name")))

    ns = _load({
        "clean_string": clean_string,
        "encrypt_row": encrypt_row,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "_mirror_account_identity_tables": _mirror_account_identity_tables,
        "MAX_NODE_NAME": 64,
    })
    return ns, calls


def test_user_save_goes_only_through_identity_tables():
    ns, calls = _harness()
    rec = {"name": "alice", "kind": "user", "pass_hash": "h"}
    asyncio.run(ns["_save_account"](object(), "bi:alice", rec, email_bi="e"))
    assert calls["sql"] == []                        # never touched accounts
    assert calls["mirrors"] == [("bi:alice", "alice")]  # went into users


def test_node_save_goes_only_through_identity_tables():
    ns, calls = _harness()
    rec = {"name": "mirror1", "kind": "node", "pubkey": "pk"}
    asyncio.run(ns["_save_account"](object(), "bi:mirror1", rec))
    assert calls["sql"] == []
    assert calls["mirrors"] == [("bi:mirror1", "mirror1")]


if __name__ == "__main__":
    test_user_save_goes_only_through_identity_tables()
    test_node_save_goes_only_through_identity_tables()
    print("ok")
