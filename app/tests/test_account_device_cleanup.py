#!/usr/bin/env python3
"""purge_stale_account_devices — prune the reinstall/rotate device-key sprawl.

Reinstalling or rotating a desktop node mints a new account_devices row (keyed on
the pubkey), so one account slowly accretes a row per historical key. The purge
removes rows untouched for STALE_DEVICE_RETAIN_MS, but ONLY when the account still
has a newer device — a node's current signing key (which the drain gate now
reuses) must never be deleted.
"""

import ast
import asyncio
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")

DAY_MS = 24 * 60 * 60 * 1000
NOW = 1_000_000_000_000
RETAIN_MS = 60 * DAY_MS


def _load():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    funcs = {"purge_stale_account_devices"}
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in funcs
    ]
    assert {n.name for n in selected} == funcs
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))

    class _Date:
        @staticmethod
        def now():
            return NOW

    ns = {
        "Date": _Date,
        "STALE_DEVICE_RETAIN_MS": RETAIN_MS,
        "STALE_DEVICE_PURGE_BATCH": 12,
    }
    exec(compile(module, str(ENTRY), "exec"), ns)
    return ns


def _harness(devices):
    # devices: list of {"device_bi","account_bi","last_seen"}
    store = list(devices)

    async def d1_all(_env, sql, *args):
        assert "FROM account_devices" in sql and "last_seen" in sql
        cutoff = args[0]
        rows = [
            dict(d) for d in store
            if int(d.get("last_seen") or 0) < cutoff
            and any(
                newer["account_bi"] == d["account_bi"]
                and int(newer.get("last_seen") or 0)
                > int(d.get("last_seen") or 0)
                for newer in store
            )
        ]
        rows.sort(key=lambda d: int(d.get("last_seen") or 0))
        return rows[: args[1]]

    async def d1_first(_env, sql, *args):
        assert "SELECT 1" in sql
        account_bi, seen = args
        for d in store:
            if d["account_bi"] == account_bi and int(d.get("last_seen") or 0) > seen:
                return {"x": 1}
        return None

    async def d1_run(_env, sql, *args):
        assert sql.startswith("DELETE FROM account_devices WHERE device_bi=?")
        store[:] = [d for d in store if d["device_bi"] != args[0]]

    ns = _load()
    ns.update({"d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run})
    return ns, store


def test_prunes_stale_superseded_key_but_keeps_the_current_one():
    old = NOW - 200 * DAY_MS   # stale, superseded (a reinstall left it behind)
    cur = NOW - 1 * DAY_MS      # the live/most-recent key for this account
    ns, store = _harness([
        {"device_bi": "old", "account_bi": "jett", "last_seen": old},
        {"device_bi": "cur", "account_bi": "jett", "last_seen": cur},
    ])
    removed = asyncio.run(ns["purge_stale_account_devices"](object()))
    assert removed == 1
    ids = {d["device_bi"] for d in store}
    assert ids == {"cur"}  # current key survives, stale duplicate is gone


def test_never_deletes_an_accounts_only_or_most_recent_device():
    # Even a long-idle device is kept when it is the account's newest — deleting
    # it would strip the node's only signing key and silently 401 its drains.
    old = NOW - 300 * DAY_MS
    ns, store = _harness([
        {"device_bi": "solo", "account_bi": "solo-acct", "last_seen": old},
    ])
    removed = asyncio.run(ns["purge_stale_account_devices"](object()))
    assert removed == 0
    assert {d["device_bi"] for d in store} == {"solo"}


def test_leaves_recently_seen_devices_untouched():
    recent = NOW - 3 * DAY_MS
    ns, store = _harness([
        {"device_bi": "a", "account_bi": "acct", "last_seen": recent},
        {"device_bi": "b", "account_bi": "acct", "last_seen": NOW - 1 * DAY_MS},
    ])
    removed = asyncio.run(ns["purge_stale_account_devices"](object()))
    assert removed == 0
    assert len(store) == 2
