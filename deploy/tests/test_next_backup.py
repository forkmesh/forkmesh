"""tools/next_backup.py: verification, retention, safe deletion, the API."""

import datetime as dt
import gzip
import io
import json
import os
import shutil
import sqlite3
import subprocess
import time
from pathlib import Path

import pytest

import next_backup
from fakes import FakeSsh
from next_common import Runner, ToolError

REPO = Path(__file__).resolve().parents[2]
FLEET = REPO / "deploy" / "fleet.example.toml"
UTC = dt.timezone.utc
# Resolved before the sandbox fixture moves HOME.
_REAL_AGE_DIR = Path(os.path.expanduser("~/.local/bin"))
AGE_DIR = _REAL_AGE_DIR if (_REAL_AGE_DIR / "age").is_file() else (
    Path(shutil.which("age")).parent if shutil.which("age") else None)
needs_age = pytest.mark.skipif(
    AGE_DIR is None or not (AGE_DIR / "age-keygen").is_file(), reason="age not installed")


@pytest.fixture
def age_keys(sandbox, monkeypatch):
    """A fresh key pair in the sandboxed config dir, via `keygen`."""
    monkeypatch.setenv("FMNEXT_AGE_DIR", str(AGE_DIR))
    next_backup.keygen(Runner(execute=True))
    return sandbox / "backup-age.key", sandbox / "backup-age.pub"


def sqlite_bytes(tmp_path, tables=("d1_migrations", "accounts")):
    path = tmp_path / ("src-%d.sqlite3" % time.monotonic_ns())
    connection = sqlite3.connect(path)
    for table in tables:
        connection.execute("CREATE TABLE %s (id INTEGER PRIMARY KEY, name TEXT)" % table)
        connection.execute("INSERT INTO %s (name) VALUES ('a'), ('b')" % table)
    connection.commit()
    connection.close()
    return path.read_bytes()


def test_verify_counts_tables(tmp_path):
    backup = tmp_path / "rqlite-20260923T033000Z.sqlite3.gz"
    backup.write_bytes(gzip.compress(sqlite_bytes(tmp_path)))
    assert next_backup.verify(backup) == {"d1_migrations": 2, "accounts": 2}


@pytest.mark.parametrize("payload", [b"not gzip", gzip.compress(b"not sqlite" * 100)])
def test_verify_rejects_garbage(tmp_path, payload):
    backup = tmp_path / "x.gz"
    backup.write_bytes(payload)
    with pytest.raises(ToolError):
        next_backup.verify(backup)


def test_verify_requires_the_migrations_ledger(tmp_path):
    backup = tmp_path / "x.gz"
    backup.write_bytes(gzip.compress(sqlite_bytes(tmp_path, tables=("accounts",))))
    with pytest.raises(ToolError, match="d1_migrations"):
        next_backup.verify(backup)


def test_verify_rejects_a_corrupt_database(tmp_path):
    raw = bytearray(sqlite_bytes(tmp_path))
    raw[4096:4096 + 8] = b"\xff" * 8  # damage the first table's b-tree page header
    backup = tmp_path / "x.gz"
    backup.write_bytes(gzip.compress(bytes(raw)))
    with pytest.raises(ToolError):
        next_backup.verify(backup)


def names_for(days):
    start = dt.datetime(2026, 9, 23, 3, 30, tzinfo=UTC)
    return [next_backup.backup_name(start - dt.timedelta(days=d)) for d in days]


def test_retention_keeps_7_daily_and_4_weekly():
    names = names_for(range(0, 60))
    keep, delete = next_backup.retention_plan(names)
    # 2026-09-23 is a Wednesday: the 7 dailies (09-17..09-23) already hold
    # the newest of ISO weeks 39 and 38, so the weeklies add weeks 37 and 36.
    assert len(keep) == 7 + 2
    assert "rqlite-20260906T033000Z.sqlite3.gz" in keep
    assert set(names[:7]) <= set(keep)
    assert len(keep) + len(delete) == 60
    two_a_day = names_for(range(0, 3)) + [
        next_backup.backup_name(dt.datetime(2026, 9, 23, 1, 0, tzinfo=UTC))]
    keep, delete = next_backup.retention_plan(two_a_day)
    assert delete == ["rqlite-20260923T010000Z.sqlite3.gz"]


