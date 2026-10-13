"""tools/next_rekey.py: re-key a database copy from DATA_KEY A to key B.

* The registry covers the schema: every *_bi column, token column,
  ciphertext column and dropped table the tool names exists, and every *_bi
  column of the schema (app/migrations + app/src/schema.py) is named.
* The tool's crypto is byte-compatible with the app's own functions
  (encrypt_row/decrypt_row, blind_index, the chat passphrases and envelope).
* End to end: a host under key A creates real data through the app (signup,
  login, org, note), more rows are added with the app's own functions
  (notifications, chat, a device, a vault row, embedded and nested values),
  the tool re-keys A -> B, and a host under key B serves the copy: the
  password logs in (by name and by email), the org and the note read back,
  the old session is refused, and nothing in the file opens under key A.
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import time

import httpx
import pytest

from forkmesh_host import d1_sqlite, paths

TOOL = paths.REPO_DIR / "tools" / "next_rekey.py"
_spec = importlib.util.spec_from_file_location("next_rekey", TOOL)
rk = importlib.util.module_from_spec(_spec)
sys.modules["next_rekey"] = rk
_spec.loader.exec_module(rk)

KEY_A = "rekey-test-old-" + os.urandom(24).hex()
KEY_B = "rekey-test-new-" + os.urandom(24).hex()
PASSWORD = "correct-horse-battery-" + os.urandom(4).hex()  # forkmesh-secret-scan:ignore-line
NAME = "rekeyuser"
EMAIL = "rekey.user@example.com"
ORG = "rekeyorg"
NOTE_TITLE = "Quarterly plan"
NOTE_BODY = "Secret roadmap text"
IP = "203.0.113.77"


class Env:
    def __init__(self, key):
        self.DATA_KEY = key


def entry_module():
    paths.ensure_import_paths()
    import entry  # noqa: PLC0415
    return entry


def run(coro):
    return asyncio.run(coro)


# --------------------------------------------------------------------------
# Registry coverage


@pytest.fixture(scope="module")
def schema_db(staged_src, tmp_path_factory):
    del staged_src
    path = tmp_path_factory.mktemp("rekey-schema") / "schema.sqlite3"
    conn = sqlite3.connect(path)
    d1_sqlite.apply_migrations(conn, paths.APP_DIR / "migrations")
    paths.ensure_import_paths()
    import schema  # noqa: PLC0415
    d1_sqlite.apply_app_schema(conn, schema)
    conn.commit()
    yield conn
    conn.close()


def _schema(conn):
    out = {}
    for (table,) in conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%'"):
        out[table] = [r[1] for r in conn.execute('PRAGMA table_info("%s")' % table)]
    return out


def test_every_blind_index_column_is_handled(schema_db):
    schema = _schema(schema_db)
    in_schema = {(t, c) for t, cols in schema.items() for c in cols if c.endswith("_bi")}
    registered = {(t, c) for t, cols in rk.BI_COLUMNS.items() for c in cols}
    assert not in_schema - registered, (
        "add these *_bi columns to BI_COLUMNS in tools/next_rekey.py and "
        "docs/REKEY.md: %s" % sorted(in_schema - registered))
    assert not registered - in_schema, (
        "BI_COLUMNS names columns the schema no longer has: %s"
        % sorted(registered - in_schema))
    assert rk.STRICT_BI <= registered


def test_every_ciphertext_token_and_dropped_table_exists(schema_db):
    schema = _schema(schema_db)
    for registry in (rk.CIPHER_COLUMNS, rk.TOKEN_COLUMNS):
        for table, cols in registry.items():
            assert table in schema, table
            for col in cols:
                assert col in schema[table], (table, col)
    for table in rk.DROP_TABLES:
        assert table in schema, table
    table, col = rk.CHAT_TABLE
    assert col in schema[table] and "room_key" in schema[table]
    # Every table holding a blind index or ciphertext has a decision: its
    # columns are rewritten (BI/TOKEN/CIPHER registries) or it is emptied.
    handled = set(rk.BI_COLUMNS) | set(rk.CIPHER_COLUMNS) | set(rk.DROP_TABLES) \
        | set(rk.TOKEN_COLUMNS) | {table}
    needing = {t for t, cols in schema.items()
               if any(c.endswith("_bi") for c in cols)}
    assert needing <= handled, sorted(needing - handled)


def test_inventory_doc_names_every_registered_table():
    doc = (paths.REPO_DIR / "docs" / "REKEY.md").read_text("utf-8")
    names = set(rk.BI_COLUMNS) | set(rk.CIPHER_COLUMNS) | set(rk.DROP_TABLES) \
        | set(rk.TOKEN_COLUMNS) | {rk.CHAT_TABLE[0]}
    missing = sorted(t for t in names if "`%s`" % t not in doc)
    assert not missing, "docs/REKEY.md does not list: %s" % missing


# --------------------------------------------------------------------------
# Byte compatibility with the app's own crypto


def test_crypto_matches_the_app(staged_src):
    del staged_src
    entry = entry_module()
    env = Env(KEY_A)
    keys = rk.Keyset(KEY_A, "a")

    async def check():
        for value in ("alice", "  Alice@Example.COM ", "org:acme", "", "owner/repo"):
            assert await entry.blind_index(env, value) == keys.bi(value)
        for obj in ({"name": "alice", "n": 1, "u": "café"}, [1, "x"], "s"):
            stored = await entry.encrypt_row(env, obj)
            assert json.loads(keys.open(stored)) == obj
            ours = keys.seal(json.dumps(obj).encode())
            assert await entry.decrypt_row(env, ours) == obj
            assert await entry.decrypt_row(Env(KEY_B), ours) is None
        for owner, repo, room in (("mainnode", "forkmesh", "general"),
                                  ("mainnode", "forkmesh", "world-general"),
                                  ("Alice", "Proj", "general")):
            assert await entry._room_chat_passphrase(env, owner, repo, room) \
                == keys.room_passphrase(owner, repo, room)
        assert await entry._chat_channel_passphrase(env, "c1d2", 3) \
            == keys.channel_passphrase("c1d2", 3)
        assert await entry._chat_direct_passphrase(env, "d9", 2) \
            == keys.direct_passphrase("d9", 2)
        room_key = "chat-channel:c1d2:v3"
        envelope = await entry._chat_encrypted_envelope(
            room_key, keys.channel_passphrase("c1d2", 3), {"text": "hi"})
        plain, index = rk.chat_open(keys, room_key, envelope)
        assert json.loads(plain) == {"text": "hi"} and index == 0
        world = await entry._chat_encrypted_envelope(
            "world-general", keys.room_passphrase("mainnode", "forkmesh", "world-general"),
            {"text": "yo"})
        plain, _ = rk.chat_open(keys, "repo:mainnode/forkmesh:room:world-general", world)
        assert json.loads(plain) == {"text": "yo"}
    run(check())


def test_keys_come_only_from_private_files(tmp_path):
    loose = tmp_path / "loose"
    loose.write_text(KEY_A + "\n")
    os.chmod(loose, 0o644)
    with pytest.raises(rk.RekeyError, match="group/other"):
        rk.read_key(loose, "old")
    os.chmod(loose, 0o400)
    assert rk.read_key(loose, "old").bi("x") == rk.Keyset(KEY_A, "a").bi("x")
    assert KEY_A not in repr(rk.read_key(loose, "old"))
    placeholder = tmp_path / "ph"
    placeholder.write_text("forkmesh-dev-data-key\n")
    os.chmod(placeholder, 0o400)
    with pytest.raises(rk.RekeyError):
        rk.read_key(placeholder, "old")


# --------------------------------------------------------------------------
# End to end


def _free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


class Server:
    def __init__(self, tmp, sandbox_env, data_dir, key):
        assets = tmp / "assets"
        if not assets.exists():
            shutil.copytree(paths.APP_DIR / "public", assets)
            (assets / "index.html").write_text("<!doctype html><title>home</title>")
            (assets / "404.html").write_text("<!doctype html><title>404</title>")
        self.port = _free_port()
        self.origin = "http://127.0.0.1:%d" % self.port
        env = sandbox_env(tmp)
        env.update({
            "FORKMESH_HOST_DATA_DIR": str(data_dir),
            "FORKMESH_HOST_ASSETS_DIR": str(assets),
            "FORKMESH_HOST_OFFLINE": "1",
            "FORKMESH_VAR_DATA_KEY": key,
            "FORKMESH_VAR_ADMIN_PASS": "rekey-admin-" + os.urandom(8).hex(),
            "FORKMESH_VAR_APP_ORIGIN": self.origin,
            "FORKMESH_VAR_API_ORIGIN": self.origin,
            "FORKMESH_VAR_WORLD_ORIGIN": self.origin,
            "FORKMESH_VAR_PUBLIC_BASE_URL": self.origin,
            "FORKMESH_VAR_CORS_ALLOWED_ORIGINS": self.origin,
            # Cloudflare's always-pass Turnstile test pair (turnstile.py).
            "FORKMESH_VAR_TURNSTILE_SITE_KEY": "1x00000000000000000000AA",
            "FORKMESH_VAR_TURNSTILE_SECRET_KEY":
                "1x0000000000000000000000000000000AA",
            "FORKMESH_VAR_TURNSTILE_TEST_MODE": "1",
        })
        self.env = env
        self.log = open(data_dir.parent / ("server-%d.log" % self.port), "w+")
        self.process = subprocess.Popen(
            [sys.executable, "-m", "forkmesh_host", "serve", "--port",
             str(self.port), "--no-cron", "--migrate", "--no-build",
             "--log-level", "warning"],
            cwd=str(paths.HOST_DIR), env=env, stdout=self.log,
            stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while time.time() < deadline:
            if self.process.poll() is not None:
                self.log.seek(0)
                pytest.fail("server exited: " + self.log.read()[-4000:])
            try:
                if httpx.get(self.origin + "/health", timeout=1).status_code == 200:
                    break
            except httpx.HTTPError:
                time.sleep(0.2)
        else:
            self.stop()
            pytest.fail("server did not start")
        self.client = httpx.Client(base_url=self.origin, timeout=30,
                                   headers={"origin": self.origin})

    def ops(self, action, account):
        completed = subprocess.run(
            [sys.executable, "-m", "forkmesh_host", "ops", action, account],
            cwd=str(paths.HOST_DIR), env=self.env, capture_output=True,
            text=True, timeout=120)
        assert completed.returncode == 0, completed.stdout[-2000:] + completed.stderr[-2000:]

    def stop(self):
        try:
            self.client.close()
        except AttributeError:
            pass
        self.process.terminate()
        try:
            self.process.wait(15)
        except subprocess.TimeoutExpired:
            self.process.kill()
        self.log.close()


def _checkpoint(db_path):
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA wal_checkpoint(TRUNCATE)")
    conn.execute("PRAGMA journal_mode=DELETE")
    conn.close()


def _add_rows_with_app_code(db_path, account_pubkey, apns_token):
    """Rows the HTTP surface cannot create offline, built with entry's code."""
    entry = entry_module()
    env = Env(KEY_A)
    from forkmesh_host.vault_crypto import VaultKey  # noqa: PLC0415

    async def build():
        bi = lambda v: entry.blind_index(env, v)  # noqa: E731
        account_bi = await bi(NAME)
        rows = {}
        rows["notification"] = (
            await bi("notification:" + NAME + ":rekey-test"), account_bi,
            await entry.encrypt_row(env, {"kind": "mention", "title": "Hello",
                                          "actor": "someone", "repo": NAME + "/proj"}))
        channel = "chat-channel:ab12cd:v1"
        rows["chat"] = [
            (channel, "m1", json.dumps(await entry._chat_encrypted_envelope(
                channel, await entry._chat_channel_passphrase(env, "ab12cd", 1),
                {"text": "channel secret"}), separators=(",", ":"))),
            (entry.PUBLIC_WORLD_GENERAL_ROOM_KEY, "m2",
             json.dumps(await entry._chat_encrypted_envelope(
                 entry.PUBLIC_WORLD_GENERAL_ROOM,
                 await entry._room_chat_passphrase(
                     env, "mainnode", "forkmesh", entry.PUBLIC_WORLD_GENERAL_ROOM),
                 {"text": "world hello"}), separators=(",", ":"))),
        ]
        rows["device_bi"] = await bi(account_bi + ":" + account_pubkey)
        rows["account_bi"] = account_bi
        rows["apns"] = (await bi("apns:" + apns_token), await entry.encrypt_row(
            env, {"token": apns_token, "environment": "production"}))
        rows["nested"] = json.dumps({"sealed": await entry.encrypt_row(
            env, {"inner": NAME})})
        rows["feedback_ip"] = await bi(IP)
        return rows

    rows = run(build())
    vault = VaultKey(KEY_A)
    conn = sqlite3.connect(db_path)
    with conn:
        conn.execute("INSERT INTO notifications (dedupe_bi, recipient_bi, ts, read_at, data) "
                     "VALUES (?,?,?,?,?)", (*rows["notification"][:2], 1, 0,
                                            rows["notification"][2]))
        for room_key, msg_id, body in rows["chat"]:
            conn.execute("INSERT INTO chat_history (room_key, msg_id, ts, body) "
                         "VALUES (?,?,?,?)", (room_key, msg_id, int(time.time() * 1000), body))
        conn.execute("INSERT INTO account_devices (device_bi, account_bi, pubkey, kind, "
                     "label, capabilities, enabled, created_at, last_seen, revoked_at) "
                     "VALUES (?,?,?,?,?,?,?,?,?,?)",
                     (rows["device_bi"], rows["account_bi"], account_pubkey,
                      "desktop_node", "", "[]", 1, 1, 1, 0))
        conn.execute("INSERT INTO apns_device_tokens (token_bi, account_bi, device_bi, "
                     "environment, data, created_at, last_seen) VALUES (?,?,?,?,?,?,?)",
                     (rows["apns"][0], rows["account_bi"], rows["device_bi"],
                      "production", rows["apns"][1], 1, 1))
        conn.execute("INSERT INTO ap_settings (k, v) VALUES (?, ?)",
                     ("rekey-nested", rows["nested"]))
        conn.execute("INSERT INTO feedback (ts, source, vote, path, message, ip_hash, "
                     "user_agent) VALUES (1,'kiosk','up','/','ok',?,'browser')",
                     (rows["feedback_ip"],))
        conn.execute("INSERT INTO error_log (ts, status, method, path, message, ray, actor) "
                     "VALUES (1, 500, 'GET', '/x', ?, '', '')",
                     ("lookup failed for " + rows["account_bi"],))
        row = vault.check_row()
        conn.execute("INSERT INTO host_secrets (name, nonce, ciphertext, key_id, updated_at) "
                     "VALUES (?,?,?,?,1)", (row["name"], row["nonce"],
                                            row["ciphertext"], row["key_id"]))
    conn.close()
    return rows


