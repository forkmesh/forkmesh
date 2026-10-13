"""Schema and route contracts for user-managed chat channels."""

from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import schema  # noqa: E402
import urls  # noqa: E402


def test_private_chat_channel_migration_and_lazy_schema_match():
    migration = ROOT / "migrations" / "0069_private_chat_channels.sql"
    assert migration.exists()
    sql = migration.read_text(encoding="utf-8")
    joined = "\n".join(schema.SCHEMA_STATEMENTS)
    for marker in (
        "CREATE TABLE IF NOT EXISTS chat_channels",
        "CREATE TABLE IF NOT EXISTS chat_channel_members",
        "idx_chat_channel_members_member",
        "trg_chat_channel_member_remove_rotate",
    ):
        assert marker in sql
        assert marker in joined


def test_chat_channel_webhook_migration_and_lazy_schema_match():
    migration = ROOT / "migrations" / "0132_chat_channel_webhooks.sql"
    assert migration.exists()
    sql = migration.read_text(encoding="utf-8")
    joined = "\n".join(schema.SCHEMA_STATEMENTS)
    for marker in (
        "CREATE TABLE IF NOT EXISTS chat_channel_webhooks",
        "idx_chat_channel_webhooks_channel",
    ):
        assert marker in sql
        assert marker in joined
    assert "token_bi" in sql and "NOT NULL UNIQUE" in sql
    assert "token_bi TEXT NOT NULL UNIQUE" in joined


def test_chat_channel_invite_migration_and_lazy_schema_match():
    migration = ROOT / "migrations" / "0254_chat_channel_invites.sql"
    sql = migration.read_text(encoding="utf-8")
    joined = "\n".join(schema.SCHEMA_STATEMENTS)
    assert "CREATE TABLE IF NOT EXISTS chat_channel_invites" in sql
    assert "CREATE TABLE IF NOT EXISTS chat_channel_invites" in joined
    assert "token_bi TEXT NOT NULL UNIQUE" in joined
    assert urls.CHAT_CHANNEL_INVITE_RE.fullmatch(
        "/api/chat/channels/" + "a" * 32 + "/invite")
    assert urls.CHAT_INVITE_RE.fullmatch("/api/chat/invites/" + "b" * 64)


def test_member_deletion_rotates_the_channel_key_once():
    sql = (
        ROOT / "migrations" / "0069_private_chat_channels.sql"
    ).read_text(encoding="utf-8")
    db = sqlite3.connect(":memory:")
    db.executescript(sql)
    channel_id = "a" * 32
    db.execute(
        "INSERT INTO chat_channels "
        "(channel_id,name_bi,data,created_by_bi,created_at,updated_at,key_version) "
        "VALUES (?,?,?,?,?,?,?)",
        (channel_id, "name-bi", "sealed", "admin-bi", 1, 1, 1),
    )
    db.execute(
        "INSERT INTO chat_channel_members "
        "(channel_id,member_bi,data,invited_by_bi,joined_at) "
        "VALUES (?,?,?,?,?)",
        (channel_id, "alice-bi", "sealed-member", "admin-bi", 2),
    )
    db.execute(
        "DELETE FROM chat_channel_members WHERE channel_id=? AND member_bi=?",
        (channel_id, "alice-bi"),
    )
    db.execute(
        "DELETE FROM chat_channel_members WHERE channel_id=? AND member_bi=?",
        (channel_id, "alice-bi"),
    )
    version = db.execute(
        "SELECT key_version FROM chat_channels WHERE channel_id=?",
        (channel_id,),
    ).fetchone()[0]
    assert version == 2


def test_private_chat_channel_routes_are_exact_and_opaque():
    channel_id = "a" * 32
    assert urls.CHAT_CHANNELS_RE.fullmatch("/api/chat/channels")
    assert urls.CHAT_CHANNEL_MEMBERS_RE.fullmatch(
        f"/api/chat/channels/{channel_id}/members"
    )
    assert urls.CHAT_CHANNEL_ROOM_ACCESS_RE.fullmatch(
        f"/api/chat/channels/{channel_id}/room-access"
    )
    assert urls.CHAT_CHANNEL_WS_RE.fullmatch(
        f"/api/chat/channels/{channel_id}/ws"
    )
    assert urls.CHAT_CHANNEL_WEBHOOKS_RE.fullmatch(
        f"/api/chat/channels/{channel_id}/webhooks"
    )
    assert urls.CHAT_CHANNEL_WEBHOOK_RE.fullmatch(
        f"/api/chat/channels/{channel_id}/webhooks/{'b' * 32}"
    )
    assert urls.CHAT_WEBHOOK_DELIVERY_RE.fullmatch(
        f"/api/chat/webhooks/{'c' * 64}"
    )
    assert not urls.CHAT_CHANNEL_WS_RE.fullmatch(
        "/api/chat/channels/release-team/ws"
    )
