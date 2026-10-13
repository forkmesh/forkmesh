#!/usr/bin/env python3
"""Catalog memo refills must not re-decrypt unchanged rows (adhoc #183).

The per-isolate public-catalog memo (_PUBLIC_CATALOG_MEMO) stopped every
request from re-decrypting the catalog, but the single request that claimed
each 5s refill still paid one AES-GCM decrypt_row() per public repo — pure
Pyodide CPU that grows with the catalog. Under clone traffic that request is a
git info/refs, and a large catalog pushed it past the Workers CPU limit
("Worker exceeded CPU time limit" on GET .../info/refs — the same clone-path
family as adhoc #144/#153/#167). Row blobs are rewritten wholesale on every
update, so an unchanged blob is byte-identical: refills now reuse the previous
plaintext via _CATALOG_ROW_DECRYPT_MEMO and only decrypt changed rows.
"""

import ast
import asyncio
from pathlib import Path


ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
_WORKER_SRC = ENTRY.read_text(encoding="utf-8")


def _load(*names, extra_globals=None):
    tree = ast.parse(_WORKER_SRC, filename=str(ENTRY))
    selected = [
        node
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in names
    ]
    found = {node.name for node in selected}
    missing = set(names) - found
    assert not missing, "missing functions: %s" % sorted(missing)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals or {})
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


TTL = 5000


def _make_env(rows, decrypt_calls):
    async def d1_all(env, sql, *args):
        return list(rows)

    async def active_registered_node_bis(env, now=None):
        # Durable catalog retention must not depend on this empty live set.
        # Before the regression fix every row below was filtered out.
        return set()

    async def decrypt_row(env, stored, key=None):
        decrypt_calls.append(stored)
        # Blobs are "cipher:<json-ish payload>"; the plaintext is the tail.
        if not str(stored).startswith("cipher:"):
            return None
        return {
            "payload": str(stored)[len("cipher:"):],
            "visibility": "public",
        }

    return {
        "_PUBLIC_CATALOG_MEMO": {"ts": 0, "rows": None, "refresh_ts": 0},
        "PUBLIC_CATALOG_MEMO_TTL_MS": TTL,
        "_CATALOG_ROW_DECRYPT_MEMO": {},
        "CATALOG_ROW_DECRYPT_MEMO_MAX": 4096,
        "d1_all": d1_all,
        "active_registered_node_bis": active_registered_node_bis,
        "decrypt_row": decrypt_row,
    }


def _row(key, blob):
    return {"key_bi": key, "owner_bi": "o" + key, "data": blob, "is_private": 0}


def test_refill_reuses_unchanged_decrypts():
    rows = [_row("r1", "cipher:a"), _row("r2", "cipher:b")]
    decrypt_calls = []
    ns = _make_env(rows, decrypt_calls)
    ns = _load("_decrypted_public_catalog", extra_globals=ns)
    catalog = ns["_decrypted_public_catalog"]

    now = 1_000_000_000_000
    first = asyncio.run(catalog(None, now))
    assert [r["data"]["payload"] for r in first] == ["a", "b"]
    assert len(decrypt_calls) == 2

    # Second refill (TTL lapsed), identical blobs: zero fresh decrypts.
    second = asyncio.run(catalog(None, now + TTL + 1))
    assert [r["data"]["payload"] for r in second] == ["a", "b"]
    assert len(decrypt_calls) == 2


def test_refill_decrypts_only_changed_rows():
    rows = [_row("r1", "cipher:a"), _row("r2", "cipher:b")]
    decrypt_calls = []
    ns = _make_env(rows, decrypt_calls)
    ns = _load("_decrypted_public_catalog", extra_globals=ns)
    catalog = ns["_decrypted_public_catalog"]

    now = 1_000_000_000_000
    asyncio.run(catalog(None, now))
    assert len(decrypt_calls) == 2

    # r2 was re-published (new ciphertext): only r2 is decrypted again.
    rows[1] = _row("r2", "cipher:b2")
    second = asyncio.run(catalog(None, now + TTL + 1))
    assert [r["data"]["payload"] for r in second] == ["a", "b2"]
    assert decrypt_calls[2:] == ["cipher:b2"]


def test_deleted_rows_drop_out_of_the_decrypt_memo():
    rows = [_row("r1", "cipher:a"), _row("r2", "cipher:b")]
    decrypt_calls = []
    ns = _make_env(rows, decrypt_calls)
    ns = _load("_decrypted_public_catalog", extra_globals=ns)
    catalog = ns["_decrypted_public_catalog"]

    now = 1_000_000_000_000
    asyncio.run(catalog(None, now))
    del rows[1]
    second = asyncio.run(catalog(None, now + TTL + 1))
    assert [r["data"]["payload"] for r in second] == ["a"]
    # The memo is rebuilt from the rows seen this refill (stays bounded by the
    # live catalog); r2's stale plaintext is gone.
    assert set(ns["_CATALOG_ROW_DECRYPT_MEMO"]) == {"cipher:a"}


def test_undecryptable_rows_are_skipped_and_never_cached():
    rows = [_row("r1", "cipher:a"), _row("r2", "garbage")]
    decrypt_calls = []
    ns = _make_env(rows, decrypt_calls)
    ns = _load("_decrypted_public_catalog", extra_globals=ns)
    catalog = ns["_decrypted_public_catalog"]

    now = 1_000_000_000_000
    first = asyncio.run(catalog(None, now))
    assert [r["data"]["payload"] for r in first] == ["a"]
    assert set(ns["_CATALOG_ROW_DECRYPT_MEMO"]) == {"cipher:a"}