def test_prune_deletes_only_its_own_files(tmp_path):
    directory = tmp_path / "backups"
    directory.mkdir()
    for name in names_for(range(0, 40)):
        (directory / name).write_bytes(b"x")
    decoys = ["rqlite-latest.sqlite3.gz", "notes.txt", "rqlite-20200101T000000Z.sqlite3",
              "other-20200101T000000Z.sqlite3.gz", "rqlite-20200101T000000Z.sqlite3.gz.part"]
    for name in decoys:
        (directory / name).write_bytes(b"keep me")
    outside = tmp_path / "rqlite-20200101T000000Z.sqlite3.gz"
    outside.write_bytes(b"outside")
    link = directory / "rqlite-20200102T000000Z.sqlite3.gz"
    link.symlink_to(outside)
    (directory / "rqlite-20200103T000000Z.sqlite3.gz").mkdir()

    runner = Runner(execute=False)
    planned = next_backup.prune(directory, runner)
    assert planned and all(next_backup.NAME_RE.match(n) for n in planned)
    assert len(list(directory.iterdir())) == 40 + len(decoys) + 2  # dry run
    next_backup.prune(directory, Runner(execute=True))
    left = sorted(p.name for p in directory.iterdir())
    for name in decoys:
        assert name in left
    assert link.is_symlink() and outside.read_bytes() == b"outside"
    assert (directory / "rqlite-20200103T000000Z.sqlite3.gz").is_dir()
    kept = [n for n in left if next_backup.NAME_RE.match(n)
            and (directory / n).is_file() and not (directory / n).is_symlink()]
    assert len(kept) == 9


class Response(io.BytesIO):
    status = 200

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def status_answer():
    return (lambda host, script: "/status" in script,
            lambda host, script, stdin: json.dumps({"store": {"raft": {
                "state": "Leader" if host == "203.0.113.12" else "Follower"}}}).encode())


@needs_age
def test_run_backup_end_to_end_with_fakes(tmp_path, sandbox, monkeypatch, age_keys):
    import next_deploy
    import render
    (sandbox / "secrets.env").write_text("RQLITE_PASSWORD=pw-SECRET\n")
    monkeypatch.setattr(next_deploy, "_port_open", lambda *a: True)
    monkeypatch.setattr(next_deploy, "loopback_accepts", lambda host: True)
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    payload = gzip.compress(sqlite_bytes(tmp_path))
    seen = {}

    def opener(request, timeout=None):
        seen["url"] = request.full_url
        seen["auth"] = request.headers["Authorization"]
        return Response(payload)
    ssh = FakeSsh([status_answer()])
    directory = tmp_path / "backups"
    now = dt.datetime(2026, 9, 23, 3, 30, tzinfo=UTC)
    final = next_backup.run_backup(fleet, ssh, Runner(execute=True), directory, now, opener)
    assert final.name == "rqlite-20260923T033000Z.sqlite3.gz.age"
    assert next_backup.is_encrypted(final)
    assert payload not in final.read_bytes()
    assert (final.stat().st_mode & 0o777) == 0o600
    assert ssh.tunnels[0][0] == "203.0.113.12"  # the leader
    assert seen["url"].endswith("/db/backup?compress")
    # No plaintext anywhere under the backups directory, no leftovers.
    files = [p for p in directory.rglob("*") if p.is_file()]
    assert files == [final]
    assert (directory / ".work").stat().st_mode & 0o777 == 0o700
    # verify and restore decrypt with the identity.
    key, _ = age_keys
    assert next_backup.verify(final, identity=key) == {"d1_migrations": 2, "accounts": 2}
    out = tmp_path / "restored.sqlite3"
    next_backup.restore(final, out, key)
    assert out.read_bytes() == gzip.decompress(payload)
    assert out.stat().st_mode & 0o777 == 0o600
    # Without the identity (moved offline) verify refuses clearly.
    with pytest.raises(ToolError, match="offline identity"):
        next_backup.verify(final, identity=tmp_path / "absent.key")


def test_run_backup_refuses_without_a_recipient(tmp_path, sandbox, monkeypatch):
    import next_deploy
    import render
    (sandbox / "secrets.env").write_text("RQLITE_PASSWORD=pw\n")
    monkeypatch.setattr(next_deploy, "_port_open", lambda *a: True)
    monkeypatch.setattr(next_deploy, "loopback_accepts", lambda host: True)
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    ssh = FakeSsh([status_answer()])
    with pytest.raises(ToolError, match="keygen"):
        next_backup.run_backup(fleet, ssh, Runner(execute=True), tmp_path / "b", None,
                               lambda *a, **k: Response(b"never"))
    assert ssh.tunnels == []  # refused before downloading anything


@needs_age
def test_run_backup_discards_an_unverifiable_download(tmp_path, sandbox, monkeypatch,
                                                       age_keys):
    import next_deploy
    import render
    (sandbox / "secrets.env").write_text("RQLITE_PASSWORD=pw\n")
    monkeypatch.setattr(next_deploy, "_port_open", lambda *a: True)
    monkeypatch.setattr(next_deploy, "loopback_accepts", lambda host: True)
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    directory = tmp_path / "backups"
    with pytest.raises(ToolError):
        next_backup.run_backup(fleet, FakeSsh([status_answer()]), Runner(execute=True),
                               directory, None, lambda *a, **k: Response(b"garbage"))
    assert [p for p in directory.rglob("*") if p.is_file()] == []


@needs_age
def test_keygen_modes_and_refuses_to_overwrite(sandbox, age_keys):
    key, pub = age_keys
    assert key.stat().st_mode & 0o777 == 0o400
    assert next_backup.read_recipient(pub).startswith("age1")
    with pytest.raises(next_backup.GuardError):
        next_backup.keygen(Runner(execute=True))