def _write_key(path, key):
    path.write_text(key + "\n")
    os.chmod(path, 0o400)
    return path


def _old_key_opens(db_path, keyset):
    """Values anywhere (whole cells or JSON leaves) that open under keyset."""
    conn = sqlite3.connect(db_path)
    hits = 0

    def leaf(value):
        nonlocal hits
        if isinstance(value, str):
            if keyset.open(value) is not None:
                hits += 1
                return
            if value.lstrip()[:1] in "{[":
                try:
                    leaf(json.loads(value))
                except ValueError:
                    pass
        elif isinstance(value, dict):
            for item in value.values():
                leaf(item)
        elif isinstance(value, list):
            for item in value:
                leaf(item)

    for (table,) in conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table'").fetchall():
        for values in conn.execute('SELECT * FROM "%s"' % table):
            for value in values:
                leaf(value)
    conn.close()
    return hits


@pytest.fixture(scope="module")
def rekeyed(staged_src, tmp_path_factory, sandbox_env):
    del staged_src
    tmp = tmp_path_factory.mktemp("rekey-e2e")
    data_a = tmp / "a"
    data_a.mkdir()
    server = Server(tmp, sandbox_env, data_a, KEY_A)
    try:
        client = server.client
        page = client.get("/signup")
        field = re.search(r"contact_[0-9a-f]{18}", page.text).group(0)
        signup = client.post("/api/accounts/signup", json={
            "nodeName": NAME, field: EMAIL, "password": PASSWORD,
            "turnstileToken": "XXXX.DUMMY.TOKEN.XXXX"})
        assert signup.status_code == 201, signup.text[:300]
        server.ops("verify-email", NAME)
        login = client.post("/api/accounts/login", json={
            "nodeName": NAME, "password": PASSWORD})
        assert login.status_code == 200, login.text[:300]
        old_token = login.json()["sessionToken"]
        auth = {"authorization": "Bearer " + old_token}
        org = client.post("/api/orgs", headers=auth, json={
            "name": ORG, "displayName": "Rekey Org"})
        assert org.status_code == 201, org.text[:300]
        note = client.post("/api/notes", headers=auth, json={
            "title": NOTE_TITLE, "markdown": NOTE_BODY})
        assert note.status_code == 201, note.text[:300]
        note_id = note.json()["note"]["id"]
        assert client.get("/api/notes/" + note_id, headers=auth).status_code == 200
    finally:
        server.stop()
    db_a = data_a / "d1.sqlite"
    _checkpoint(db_a)
    pubkey = os.urandom(32).hex()
    apns_token = os.urandom(32).hex()
    rows = _add_rows_with_app_code(db_a, pubkey, apns_token)
    assert _old_key_opens(db_a, rk.Keyset(KEY_A, "a")) > 5

    keys_dir = tmp / "keys"
    keys_dir.mkdir(mode=0o700)
    old_file = _write_key(keys_dir / "DATA_KEY", KEY_A)
    new_file = _write_key(keys_dir / "DATA_KEY.next-original", KEY_B)
    work = tmp / "work"
    out = tmp / "rekeyed.sqlite3"
    summary_path = tmp / "summary.json"
    env = sandbox_env(tmp)
    env["PYTHONPATH"] = ""
    completed = subprocess.run(
        [sys.executable, str(TOOL), "run", "--in", str(db_a), "--out", str(out),
         "--old-key", str(old_file), "--new-key", str(new_file),
         "--work-dir", str(work), "--summary-json", str(summary_path)],
        env=env, capture_output=True, text=True, timeout=600)
    assert completed.returncode == 0, completed.stdout[-3000:] + completed.stderr[-3000:]
    return {
        "tmp": tmp, "db_a": db_a, "out": out, "work": work, "stdout": completed.stdout,
        "stderr": completed.stderr, "summary": json.loads(summary_path.read_text()),
        "summary_text": summary_path.read_text(), "old_token": old_token,
        "note_id": note_id, "pubkey": pubkey, "apns_token": apns_token, "rows": rows,
        "sandbox_env": sandbox_env, "keys": (old_file, new_file),
    }


