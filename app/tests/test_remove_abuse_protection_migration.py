"""Persistent-state contracts for removing automated abuse quarantine."""

import sqlite3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCHEMA = ROOT / "src" / "schema.py"
INSTALL = ROOT / "migrations" / "0043_security_control_plane.sql"
REMOVE = ROOT / "migrations" / "0068_remove_automated_abuse_protection.sql"
REMOVED_TABLES = (
    "security_appeals",
    "security_restrictions",
    "security_signals",
)
PRESERVED_TABLES = (
    "role_grants",
    "sensitive_audit_log",
    "owner_encryption_keys",
)


def _tables(connection):
    return {
        row[0]
        for row in connection.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )
    }


def test_lazy_schema_does_not_recreate_retired_abuse_tables():
    schema = SCHEMA.read_text(encoding="utf-8")
    for table in REMOVED_TABLES:
        assert f"CREATE TABLE IF NOT EXISTS {table}" not in schema
    for table in PRESERVED_TABLES:
        assert f"CREATE TABLE IF NOT EXISTS {table}" in schema


def test_removal_migration_is_ordered_idempotent_and_preserves_other_planes():
    migration = REMOVE.read_text(encoding="utf-8")
    statements = [
        f"DROP TABLE IF EXISTS {table};" for table in REMOVED_TABLES
    ]
    assert [migration.index(statement) for statement in statements] == sorted(
        migration.index(statement) for statement in statements
    )

    connection = sqlite3.connect(":memory:")
    connection.executescript(INSTALL.read_text(encoding="utf-8"))
    connection.execute(
        """
        INSERT INTO role_grants (
            account_bi, role, granted_at
        ) VALUES ('account', 'moderator', 1)
        """
    )
    connection.execute(
        """
        INSERT INTO sensitive_audit_log (
            ts, action, outcome
        ) VALUES (1, 'account-role-granted', 'success')
        """
    )
    connection.execute(
        """
        INSERT INTO owner_encryption_keys (
            account_bi, key_id, public_bundle, created_at
        ) VALUES ('account', 'key', 'public-only', 1)
        """
    )

    connection.executescript(migration)
    connection.executescript(migration)

    tables = _tables(connection)
    assert set(REMOVED_TABLES).isdisjoint(tables)
    assert set(PRESERVED_TABLES) <= tables
    assert connection.execute("SELECT COUNT(*) FROM role_grants").fetchone() == (
        1,
    )
    assert connection.execute(
        "SELECT COUNT(*) FROM sensitive_audit_log"
    ).fetchone() == (1,)
    assert connection.execute(
        "SELECT COUNT(*) FROM owner_encryption_keys"
    ).fetchone() == (1,)
