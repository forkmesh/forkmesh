#!/usr/bin/env python3
"""New accounts get users.signup_origin stamped at signup (migration 0175)."""

import ast
import asyncio
import sys
from pathlib import Path
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY = SRC / "entry.py"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import product_analysis  # noqa: E402


def _namespace(run=None):
    selected = [
        node for node in ast.parse(ENTRY.read_text(encoding="utf-8")).body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_record_signup_origin"
    ]
    assert len(selected) == 1
    writes = []

    async def d1_run(_env, sql, *args):
        if run is not None:
            return await run(sql, *args)
        writes.append((sql, args))
        return SimpleNamespace()

    namespace = {
        "classify_signup_origin": product_analysis.classify_signup_origin,
        "d1_run": d1_run,
    }
    module = ast.fix_missing_locations(ast.Module(selected, type_ignores=[]))
    exec(compile(module, str(ENTRY), "exec"), namespace)
    namespace["_writes"] = writes
    return namespace


def test_signup_calls_the_stamp_right_after_the_account_is_saved():
    text = ENTRY.read_text(encoding="utf-8")
    signup = text[text.index("async def _account_signup(env, request):"):]
    signup = signup[:signup.index("\n\n\nasync def ")]
    saved = signup.index(
        "await _save_account(env, name_bi, rec, email_bi=email_bi, ip_bi=ip_bi)")
    stamped = signup.index("await _record_signup_origin(env, name_bi, rec)")
    assert saved < stamped
    # The marker the stamp reads is written before the save.
    assert signup.index('rec["signup"] = signup_meta') < saved


def test_direct_and_referred_signups_are_stamped_unknown_is_left_alone():
    ns = _namespace()
    asyncio.run(ns["_record_signup_origin"](
        None, "bi-1", {"signup": {"referralCaptured": True, "referrer": ""}}))
    asyncio.run(ns["_record_signup_origin"](
        None, "bi-2", {"signup": {"referralCaptured": True, "referrer": "ann"}}))
    asyncio.run(ns["_record_signup_origin"](
        None, "bi-3", {"signup": {"referralCaptured": False, "referrer": ""}}))
    asyncio.run(ns["_record_signup_origin"](None, "bi-4", {}))
    assert ns["_writes"] == [
        ("UPDATE users SET signup_origin=? WHERE user_bi=?", ("organic", "bi-1")),
        ("UPDATE users SET signup_origin=? WHERE user_bi=?", ("referral", "bi-2")),
    ]


def test_a_database_behind_the_migration_does_not_fail_the_signup():
    async def missing_column(sql, *args):
        raise RuntimeError("no such column: signup_origin")

    ns = _namespace(run=missing_column)
    asyncio.run(ns["_record_signup_origin"](
        None, "bi-1", {"signup": {"referralCaptured": True, "referrer": ""}}))