def test_output_is_private_and_work_dir_is_gone(rekeyed):
    assert (rekeyed["out"].stat().st_mode & 0o777) == 0o600
    assert (rekeyed["work"].stat().st_mode & 0o777) == 0o700
    assert not list(rekeyed["work"].glob("run-*"))


def test_summary_and_output_carry_no_plaintext_or_key(rekeyed):
    for text in (rekeyed["stdout"], rekeyed["stderr"], rekeyed["summary_text"]):
        for secret in (KEY_A, KEY_B, PASSWORD, EMAIL, NAME, ORG, NOTE_TITLE,
                       NOTE_BODY, IP, rekeyed["apns_token"]):
            assert secret not in text
    summary = rekeyed["summary"]
    assert summary["problems"] == []
    assert summary["totals"]["cipher_reencrypted"] > 5
    assert summary["totals"]["chat_reencrypted"] == 2
    raw = rekeyed["out"].read_bytes()
    for secret in (KEY_A.encode(), KEY_B.encode(), PASSWORD.encode(),
                   NOTE_BODY.encode(), EMAIL.encode()):
        assert secret not in raw


def test_nothing_opens_under_the_old_key(rekeyed):
    old, new = rk.Keyset(KEY_A, "a"), rk.Keyset(KEY_B, "b")
    assert _old_key_opens(rekeyed["out"], old) == 0
    assert _old_key_opens(rekeyed["out"], new) > 5
    problems, _stats = rk.verify_file(rekeyed["out"], old, new)
    assert problems == []
    # The standalone verifier catches an input that was never re-keyed.
    problems, _stats = rk.verify_file(rekeyed["db_a"], old, new)
    assert any("old key" in p for p in problems)


