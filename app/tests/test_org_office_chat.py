import asyncio
import hashlib
import importlib
import json
import sqlite3
import sys

import pytest

ORG = "o" * 64


def _module(db, revoked, notified):
    sys.modules.pop("org_office_chat", None)
    module = importlib.import_module("org_office_chat")
    counter = {"n": 0}

    async def d1_all(env, sql, *args):
        return [dict(r) for r in db.execute(sql, args).fetchall()]

    async def d1_first(env, sql, *args):
        row = db.execute(sql, args).fetchone()
        return dict(row) if row else None

    async def d1_run(env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def encrypt_row(env, record):
        return "sealed:" + json.dumps(record, sort_keys=True)

    async def decrypt_row(env, stored):
        return json.loads(stored[7:]) if str(stored).startswith("sealed:") else None

    async def blind_index(env, value):
        return hashlib.sha256(value.encode()).hexdigest()

    def new_world_peer_id():
        counter["n"] += 1
        return "%032x" % counter["n"]

    async def revoke(env, channel_id, version):
        revoked.append((channel_id, version))

    async def notify_account_event(env, name, topic):
        notified.append((name, topic))

    async def _org_row(env, name):
        return ORG, {"name": name}

    class Date:
        @staticmethod
        def now():
            return 1_800_000_000_000

    module._bind_runtime({
        "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
        "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
        "blind_index": blind_index, "new_world_peer_id": new_world_peer_id,
        "_revoke_chat_channel_room": revoke,
        "_revoke_office_channel_room": revoke,
        "notify_account_event": notify_account_event, "_org_row": _org_row,
        "Date": Date,
    })
    return module


@pytest.fixture
def world():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript("""
        CREATE TABLE chat_channels (channel_id TEXT PRIMARY KEY, name_bi TEXT NOT NULL UNIQUE,
            data TEXT NOT NULL, created_by_bi TEXT NOT NULL, created_at INTEGER NOT NULL,
            updated_at INTEGER NOT NULL, key_version INTEGER NOT NULL DEFAULT 1);
        CREATE TABLE chat_channel_members (channel_id TEXT NOT NULL, member_bi TEXT NOT NULL,
            data TEXT NOT NULL, invited_by_bi TEXT NOT NULL, joined_at INTEGER NOT NULL,
            PRIMARY KEY (channel_id, member_bi));
        CREATE TRIGGER rotate AFTER DELETE ON chat_channel_members BEGIN
            UPDATE chat_channels SET key_version = key_version + 1 WHERE channel_id = OLD.channel_id;
        END;
        CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT, name TEXT, created_at INTEGER);
        CREATE TABLE org_teams (org_bi TEXT, team TEXT, permission TEXT);
        CREATE TABLE org_team_members (org_bi TEXT, team TEXT, member_bi TEXT, name TEXT);
        CREATE TABLE org_office_channels (org_bi TEXT NOT NULL, room TEXT NOT NULL,
            channel_id TEXT NOT NULL UNIQUE, created_at INTEGER NOT NULL, PRIMARY KEY (org_bi, room));
    """)
    for bi, role, name in (("a" * 64, "owner", "alice"), ("b" * 64, "member", "bob"),
                           ("d" * 64, "member", "dave")):
        db.execute("INSERT INTO org_members VALUES (?,?,?,?,0)", (ORG, bi, role, name))
    db.execute("INSERT INTO org_teams VALUES (?,?,?)", (ORG, "engineering", "write"))
    db.execute("INSERT INTO org_team_members VALUES (?,?,?,?)", (ORG, "engineering", "b" * 64, "bob"))
    db.commit()
    revoked, notified = [], []
    return {"db": db, "m": _module(db, revoked, notified), "revoked": revoked,
            "notified": notified}


def _members(db, channel_id):
    return {r[0] for r in db.execute(
        "SELECT member_bi FROM chat_channel_members WHERE channel_id=?", (channel_id,))}


def test_channels_are_created_once_and_follow_org_and_team_membership(world):
    db, m = world["db"], world["m"]
    mapped = asyncio.run(m.sync(None, ORG, "acme"))
    assert sorted(mapped) == sorted(m.ROOMS)
    assert asyncio.run(m.sync(None, ORG, "acme")) == mapped
    assert db.execute("SELECT COUNT(*) FROM chat_channels").fetchone()[0] == 6
    everyone = {"a" * 64, "b" * 64, "d" * 64}
    assert _members(db, mapped["general"]) == everyone
    assert _members(db, mapped["engineering"]) == {"a" * 64, "b" * 64}
    assert _members(db, mapped["design"]) == {"a" * 64}
    row = db.execute("SELECT data, created_by_bi FROM chat_channels WHERE channel_id=?",
                     (mapped["general"],)).fetchone()
    record = json.loads(row[0][7:])
    assert record["kind"] == "office" and record["name"] == "office/acme/general"
    assert record["visibility"] == "private" and row[1] == ORG
    assert asyncio.run(m.rooms_for(None, ORG, "d" * 64, mapped)) == {
        room: cid for room, cid in mapped.items() if room not in ("engineering", "design")}


def test_leaving_the_org_or_team_removes_access_and_rotates_keys(world):
    db, m = world["db"], world["m"]
    mapped = asyncio.run(m.sync(None, ORG, "acme"))
    db.execute("DELETE FROM org_team_members WHERE member_bi=?", ("b" * 64,))
    db.execute("DELETE FROM org_members WHERE member_bi=?", ("d" * 64,))
    db.commit()
    world["notified"].clear()
    asyncio.run(m.resync_org(None, "acme"))
    assert _members(db, mapped["engineering"]) == {"a" * 64}
    assert "d" * 64 not in _members(db, mapped["general"])
    version = db.execute("SELECT key_version FROM chat_channels WHERE channel_id=?",
                         (mapped["engineering"],)).fetchone()[0]
    assert version == 2
    assert (mapped["engineering"], 1) in world["revoked"]
    assert ("bob", "private-channels") in world["notified"]
    assert ("dave", "private-channels") in world["notified"]


def test_resync_never_creates_channels_for_an_org_without_an_office(world):
    asyncio.run(world["m"].resync_org(None, "acme"))
    assert world["db"].execute("SELECT COUNT(*) FROM chat_channels").fetchone()[0] == 0


def test_a_user_channel_with_the_same_name_is_never_adopted(world):
    db, m = world["db"], world["m"]
    name_bi = hashlib.sha256(b"chat-channel-name:office/acme/general").hexdigest()
    db.execute("INSERT INTO chat_channels VALUES (?,?,?,?,0,0,1)",
               ("f" * 32, name_bi, "sealed:{}", "e" * 64))
    db.commit()
    mapped = asyncio.run(m.sync(None, ORG, "acme"))
    assert "general" not in mapped
    assert _members(db, "f" * 32) == set()


def test_dissolving_the_org_removes_its_office_channels_and_members(world):
    db, m = world["db"], world["m"]
    db.executescript("""
        CREATE TABLE chat_channel_webhooks (channel_id TEXT, webhook_id TEXT);
        CREATE TABLE chat_channel_invites (channel_id TEXT, code TEXT);
        CREATE TABLE chat_channel_reads (channel_id TEXT, account_bi TEXT);
    """)
    db.execute("INSERT INTO chat_channels VALUES (?,?,?,?,0,0,1)",
               ("f" * 32, "user-channel", "sealed:{}", "e" * 64))
    mapped = asyncio.run(m.sync(None, ORG, "acme"))
    for channel_id in mapped.values():
        db.execute("INSERT INTO chat_channel_reads VALUES (?,?)",
                   (channel_id, "b" * 64))
    db.commit()
    world["notified"].clear()
    assert asyncio.run(m.dissolve(None, ORG)) == len(m.ROOMS)
    assert db.execute("SELECT COUNT(*) FROM org_office_channels").fetchone()[0] == 0
    assert [r[0] for r in db.execute("SELECT channel_id FROM chat_channels")] == [
        "f" * 32]
    for table in ("chat_channel_members", "chat_channel_reads"):
        assert db.execute("SELECT COUNT(*) FROM %s" % table).fetchone()[0] == 0
    assert {cid for cid, _version in world["revoked"]} >= set(mapped.values())
    assert ("dave", "private-channels") in world["notified"]


def test_organization_delete_dissolves_the_office_before_the_roster():
    from pathlib import Path
    entry = (Path(__file__).resolve().parents[1] / "src" / "entry.py").read_text(
        encoding="utf-8")
    start = entry.index('"organization.dissolve"):')
    handler = entry[start:entry.index('DELETE FROM orgs WHERE org_bi=?', start)]
    assert handler.index("await org_office_chat.dissolve(env, org_bi)") < handler.index(
        '"DELETE FROM org_members WHERE org_bi=?"')
