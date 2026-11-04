#!/usr/bin/env python3

import ast
import asyncio
from pathlib import Path

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"


def _owns(accounts):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = {"_account_owns_node", "_owned_nodes"}
    selected = [node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in wanted]
    assert {node.name for node in selected} == wanted

    async def _account_row(_env, name):
        rec = accounts.get(name)
        return "bi:" + name, (dict(rec) if rec is not None else None)

    namespace = {"_account_row": _account_row}
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(ENTRY),
                 "exec"), namespace)
    return lambda actor, node: asyncio.run(
        namespace["_account_owns_node"](None, actor, node))


def test_the_node_records_owner_wins_over_a_stale_user_list():
    owns = _owns({
        "mallory": {"name": "mallory", "nodes": ["laptop"]},
        "alice": {"name": "alice", "nodes": ["laptop"]},
        "laptop": {"name": "laptop", "owner": "alice"},
    })
    assert owns("alice", "laptop") is True
    assert owns("mallory", "laptop") is False


def test_a_user_list_counts_only_when_the_node_names_no_owner():
    owns = _owns({
        "alice": {"name": "alice", "nodes": ["laptop", "gone"]},
        "bob": {"name": "bob", "nodes": []},
        "laptop": {"name": "laptop"},
    })
    assert owns("alice", "laptop") is True
    assert owns("alice", "gone") is True
    assert owns("bob", "laptop") is False
    assert owns("laptop", "laptop") is True
    assert owns("", "laptop") is False


def test_relinking_a_node_moves_ownership_with_the_node_record():
    accounts = {
        "alice": {"name": "alice", "nodes": ["laptop"]},
        "bob": {"name": "bob", "nodes": ["laptop"]},
        "laptop": {"name": "laptop", "owner": "bob"},
    }
    owns = _owns(accounts)
    assert owns("bob", "laptop") is True
    assert owns("alice", "laptop") is False
