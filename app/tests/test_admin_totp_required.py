#!/usr/bin/env python3

import ast
import asyncio
from pathlib import Path
from urllib.parse import parse_qs, quote, urlparse


ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"


def _load(names, extra_globals):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"), filename=str(ENTRY))
    selected = [
        node for node in tree.body
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = dict(extra_globals)
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace


def _admin_namespace(records, admins):
    async def blind_index(_env, value):
        return "bi:" + value

    async def d1_first(_env, sql, name_bi):
        assert sql == "SELECT is_admin FROM users WHERE user_bi=?"
        name = name_bi[len("bi:"):]
        return {"is_admin": 1 if name in admins else 0}

    async def _account_row(_env, name):
        rec = records.get(name)
        return "bi:" + name, dict(rec) if rec else None

    async def ed25519_verify(pubkey, sig, _canonical):
        return pubkey == "admin-key" and sig == "good"

    return _load(
        {"_is_admin_flag", "_is_admin", "_admin_authorized"},
        {
            "blind_index": blind_index,
            "d1_first": d1_first,
            "_account_row": _account_row,
            "ed25519_verify": ed25519_verify,
            "_ts_ok": lambda ts: ts == "now",
        },
    )


def _records():
    return {
        "enrolled": {"name": "enrolled", "status": "active",
                     "pubkey": "admin-key", "totp_enrolled": True},
        "unenrolled": {"name": "unenrolled", "status": "active",
                       "pubkey": "admin-key"},
        "member": {"name": "member", "status": "active",
                   "pubkey": "admin-key", "totp_enrolled": True},
    }


def test_admin_rights_need_an_enrolled_authenticator():
    ns = _admin_namespace(_records(), {"enrolled", "unenrolled"})

    async def scenario():
        return (
            await ns["_is_admin"](None, "Enrolled"),
            await ns["_is_admin"](None, "unenrolled"),
            await ns["_is_admin"](None, "member"),
            await ns["_is_admin"](None, ""),
            await ns["_is_admin_flag"](None, "unenrolled"),
        )

    assert asyncio.run(scenario()) == (True, False, False, False, True)


def test_signed_admin_requests_need_an_enrolled_authenticator():
    ns = _admin_namespace(_records(), {"enrolled", "unenrolled"})
    authorized = ns["_admin_authorized"]

    async def scenario():
        return (
            await authorized(None, "enrolled", "now", "good", b"x"),
            await authorized(None, "unenrolled", "now", "good", b"x"),
            await authorized(None, "member", "now", "good", b"x"),
            await authorized(None, "enrolled", "now", "bad", b"x"),
        )

    enrolled, unenrolled, member, bad_sig = asyncio.run(scenario())
    assert enrolled and enrolled["name"] == "enrolled"
    assert unenrolled is None
    assert member is None
    assert bad_sig is None


def test_otpauth_uri_names_forkmesh_and_the_account():
    ns = _load({"_totp_otpauth_uri"}, {"quote": quote})

    uri = ns["_totp_otpauth_uri"]("alice node", "ABCDEF")

    parsed = urlparse(uri)
    assert parsed.scheme == "otpauth"
    assert parsed.netloc == "totp"
    assert parsed.path == "/ForkMesh:alice%20node"
    assert parse_qs(parsed.query) == {
        "secret": ["ABCDEF"], "issuer": ["ForkMesh"], "algorithm": ["SHA1"],
        "digits": ["6"], "period": ["30"],
    }


def test_admin_target_protection_reads_the_raw_flag():
    text = ENTRY.read_text(encoding="utf-8")
    assert "await _is_admin(env, target)" not in text
    assert text.count("await _is_admin_flag(env, target)") == 2