def test_blind_indexes_are_recomputed_or_replaced(rekeyed):
    old, new = rk.Keyset(KEY_A, "a"), rk.Keyset(KEY_B, "b")
    conn = sqlite3.connect(rekeyed["out"])
    try:
        text = "\n".join(str(v) for (t,) in conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table'").fetchall()
            for row in conn.execute('SELECT * FROM "%s"' % t) for v in row)
        for plain in (NAME, EMAIL, "org:" + ORG, IP, NAME + ":" + rekeyed["pubkey"],
                      "apns:" + rekeyed["apns_token"],
                      "notification:" + NAME + ":rekey-test"):
            assert old.bi(plain) not in text, plain
        assert conn.execute("SELECT COUNT(*) FROM users WHERE user_bi=? AND email_bi=?",
                            (new.bi(NAME), new.bi(EMAIL))).fetchone()[0] == 1
        new_account = new.bi(NAME)
        assert conn.execute("SELECT COUNT(*) FROM account_devices WHERE device_bi=?",
                            (new.bi(new_account + ":" + rekeyed["pubkey"]),)).fetchone()[0] == 1
        assert conn.execute("SELECT COUNT(*) FROM apns_device_tokens WHERE token_bi=? "
                            "AND device_bi=?", (new.bi("apns:" + rekeyed["apns_token"]),
                                                new.bi(new_account + ":" + rekeyed["pubkey"]))
                            ).fetchone()[0] == 1
        message = conn.execute("SELECT message FROM error_log WHERE path='/x'").fetchone()[0]
        assert message == "lookup failed for " + new_account
        ip_hash = conn.execute("SELECT ip_hash FROM feedback").fetchone()[0]
        assert re.fullmatch(r"[0-9a-f]{64}", ip_hash)
        assert ip_hash not in (old.bi(IP), new.bi(IP))  # unrecoverable: pseudonym
        nested = json.loads(conn.execute(
            "SELECT v FROM ap_settings WHERE k='rekey-nested'").fetchone()[0])
        assert json.loads(new.open(nested["sealed"])) == {"inner": NAME}
        for table in rk.DROP_TABLES:
            assert conn.execute('SELECT COUNT(*) FROM "%s"' % table).fetchone()[0] == 0
        for room_key, body in conn.execute("SELECT room_key, body FROM chat_history"):
            envelope = rk.chat_envelope(body)
            assert rk.chat_open(new, room_key, envelope)[0] is not None
            assert rk.chat_open(old, room_key, envelope)[0] is None
    finally:
        conn.close()


