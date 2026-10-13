"""Bounded, fair traversal for the blocked-catalog maintenance phase."""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
INTERVAL_MS = 15 * 60 * 1000


def _load_purge():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = {
        "_blocked_catalog_set",
        "_blocked_catalog_owner_batch",
        "purge_blocked_catalog",
    }
    constants = {
        "_BLOCKED_CATALOG_DEFAULT",
        "BLOCKED_CATALOG_OWNER_SCAN_LIMIT",
        "BLOCKED_CATALOG_PHASE_INTERVAL_MS",
    }
    nodes = []
    for node in tree.body:
        if (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and node.name in wanted):
            nodes.append(node)
        elif isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id in constants
                for target in node.targets):
            nodes.append(node)
    namespace = {
        "Date": type("Date", (), {"now": staticmethod(lambda: 0)}),
    }
    module = ast.fix_missing_locations(ast.Module(body=nodes, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    assert wanted <= namespace.keys()
    return namespace


def _run_purge(owner_names, *, now, present=None):
    namespace = _load_purge()
    calls = {"owners": [], "deleted": [], "invalidations": 0, "caches": 0}
    present = set(owner_names if present is None else present)

    async def blind_index(_env, owner):
        return "owner:" + owner

    async def d1_all(_env, sql, owner_bi):
        assert "ORDER BY key_bi LIMIT 1" in sql
        owner = owner_bi.removeprefix("owner:")
        calls["owners"].append(owner)
        if owner not in present:
            return []
        return [{
            "key_bi": "repo:" + owner,
            "data": {"name": "project"},
        }]

    async def decrypt_row(_env, data):
        return data

    async def invalidate(_env, *, include_flagship):
        assert include_flagship is False
        calls["invalidations"] += 1

    async def delete_scoped(_env, repo_bi):
        calls["deleted"].append(repo_bi)

    async def d1_run(_env, sql, repo_bi):
        assert sql == "DELETE FROM repositories WHERE key_bi=?"
        assert repo_bi == calls["deleted"][-1]

    async def purge_caches():
        calls["caches"] += 1

    async def republish(_env, *, include_flagship):
        assert include_flagship is False
        calls["republishes"] = calls.get("republishes", 0) + 1

    async def not_flagship(_env, _owner, _name):
        return False

    namespace.update({
        "blind_index": blind_index,
        "d1_all": d1_all,
        "decrypt_row": decrypt_row,
        "invalidate_public_catalog_snapshots": invalidate,
        "_is_flagship_snapshot_target": not_flagship,
        "_delete_repo_scoped_state": delete_scoped,
        "d1_run": d1_run,
        "purge_catalog_related_caches": purge_caches,
        "schedule_public_catalog_republish": republish,
    })
    env = SimpleNamespace(BLOCKED_CATALOG_OWNERS=" ".join(owner_names))
    result = asyncio.run(namespace["purge_blocked_catalog"](env, now=now))
    return result, calls


def test_owner_scan_has_a_hard_eight_owner_ceiling():
    owners = ["owner-%02d" % index for index in range(20)]
    result, calls = _run_purge(owners, now=0, present=set())

    assert result == 0
    assert len(calls["owners"]) == 8
    assert calls["owners"] == sorted({"7cbaf0dc", *owners})[:8]
    assert calls["deleted"] == []


def test_phase_slots_rotate_every_owner_to_the_front_after_restarts():
    owners = ["owner-%02d" % index for index in range(12)]
    all_owners = sorted({"7cbaf0dc", *owners})
    first_owners = []

    for slot in range(len(all_owners)):
        result, calls = _run_purge(
            owners, now=slot * INTERVAL_MS, present=all_owners)
        assert result == 1
        assert len(calls["owners"]) == 1
        assert len(calls["deleted"]) == 1
        assert calls["invalidations"] == 1
        assert calls["caches"] == 1
        first_owners.append(calls["owners"][0])

    assert first_owners == all_owners


def test_same_slot_retry_is_deterministic_and_next_slot_makes_progress():
    owners = ["owner-%02d" % index for index in range(10)]
    all_owners = sorted({"7cbaf0dc", *owners})
    first_result, first_calls = _run_purge(owners, now=0, present=set())
    retry_result, retry_calls = _run_purge(
        owners, now=INTERVAL_MS - 1, present=set())
    next_result, next_calls = _run_purge(
        owners, now=INTERVAL_MS, present=set())

    assert first_result == retry_result == next_result == 0
    assert retry_calls["owners"] == first_calls["owners"]
    assert next_calls["owners"] == [
        all_owners[(1 + offset) % len(all_owners)] for offset in range(8)
    ]
