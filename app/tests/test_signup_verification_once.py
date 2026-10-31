#!/usr/bin/env python3
import ast
import asyncio
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ROUTES = ROOT / "src" / "route_handlers.py"
TEXT = ENTRY.read_text(encoding="utf-8")
FUNCS = {"_claim_signup_verification", "_send_signup_verification"}


class _Meta:
    def __init__(self, changes):
        self.changes = changes


class _Result:
    def __init__(self, changes):
        self.meta = _Meta(changes)


def _harness(sent=True, d1_error=False):
    claims = set()
    sends = []
    queued = []

    class _DateStub:
        @staticmethod
        def now():
            return 2_000_000_000_000

    async def blind_index(_env, value):
        return "bi:" + value

    async def d1_run(_env, sql, key, created_at):
        if d1_error:
            raise RuntimeError("d1 down")
        assert "INSERT OR IGNORE INTO notification_email_sends" in sql
        if key in claims:
            return _Result(0)
        claims.add(key)
        return _Result(1)

    async def _send_verification_email(_env, _request, name, email, rec=None):
        await asyncio.sleep(0)
        sends.append((name, email))
        return sent

    async def _enqueue_verification(_env, name_bi, name, email):
        queued.append((name_bi, name, email))

    tree = ast.parse(TEXT, filename=str(ENTRY))
    selected = [node for node in tree.body
                if isinstance(node, ast.AsyncFunctionDef)
                and node.name in FUNCS]
    assert {node.name for node in selected} == FUNCS
    namespace = {
        "Date": _DateStub,
        "MAX_NODE_NAME": 63,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "blind_index": blind_index,
        "d1_run": d1_run,
        "_send_verification_email": _send_verification_email,
        "_enqueue_verification": _enqueue_verification,
    }
    module = ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    namespace["_sends"] = sends
    namespace["_queued"] = queued
    return namespace


def _send(ns, rec, name="alice"):
    return asyncio.run(
        ns["_send_signup_verification"](None, None, "bi:" + name, name, rec))


def test_repeated_signup_sends_one_email():
    ns = _harness()
    rec = {"email": "Alice@Example.org", "email_verified": False}
    assert _send(ns, dict(rec)) is True
    assert _send(ns, dict(rec)) is False
    assert _send(ns, {"email": "alice@example.org"}) is False
    assert ns["_sends"] == [("alice", "Alice@Example.org")]


def test_concurrent_requests_send_one_email():
    ns = _harness()
    rec = {"email": "alice@example.org", "email_verified": False}

    async def both():
        return await asyncio.gather(*(
            ns["_send_signup_verification"](None, None, "bi:alice", "alice",
                                            dict(rec))
            for _ in range(3)))

    assert sorted(asyncio.run(both())) == [False, False, True]
    assert len(ns["_sends"]) == 1


def test_a_new_address_or_account_gets_its_own_email():
    ns = _harness()
    _send(ns, {"email": "alice@example.org"})
    _send(ns, {"email": "alice@other.example"})
    _send(ns, {"email": "alice@example.org"}, name="bob")
    assert ns["_sends"] == [("alice", "alice@example.org"),
                            ("alice", "alice@other.example"),
                            ("bob", "alice@example.org")]


def test_verified_or_missing_address_sends_nothing():
    ns = _harness()
    assert _send(ns, {"email": "alice@example.org",
                      "email_verified": True}) is False
    assert _send(ns, {"email": ""}) is False
    assert ns["_sends"] == []


def test_failed_send_is_queued_once():
    ns = _harness(sent=False)
    rec = {"email": "alice@example.org"}
    _send(ns, dict(rec))
    _send(ns, dict(rec))
    assert ns["_queued"] == [("bi:alice", "alice", "alice@example.org")]


def test_claim_store_outage_still_sends():
    ns = _harness(d1_error=True)
    assert _send(ns, {"email": "alice@example.org"}) is True
    assert len(ns["_sends"]) == 1


def test_signup_and_finalize_use_the_one_time_send():
    signup = TEXT[TEXT.index("async def _account_signup("):]
    signup = signup[:signup.index("\nasync def ")]
    assert "_send_signup_verification(env, request, name_bi, name, rec)" in signup
    assert "_send_verification_email(" not in signup
    routes = ROUTES.read_text(encoding="utf-8")
    finalize = routes[routes.index("async def _account_finalize("):]
    finalize = finalize[:finalize.index("\nasync def ")]
    assert "_send_signup_verification(env, request, name_bi, name, rec)" in finalize
    assert "_send_verification_email(" not in finalize
