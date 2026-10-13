"""Schema and route contracts for one-to-one web direct messages."""

from pathlib import Path
import sqlite3
import sys


ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import schema  # noqa: E402
import urls  # noqa: E402

ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")


def test_direct_message_migration_and_lazy_schema_match():
    create_migration = (
        ROOT / "migrations" / "0079_chat_direct_messages.sql"
    )
    limits_migration = (
        ROOT / "migrations" / "0084_chat_direct_message_limits.sql"
    )
    assert create_migration.exists()
    assert limits_migration.exists()
    sql = create_migration.read_text(encoding="utf-8") + "\n"
    sql += limits_migration.read_text(encoding="utf-8")
    joined = "\n".join(schema.SCHEMA_STATEMENTS)
    for marker in (
        "CREATE TABLE IF NOT EXISTS chat_direct_conversations",
        "CREATE TABLE IF NOT EXISTS chat_direct_participants",
        "idx_chat_direct_participants_account",
        "message_count",
        "last_read_count",
        "initiated",
        "trg_chat_direct_creation_rate",
    ):
        assert marker in sql
        assert marker in joined


def test_lazy_upgrade_adds_direct_message_columns_before_dependent_indexes():
    start = ENTRY.index("SCHEMA_PRE_CREATE_ALTER_STATEMENTS = [")
    end = ENTRY.index("\n]\n\n# Post-CREATE", start)
    pre_create = ENTRY[start:end]
    for column in ("message_count", "last_read_count", "initiated"):
        assert f"ADD COLUMN {column}" in pre_create


def test_direct_message_schema_enforces_one_pair_and_two_unique_participants():
    create_sql = (
        ROOT / "migrations" / "0079_chat_direct_messages.sql"
    ).read_text(encoding="utf-8")
    limits_sql = (
        ROOT / "migrations" / "0084_chat_direct_message_limits.sql"
    ).read_text(encoding="utf-8")
    db = sqlite3.connect(":memory:")
    db.executescript(create_sql)
    db.executescript(limits_sql)
    conversation_id = "a" * 32
    db.execute(
        "INSERT INTO chat_direct_conversations "
        "(conversation_id,pair_bi,data,created_at,updated_at,key_version,"
        "message_count) VALUES (?,?,?,?,?,1,0)",
        (conversation_id, "pair-bi", "sealed", 1, 1),
    )
    db.execute(
        "INSERT INTO chat_direct_participants "
        "(conversation_id,participant_bi,data,joined_at) VALUES (?,?,?,?)",
        (conversation_id, "alice-bi", "sealed-alice", 1),
    )

    try:
        db.execute(
            "INSERT INTO chat_direct_conversations "
            "(conversation_id,pair_bi,data,created_at,updated_at,key_version,"
            "message_count) VALUES (?,?,?,?,?,1,0)",
            ("b" * 32, "pair-bi", "sealed", 1, 1),
        )
    except sqlite3.IntegrityError:
        pass
    else:
        raise AssertionError("participant pair must be unique")

    try:
        db.execute(
            "INSERT INTO chat_direct_participants "
            "(conversation_id,participant_bi,data,joined_at) "
            "VALUES (?,?,?,?)",
            (conversation_id, "alice-bi", "sealed-alice", 1),
        )
    except sqlite3.IntegrityError:
        pass
    else:
        raise AssertionError("conversation participant must be unique")


def test_direct_message_routes_are_exact_and_opaque():
    conversation_id = "a" * 32
    assert urls.CHAT_DIRECT_MESSAGES_RE.fullmatch(
        "/api/chat/direct-messages"
    )
    assert urls.CHAT_DIRECT_MESSAGE_ROOM_ACCESS_RE.fullmatch(
        f"/api/chat/direct-messages/{conversation_id}/room-access"
    )
    assert urls.CHAT_DIRECT_MESSAGE_WS_RE.fullmatch(
        f"/api/chat/direct-messages/{conversation_id}/ws"
    )
    assert not urls.CHAT_DIRECT_MESSAGE_WS_RE.fullmatch(
        "/api/chat/direct-messages/alice/ws"
    )
    assert urls.CHAT_DIRECT_MESSAGE_USERS_RE.fullmatch(
        "/api/chat/direct-messages/users"
    )
    assert urls.CHAT_DIRECT_MESSAGE_READ_RE.fullmatch(
        f"/api/chat/direct-messages/{conversation_id}/read"
    )


def test_direct_message_schema_atomically_enforces_creation_rate():
    create_sql = (
        ROOT / "migrations" / "0079_chat_direct_messages.sql"
    ).read_text(encoding="utf-8")
    limits_sql = (
        ROOT / "migrations" / "0084_chat_direct_message_limits.sql"
    ).read_text(encoding="utf-8")
    db = sqlite3.connect(":memory:")
    db.executescript(create_sql)
    db.executescript(limits_sql)
    for index in range(20):
        db.execute(
            "INSERT INTO chat_direct_participants "
            "(conversation_id,participant_bi,data,joined_at,initiated) "
            "VALUES (?,?,?,?,1)",
            (f"{index:032x}", "alice-bi", "sealed", 1000 + index),
        )
    try:
        db.execute(
            "INSERT INTO chat_direct_participants "
            "(conversation_id,participant_bi,data,joined_at,initiated) "
            "VALUES (?,?,?,?,1)",
            ("f" * 32, "alice-bi", "sealed", 2000),
        )
    except sqlite3.IntegrityError as error:
        assert "chat_direct_creation_rate_limited" in str(error)
    else:
        raise AssertionError("creation rate trigger must reject request 21")
