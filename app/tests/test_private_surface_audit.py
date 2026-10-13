"""Operator-view evidence for private storage, backups, indexes, and replicas."""

from __future__ import annotations

import ast
import base64
import hashlib
import json
import re
import sqlite3
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "app" / "src"))

from private_surface_audit import (  # noqa: E402
    DECLARED_COLUMNS,
    audit_database,
    audit_replica_bytes,
)


def _schema_statements():
    tree = ast.parse((ROOT / "app/src/schema.py").read_text(encoding="utf-8"))
    assignment = next(
        node for node in tree.body
        if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == "SCHEMA_STATEMENTS"
                for target in node.targets)
    )
    return ast.literal_eval(assignment.value)


def _database(path: str = ":memory:") -> sqlite3.Connection:
    connection = sqlite3.connect(path)
    for statement in _schema_statements():
        match = re.search(
            r"CREATE TABLE IF NOT EXISTS\s+([A-Za-z0-9_]+)", statement)
        if match and match.group(1) in DECLARED_COLUMNS:
            connection.execute(statement)
    connection.execute(
        "ALTER TABLE repositories ADD COLUMN is_private INTEGER NOT NULL DEFAULT 0")
    return connection


def _cipher(seed: int = 0) -> str:
    # Structural fixture only: the keyless audit proves framing, not plaintext
    # correctness. Production WebCrypto verification remains separately tested.
    return base64.b64encode(bytes([seed % 256]) * 29).decode("ascii")


def _seed_private(connection: sqlite3.Connection) -> str:
    repo_bi = "a" * 64
    connection.execute(
        "INSERT INTO repositories(key_bi,owner_bi,is_private,data) VALUES(?,?,1,?)",
        (repo_bi, "b" * 64, _cipher()),
    )
    connection.execute(
        "INSERT INTO repo_source_authorities"
        "(repo_bi,node_id,machine_name,created_at,updated_at,is_private,data) "
        "VALUES(?,?,?,1,1,1,?)",
        (repo_bi, "", "", _cipher(1)),
    )
    connection.execute(
        "INSERT INTO repo_device_mirrors(repo_bi,node_id,data,updated_at) "
        "VALUES(?,?,?,1)",
        (repo_bi, "c" * 64, _cipher(2)),
    )
    connection.execute(
        "INSERT INTO issue_inbox(repo_bi,data,submitter_bi) VALUES(?,?,?)",
        (repo_bi, _cipher(3), "d" * 64),
    )
    connection.execute(
        "INSERT INTO notifications(dedupe_bi,recipient_bi,ts,data) VALUES(?,?,1,?)",
        ("e" * 64, "f" * 64, _cipher(4)),
    )
    connection.commit()
    return repo_bi


def _b64url(size: int, byte: int) -> str:
    return base64.urlsafe_b64encode(bytes([byte]) * size).decode().rstrip("=")


def _replica() -> bytes:
    recipient = {
        "kid": "recipient_key_1234",
        "x25519": _b64url(32, 1),
        "mlkem768": _b64url(1088, 2),
        "nonce": _b64url(12, 3),
        "tag": _b64url(16, 4),
        "key": _b64url(32, 5),
    }
    envelope = {
        "kind": "forkmesh.mirror",
        "v": 1,
        "alg": "x25519+mlkem768/aes256gcm",
        "nonce": _b64url(12, 6),
        "tag": _b64url(16, 7),
        "body": _b64url(48, 8),
        "recipients": [recipient],
    }
    root = {
        "schemaVersion": 2,
        "kind": "forkmesh.private-replica",
        "opaqueId": "9" * 64,
        "ownerKeyId": recipient["kid"],
        "keyEpoch": 3,
        "createdAt": 1,
        "updatedAt": 2,
        "ciphertextSha256": hashlib.sha256(
            json.dumps(envelope, separators=(",", ":")).encode()).hexdigest(),
        "envelope": envelope,
    }
    return json.dumps(root, separators=(",", ":")).encode()


def test_operator_view_accepts_only_declared_metadata_and_ciphertext():
    connection = _database()
    try:
        _seed_private(connection)
        report = audit_database(connection)
    finally:
        connection.close()
    assert report["ok"] is True
    assert report["privateRepositories"] == 1
    assert report["findings"] == []
    assert set(report["inspectedRows"]) == set(DECLARED_COLUMNS)


def test_plaintext_indexes_and_authority_labels_fail_without_echoing_values():
    connection = _database()
    secret = "workstation-for-project-cobalt"
    try:
        repo_bi = _seed_private(connection)
        connection.execute(
            "UPDATE repo_source_authorities SET machine_name=? WHERE repo_bi=?",
            (secret, repo_bi),
        )
        connection.execute(
            "INSERT INTO repo_state_history(key_bi,state_hash,ts) VALUES(?,?,1)",
            (repo_bi, "1" * 64),
        )
        connection.execute(
            "UPDATE repo_device_mirrors SET node_id=? WHERE repo_bi=?",
            (secret, repo_bi),
        )
        report = audit_database(connection)
    finally:
        connection.close()
    assert report["ok"] is False
    codes = {finding["code"] for finding in report["findings"]}
    assert {
        "private_authority_plaintext",
        "private_device_plaintext",
        "private_derived_index",
    }.issubset(codes)
    assert secret not in json.dumps(report)