@needs_age
def test_encrypt_existing_replaces_only_its_plaintext_backups(tmp_path, sandbox, age_keys):
    key, _ = age_keys
    directory = tmp_path / "backups"
    directory.mkdir()
    plain_names = names_for(range(0, 2))
    payload = gzip.compress(sqlite_bytes(tmp_path))
    for name in plain_names:
        (directory / name).write_bytes(payload)
    decoys = {"notes.txt": b"keep", "rqlite-latest.sqlite3.gz": payload,
              "rqlite-20200101T000000Z.sqlite3": b"keep"}
    for name, data in decoys.items():
        (directory / name).write_bytes(data)
    outside = tmp_path / "rqlite-20200101T000000Z.sqlite3.gz"
    outside.write_bytes(payload)
    (directory / "rqlite-20200102T000000Z.sqlite3.gz").symlink_to(outside)

    next_backup.encrypt_existing(directory, Runner(execute=False), key)
    assert all((directory / n).exists() for n in plain_names)  # dry run

    done = next_backup.encrypt_existing(directory, Runner(execute=True), key)
    assert sorted(done) == sorted(plain_names)
    for name in plain_names:
        assert not (directory / name).exists()
        encrypted = directory / (name + ".age")
        assert next_backup.is_encrypted(encrypted)
        assert next_backup.verify(encrypted, identity=key)["d1_migrations"] == 2
    for name, data in decoys.items():
        assert (directory / name).read_bytes() == data
    assert outside.read_bytes() == payload
    assert (directory / "rqlite-20200102T000000Z.sqlite3.gz").is_symlink()


@needs_age
def test_encrypt_existing_keeps_plaintext_when_the_round_trip_fails(
        tmp_path, sandbox, age_keys, monkeypatch):
    key, _ = age_keys
    directory = tmp_path / "backups"
    directory.mkdir()
    name = names_for([0])[0]
    (directory / name).write_bytes(gzip.compress(sqlite_bytes(tmp_path)))
    monkeypatch.setattr(next_backup, "age_decrypt", lambda *a: (_ for _ in ()).throw(
        ToolError("boom")))
    with pytest.raises(ToolError):
        next_backup.encrypt_existing(directory, Runner(execute=True), key)
    assert (directory / name).exists()
    assert not (directory / (name + ".age")).exists()


def test_retention_counts_encrypted_names():
    names = [n + ".age" for n in names_for(range(0, 20))]
    keep, delete = next_backup.retention_plan(names)
    assert keep and delete and all(n.endswith(".age") for n in keep + delete)


def test_run_backup_dry_run(tmp_path):
    import render
    fleet = render.load_fleet(FLEET, state_path=tmp_path / "none.json")
    ssh = FakeSsh([status_answer()])
    next_backup.run_backup(fleet, ssh, Runner(execute=False), tmp_path / "b")
    assert ssh.runs == [] and ssh.tunnels == []
    assert not (tmp_path / "b").exists()


def rqlited():
    import forkmesh_paths
    return forkmesh_paths.resolve().fleet_binary("rqlited", "RQLITED")


RQLITED = rqlited()


@pytest.mark.skipif(RQLITED is None, reason="rqlited not available")
def test_download_against_a_real_rqlited(tmp_path):
    import socket
    import urllib.request
    from base64 import b64encode
    ports = []
    for _ in range(2):
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            ports.append(sock.getsockname()[1])
    auth = tmp_path / "auth.json"
    auth.write_text(json.dumps([{"username": "fmnext", "password": "pw", "perms": ["all"]}]))
    process = subprocess.Popen(
        [RQLITED, "-node-id", "b1", "-http-addr", "127.0.0.1:%d" % ports[0],
         "-raft-addr", "127.0.0.1:%d" % ports[1], "-auth", str(auth), str(tmp_path / "d")],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=dict(os.environ, TZ="UTC"))
    try:
        base = "http://127.0.0.1:%d" % ports[0]
        headers = {"Authorization": "Basic " + b64encode(b"fmnext:pw").decode(),
                   "Content-Type": "application/json"}
        body = json.dumps(["CREATE TABLE d1_migrations(id INTEGER PRIMARY KEY, name TEXT)",
                           "INSERT INTO d1_migrations(name) VALUES('0001_init.sql')"]).encode()
        deadline = time.time() + 20
        while True:
            try:
                urllib.request.urlopen(urllib.request.Request(
                    base + "/db/execute", data=body, headers=headers), timeout=2).close()
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.3)
        target = tmp_path / "rqlite-20260923T033000Z.sqlite3.gz"
        next_backup.download(base, "fmnext", "pw", target)
        assert next_backup.verify(target) == {"d1_migrations": 1}
        with pytest.raises(Exception):
            next_backup.download(base, "fmnext", "wrong", tmp_path / "other.gz")
    finally:
        process.terminate()
        process.wait(timeout=10)