def test_a_host_on_key_b_serves_the_rekeyed_copy(rekeyed):
    tmp = rekeyed["tmp"]
    data_b = tmp / "b"
    data_b.mkdir()
    shutil.copyfile(rekeyed["out"], data_b / "d1.sqlite")
    server = Server(tmp, rekeyed["sandbox_env"], data_b, KEY_B)
    try:
        client = server.client
        old_auth = {"authorization": "Bearer " + rekeyed["old_token"]}
        assert client.get("/api/accounts/sessions", headers=old_auth).status_code == 401
        bad = client.post("/api/accounts/login", json={
            "nodeName": NAME, "password": "not-the-password"})
        assert bad.status_code in (401, 403)
        by_email = client.post("/api/accounts/login", json={
            "identifier": EMAIL, "password": PASSWORD})
        assert by_email.status_code == 200, by_email.text[:300]
        login = client.post("/api/accounts/login", json={
            "nodeName": NAME, "password": PASSWORD})
        assert login.status_code == 200, login.text[:300]
        auth = {"authorization": "Bearer " + login.json()["sessionToken"]}
        profile = client.get("/api/accounts/" + NAME, headers=auth)
        assert profile.status_code == 200 and profile.json()["exists"] is True
        org = client.get("/api/orgs/" + ORG)
        assert org.status_code == 200, org.text[:300]
        assert ORG in org.text
        mine = client.get("/api/orgs", headers=auth)
        assert mine.status_code == 200
        assert {"name": ORG, "role": "owner"} in mine.json()["orgs"]
        note = client.get("/api/notes/" + rekeyed["note_id"], headers=auth)
        assert note.status_code == 200, note.text[:300]
        body = note.json()["note"]
        assert body["title"] == NOTE_TITLE and body["markdown"] == NOTE_BODY
        pings = client.get("/api/notifications?node=" + NAME, headers=auth)
        assert pings.status_code == 200, pings.text[:300]
        assert any(item.get("title") == "Hello"
                   for item in pings.json()["notifications"])
    finally:
        server.stop()


