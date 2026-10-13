import asyncio
import base64
import importlib
import sqlite3
import sys
from pathlib import Path

import pytest
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import (
    Ed25519PrivateKey, Ed25519PublicKey)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))

NOW = 1_791_500_000_000
BASE, HEAD, AFTER = "a" * 40, "b" * 40, "c" * 40


def b64(raw):
    return base64.urlsafe_b64encode(raw).decode().rstrip("=")


def unb64(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


class Env:
    SELF_DEPLOY_REPOSITORY = ""


@pytest.fixture
def module():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript((ROOT / "migrations" / "0249_deploy_approvals.sql")
                     .read_text().split("INSERT INTO schema_meta")[0])
    keys = {"jett": []}
    pings = []

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def d1_first(_env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_all(_env, sql, *args):
        return [dict(r) for r in db.execute(sql, args).fetchall()]

    async def ensure_schema(_env):
        return None

    async def owner_keys(_env, actor):
        return list(keys.get(actor, []))

    async def verify(public_key, signature, message):
        try:
            Ed25519PublicKey.from_public_bytes(unb64(public_key)).verify(
                unb64(signature), message)
            return True
        except (InvalidSignature, ValueError):
            return False

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        pings.append((recipient, kind, title, kwargs))
        return True

    class Alerts:
        @staticmethod
        async def _operator_alert_admins(_env):
            return ["root"]

    sys.modules.pop("deploy_approvals", None)
    loaded = importlib.import_module("deploy_approvals")
    loaded._bind_runtime({
        "d1_run": d1_run, "d1_first": d1_first, "d1_all": d1_all,
        "ensure_schema": ensure_schema, "_owner_signing_pubkeys": owner_keys,
        "ed25519_verify": verify, "enqueue_notification": enqueue_notification,
        "_operator_alerts": Alerts,
    })
    yield loaded, db, keys, pings
    sys.modules.pop("deploy_approvals", None)


def phone():
    key = Ed25519PrivateKey.generate()
    public = b64(key.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw))
    return key, public


def signed(loaded, key, public, kind="pull", owner="forkmesh", target="12",
           base=BASE, head=HEAD, request_id="mobile_00000001", ts=NOW):
    message = loaded.approval_message(kind, owner, "forkmesh", target, base,
                                      head, request_id, ts)
    return {"publicKey": public, "ts": ts, "signature": b64(key.sign(message))}


def verify(loaded, approval, **overrides):
    args = dict(actor="jett", kind="pull", owner="forkmesh", repo="forkmesh",
                target="12", base=BASE, head=HEAD,
                request_id="mobile_00000001", now=NOW + 1000)
    args.update(overrides)
    return asyncio.run(loaded.verify(
        Env(), args["actor"], approval, args["kind"], args["owner"],
        args["repo"], args["target"], args["base"], args["head"],
        args["request_id"], args["now"]))


def test_only_the_mergers_own_device_key_over_the_exact_merge_is_accepted(module):
    loaded, _db, keys, _pings = module
    key, public = phone()
    approval = signed(loaded, key, public)
    assert verify(loaded, approval) is None
    keys["jett"].append(public)
    approved = verify(loaded, approval)
    assert approved["publicKey"] == public and approved["actor"] == "jett"
    assert verify(loaded, approval, actor="bob") is None
    assert verify(loaded, approval, head="d" * 40) is None
    assert verify(loaded, approval, target="13") is None
    assert verify(loaded, approval, repo="other") is None
    assert verify(loaded, approval, now=NOW + loaded.MAX_SKEW_MS + 1) is None
    assert verify(loaded, dict(approval, extra=1)) is None
    assert verify(loaded, dict(approval, ts=True)) is None


def test_a_successful_merge_is_recorded_once_and_the_outcome_is_pinged(module):
    loaded, db, keys, pings = module
    key, public = phone()
    keys["jett"].append(public)
    approved = verify(loaded, signed(loaded, key, public))
    result = {"baseBefore": BASE, "head": HEAD, "baseAfter": AFTER}
    first = asyncio.run(loaded.record(Env(), approved, result, NOW))
    assert first and asyncio.run(loaded.record(Env(), approved, result, NOW)) == first
    row = dict(db.execute("SELECT * FROM deploy_approvals").fetchone())
    assert row["base_after"] == AFTER and row["status"] == "pending"
    assert db.execute("SELECT COUNT(*) FROM deploy_approvals").fetchone()[0] == 1
    assert asyncio.run(loaded.announce(Env())) == 0
    db.execute("UPDATE deploy_approvals SET status='deployed'")
    assert asyncio.run(loaded.announce(Env())) == 1
    assert {p[0] for p in pings} == {"root", "jett"}
    assert pings[0][2] == "Deployed cccccccccccc to the cores"
    assert asyncio.run(loaded.announce(Env())) == 0
    [item] = asyncio.run(loaded.recent(Env()))
    assert item["status"] == "deployed" and item["commit"] == AFTER


def test_a_pull_result_that_differs_from_the_approval_is_not_recorded(module):
    loaded, db, keys, _pings = module
    key, public = phone()
    keys["jett"].append(public)
    approved = verify(loaded, signed(loaded, key, public))
    assert asyncio.run(loaded.record(
        Env(), approved, {"baseBefore": BASE, "head": "e" * 40,
                          "baseAfter": AFTER}, NOW)) == 0
    assert db.execute("SELECT COUNT(*) FROM deploy_approvals").fetchone()[0] == 0


def test_branch_merges_sign_the_session_without_tips(module):
    loaded, _db, keys, _pings = module
    key, public = phone()
    keys["jett"].append(public)
    approval = signed(loaded, key, public, kind="branch", target="sess-1",
                      base="", head="")
    assert verify(loaded, approval, kind="branch", target="sess-1", base="",
                  head="") is not None


def test_the_merge_body_accepts_an_approval_only_for_merges():
    import ast
    entry = (ROOT / "src" / "entry.py").read_text()
    tree = ast.parse(entry)
    node = next(n for n in tree.body if isinstance(n, ast.FunctionDef)
                and n.name == "_https_mirror_merge_body")
    source = ast.get_source_segment(entry, node)
    assert '"deployApproval"' in source
    assert 'if profile.get("override")' in source