def test_malformed_ciphertext_and_new_columns_fail_closed():
    connection = _database()
    try:
        repo_bi = _seed_private(connection)
        connection.execute(
            "UPDATE repositories SET data='not ciphertext' WHERE key_bi=?",
            (repo_bi,),
        )
        connection.execute("ALTER TABLE notifications ADD COLUMN preview TEXT")
        report = audit_database(connection)
    finally:
        connection.close()
    codes = {finding["code"] for finding in report["findings"]}
    assert "malformed_ciphertext" in codes
    assert "undeclared_metadata_column" in codes


def test_private_replica_is_keylessly_verified_and_tampering_is_rejected():
    replica = _replica()
    assert audit_replica_bytes(replica) == []
    changed = json.loads(replica)
    changed["envelope"]["body"] = _b64url(48, 9)
    assert audit_replica_bytes(
        json.dumps(changed, separators=(",", ":")).encode()
    ) == ["invalid_replica_metadata"]


def test_cli_emits_machine_readable_evidence_and_nonzero_on_a_leak(tmp_path):
    database = tmp_path / "backup.sqlite3"
    connection = _database(str(database))
    repo_bi = _seed_private(connection)
    connection.close()
    tool = ROOT / "tools/audit_private_surfaces.py"
    clean = subprocess.run(
        [sys.executable, str(tool), str(database)], cwd=ROOT,
        capture_output=True, text=True, check=False)
    assert clean.returncode == 0, clean.stderr
    assert json.loads(clean.stdout)["ok"] is True

    connection = sqlite3.connect(database)
    connection.execute(
        "INSERT INTO repo_state_history(key_bi,state_hash,ts) VALUES(?,?,1)",
        (repo_bi, "2" * 64),
    )
    connection.commit()
    connection.close()
    failed = subprocess.run(
        [sys.executable, str(tool), str(database)], cwd=ROOT,
        capture_output=True, text=True, check=False)
    assert failed.returncode == 1
    assert json.loads(failed.stdout)["ok"] is False


def test_migration_scrubs_existing_private_authority_and_derived_indexes():
    connection = sqlite3.connect(":memory:")
    connection.executescript(
        """
        CREATE TABLE schema_meta(k TEXT PRIMARY KEY,v TEXT NOT NULL);
        CREATE TABLE repositories(key_bi TEXT PRIMARY KEY,is_private INTEGER NOT NULL);
        CREATE TABLE repo_source_authorities(
          repo_bi TEXT PRIMARY KEY,node_id TEXT NOT NULL,machine_name TEXT NOT NULL,
          created_at INTEGER NOT NULL,updated_at INTEGER NOT NULL);
        CREATE TABLE repo_device_mirrors(repo_bi TEXT,node_id TEXT);
        CREATE TABLE repo_state_history(key_bi TEXT,state_hash TEXT,ts INTEGER);
        INSERT INTO repositories VALUES('private',1),('public',0);
        INSERT INTO repo_source_authorities VALUES('private','secret-device','secret-host',1,1);
        INSERT INTO repo_source_authorities VALUES('public','public-device','public-host',1,1);
        INSERT INTO repo_device_mirrors VALUES('private','secret-secondary');
        INSERT INTO repo_device_mirrors VALUES('public','public-secondary');
        INSERT INTO repo_state_history VALUES('private','private-state',1);
        INSERT INTO repo_state_history VALUES('public','public-state',1);
        """
    )
    migration = (ROOT / "app/migrations/0181_private_authority_envelope.sql").read_text(
        encoding="utf-8")
    connection.executescript(migration)
    assert connection.execute(
        "SELECT node_id,machine_name,is_private,data FROM repo_source_authorities "
        "WHERE repo_bi='private'").fetchone() == ("", "", 1, "")
    assert connection.execute(
        "SELECT node_id,machine_name,is_private FROM repo_source_authorities "
        "WHERE repo_bi='public'").fetchone() == (
            "public-device", "public-host", 0)
    assert connection.execute(
        "SELECT repo_bi FROM repo_device_mirrors").fetchall() == [("public",)]
    assert connection.execute(
        "SELECT key_bi FROM repo_state_history").fetchall() == [("public",)]
    down = (ROOT / "app/migrations/down/0181_private_authority_envelope.sql").read_text(
        encoding="utf-8")
    connection.executescript(down)
    columns = [row[1] for row in connection.execute(
        "PRAGMA table_info(repo_source_authorities)")]
    assert "data" not in columns and "is_private" not in columns
    assert connection.execute(
        "SELECT COUNT(*) FROM schema_meta WHERE k LIKE 'schema_fingerprint:%'"
    ).fetchone() == (0,)
    connection.close()
