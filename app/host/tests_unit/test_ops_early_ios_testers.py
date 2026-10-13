import asyncio
import json
import sqlite3
import sys
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from forkmesh_host import ops  # noqa: E402


def runtime(users):
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE users (username TEXT, data TEXT, "
               "early_ios_tester INTEGER NOT NULL DEFAULT 0)")
    for name, flag, rec in users:
        db.execute("INSERT INTO users VALUES (?,?,?)", (name, json.dumps(rec), flag))

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args)]

    async def decrypt_row(_env, stored):
        return json.loads(stored)

    return SimpleNamespace(env=object(),
                           entry=SimpleNamespace(d1_all=d1_all, decrypt_row=decrypt_row))


def verified(email, **extra):
    return {"status": "active", "email": email, "email_verified": True, **extra}


def test_lists_flagged_active_verified_testers_only():
    result = asyncio.run(ops.COMMANDS["early-ios-testers"](runtime([
        ("bob", 1, verified(" Bob@Example.com ")),
        ("ada", 1, verified("ada@example.com")),
        ("off", 0, verified("off@example.com")),
        ("cy", 1, {"status": "active", "email": "cy@example.com"}),
        ("dee", 1, verified("dee@example.com", status="suspended")),
        ("eve", 1, verified("")),
    ]), "all"))
    assert result["ok"] is True
    assert result["testers"] == [{"account": "ada", "email": "ada@example.com"},
                                 {"account": "bob", "email": "bob@example.com"}]
    assert result["skipped"] == [
        {"account": "cy", "reason": "email_not_verified"},
        {"account": "dee", "reason": "not_active"},
        {"account": "eve", "reason": "no_email"},
    ]


def test_the_command_rides_the_vault_socket_shape():
    assert "early-ios-testers" in ops.COMMANDS
    assert len("ops early-ios-testers all".split()) == 3


def pinging(users, muted=()):
    rt = runtime(users)
    filed = []

    async def enqueue_notification(_env, recipient, kind, title, **fields):
        if recipient in muted:
            return False
        filed.append(dict(fields, recipient=recipient, kind=kind, title=title))
        return True

    rt.entry.enqueue_notification = enqueue_notification
    return rt, filed


DIGEST = "ab" * 32


def test_ios_build_ping_files_a_ping_with_the_install_link_per_tester():
    rt, filed = pinging([
        ("ada", 1, {"status": "active"}),
        ("bob", 1, verified("bob@example.com")),
        ("off", 0, verified("off@example.com")),
        ("dee", 1, {"status": "suspended"}),
        ("mute", 1, {"status": "active"}),
    ], muted=("mute",))
    result = asyncio.run(ops.COMMANDS["ios-build-ping"](
        rt, DIGEST + ":1.4.0:21:c3f94cc775e67190"))
    page = ("https://forkmesh.com/ios-install.html?build=21&v=" + DIGEST[:12]
            + "#" + DIGEST)
    assert result == {"ok": True, "page": page, "pinged": ["ada", "bob"], "skipped": [
        {"account": "dee", "reason": "not_active"},
        {"account": "mute", "reason": "not_filed"}]}
    ping = filed[0]
    assert ping["kind"] == "release_published" and ping["href"] == page
    assert ping["title"] == "iPhone build 1.4.0 (21) is ready"
    assert page in ping["body"] and "c3f94cc775e6." in ping["body"]
    assert ping["dedupe"] == "ios-build:" + DIGEST


def test_ios_build_ping_refuses_a_bad_spec():
    rt, filed = pinging([("ada", 1, {"status": "active"})])
    for spec in ("all", DIGEST, DIGEST + ":1.4 0:21:", "zz" * 32 + ":1:2:"):
        result = asyncio.run(ops.COMMANDS["ios-build-ping"](rt, spec))
        assert result["ok"] is False and result["error"] == "bad_spec"
    assert filed == []
    assert len(("ops ios-build-ping " + DIGEST + ":1.4.0:21:").split()) == 3