def test_remote_fork_lookups_survive_rekey(tmp_path):
    import io
    old, new = rk.Keyset(KEY_A, "a"), rk.Keyset(KEY_B, "b")
    source = tmp_path / "forks.sqlite3"
    target = tmp_path / "rekeyed.sqlite3"
    repo = "alice/engine"
    url = "https://github.com/alice/engine"
    metadata = json.dumps({"url": url, "label": "GitHub", "addedBy": "alice"})
    with sqlite3.connect(source) as conn:
        conn.execute("CREATE TABLE repositories (key_bi TEXT PRIMARY KEY, data TEXT)")
        conn.execute("INSERT INTO repositories VALUES (?,?)", (
            old.bi(repo), old.seal(json.dumps({"owner": "alice", "name": "engine"}).encode())))
        conn.execute("CREATE TABLE repo_remote_forks (repo_bi TEXT, url_bi TEXT, "
                     "created_at INTEGER, data TEXT, PRIMARY KEY (repo_bi, url_bi))")
        conn.execute("INSERT INTO repo_remote_forks VALUES (?,?,?,?)", (
            old.bi("remote-forks:" + repo), old.bi("remote-fork-url:" + url), 123, metadata))
    rk.rekey(source, target, old, new, tmp_path / "work", out=io.StringIO())
    with sqlite3.connect(target) as conn:
        row = conn.execute("SELECT created_at, data FROM repo_remote_forks "
                           "WHERE repo_bi=? AND url_bi=?", (
                               new.bi("remote-forks:" + repo),
                               new.bi("remote-fork-url:" + url))).fetchone()
        assert row == (123, metadata)
        assert conn.execute("SELECT COUNT(*) FROM repo_remote_forks "
                            "WHERE repo_bi=? OR url_bi=?", (
                                old.bi("remote-forks:" + repo),
                                old.bi("remote-fork-url:" + url))).fetchone()[0] == 0


