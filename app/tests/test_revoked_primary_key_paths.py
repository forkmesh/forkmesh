#!/usr/bin/env python3

import ast
import asyncio
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[1] / "src"
ENTRY = SRC / "entry.py"
SIGNATURE_MODULES = ("entry.py", "route_handlers.py", "microjobs_runtime.py")
RECORD_PUBKEY = re.compile(
    r"\b(rec|node_rec|account_rec|record|owner_rec|target_rec)\b\s*"
    r"(?:or \{\}\)\s*)?(?:\.get\(\"pubkey\"|\[\"pubkey\"\])")
REFUSAL = ("_trusted_primary_pubkey(", "_account_key_revoked(",
           "owner_pub not in refused")
NOT_A_TRUST_DECISION = {
    ("entry.py", "_account_reserve"),
    ("entry.py", "_account_browser_control_bind"),
    ("entry.py", "_account_login"),
    ("entry.py", "_account_drop_revoked_primary"),
    ("entry.py", "_trusted_primary_pubkey"),
}


def _functions(path):
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    for node in ast.walk(ast.parse(text, filename=str(path))):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            body = "".join(lines[node.lineno - 1:node.end_lineno])
            yield node.name, re.sub(r"#[^\n]*", "", body)


def test_every_primary_key_signature_check_refuses_a_revoked_key():
    checked = set()
    for module in SIGNATURE_MODULES:
        for name, body in _functions(SRC / module):
            if not RECORD_PUBKEY.search(body):
                continue
            if "ed25519_verify(" not in body and "pubkey" not in name:
                continue
            if (module, name) in NOT_A_TRUST_DECISION:
                continue
            checked.add((module, name))
            assert any(marker in body for marker in REFUSAL), (module, name)
    bodies = {(module, name): body for module in SIGNATURE_MODULES
              for name, body in _functions(SRC / module)}
    for site in (
        ("entry.py", "_owner_pubkey"),
        ("entry.py", "_account_link_node"),
        ("entry.py", "_account_link_self"),
        ("entry.py", "_account_link_grant"),
        ("entry.py", "_account_ownership_transfer_confirm"),
        ("entry.py", "_contribution_ingest_snapshot"),
        ("entry.py", "subscribe_handler"),
        ("entry.py", "_account_heartbeat"),
        ("entry.py", "_admin_authorized"),
        ("entry.py", "_account_rotate"),
        ("entry.py", "_account_signing_pubkeys"),
        ("route_handlers.py", "_account_finalize"),
        ("microjobs_runtime.py", "account_owns_node_key"),
    ):
        assert any(marker in bodies[site] for marker in REFUSAL), site
    profile = bodies[("entry.py", "accounts_handler")]
    assert '"pubkey": await _trusted_primary_pubkey(env, name_bi, rec),' in profile


def test_owner_pubkey_hides_a_primary_key_whose_device_was_revoked():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    wanted = {"_owner_pubkey", "_trusted_primary_pubkey",
              "_account_key_revoked"}
    selected = [node for node in tree.body
                if isinstance(node, ast.AsyncFunctionDef)
                and node.name in wanted]
    assert {node.name for node in selected} == wanted
    revoked = set()

    async def _account_row(_env, name):
        return "bi:" + name, {"name": name, "pubkey": "PK-" + name}

    async def d1_first(_env, _sql, account_bi, pubkey):
        if (account_bi, pubkey) in revoked:
            return {"enabled": 0, "revoked_at": 1}
        return {"enabled": 1, "revoked_at": 0}

    namespace = {
        "_account_row": _account_row,
        "d1_first": d1_first,
        "clean_string": lambda value, limit=240: str(value or "")[:limit],
    }
    exec(compile(ast.Module(body=selected, type_ignores=[]), str(ENTRY),
                 "exec"), namespace)
    owner_pubkey = namespace["_owner_pubkey"]
    assert asyncio.run(owner_pubkey(None, "alice")) == "PK-alice"
    revoked.add(("bi:alice", "PK-alice"))
    assert asyncio.run(owner_pubkey(None, "alice")) == ""