def test_unrecoverable_identity_index_aborts_unless_allowed(tmp_path):
    old, new = rk.Keyset(KEY_A, "a"), rk.Keyset(KEY_B, "b")
    src = tmp_path / "in.sqlite3"
    conn = sqlite3.connect(src)
    conn.execute("CREATE TABLE users (user_bi TEXT PRIMARY KEY, email_bi TEXT, "
                 "ip_bi TEXT, discord_bi TEXT, data TEXT, username TEXT)")
    # Known plaintext: recomputed. Unknown plaintext in user_bi: an orphan.
    conn.execute("INSERT INTO users VALUES (?,?,?,?,?,?)", (
        old.bi("known"), old.bi("k@example.com"), None, None,
        old.seal(json.dumps({"name": "known", "email": "k@example.com"}).encode()),
        "known"))
    conn.execute("INSERT INTO users VALUES (?,?,?,?,?,?)", (
        old.bi("ghost-" + os.urandom(4).hex()), None, None, None,
        old.seal(b"{}"), ""))
    conn.commit()
    conn.close()
    work = tmp_path / "work"
    with pytest.raises(rk.RekeyError, match="users.user_bi"):
        rk.rekey(src, tmp_path / "out1.sqlite3", old, new, work)
    assert not (tmp_path / "out1.sqlite3").exists()
    import io
    summary = rk.rekey(src, tmp_path / "out2.sqlite3", old, new, work,
                       allow_orphans=True, out=io.StringIO())
    assert summary["tables"]["users"]["bi_pseudonymised"] == 1
    conn = sqlite3.connect(tmp_path / "out2.sqlite3")
    names = {r[0] for r in conn.execute("SELECT user_bi FROM users")}
    conn.close()
    assert new.bi("known") in names and len(names) == 2


def test_find_key_names_files_holding_key_material(tmp_path):
    import hashlib
    import io
    key_file = _write_key(tmp_path / "DATA_KEY.production", KEY_A)
    tree = tmp_path / "tree"
    (tree / "sub").mkdir(parents=True)
    (tree / "secrets.env").write_text("ADMIN_PASS=x\nDATA_KEY=%s\n" % KEY_A)
    (tree / "sub" / "derived.txt").write_text(hashlib.sha256(KEY_A.encode()).hexdigest())
    (tree / "clean.txt").write_text("nothing here\n" * 100000)
    out = io.StringIO()
    hits = rk.find_key(key_file, [tree, key_file], out=out)
    assert sorted(p.name for p in hits) == ["derived.txt", "secrets.env"]
    assert KEY_A not in out.getvalue()
