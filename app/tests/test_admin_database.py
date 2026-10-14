"""Desktop Database tab: signed D1 browser, audited CRUD, #682 annotations.

The module runs against a real SQLite database standing in for D1, with the
restricted-table list and wallet-custody guards taken verbatim from
admin_console.py, so the boundary under test is the production one.
"""
import ast
import asyncio
import base64
import hashlib
import importlib.util
import json
import re
import sqlite3
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import urlencode


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "app" / "src" / "admin_database.py"
CONSOLE = ROOT / "app" / "src" / "admin_console.py"
NOW = 1_789_400_000_000  # 2026-09-14
ADMIN = "root"


def _console_helpers():
    wanted = {
        "_admin_contains_wallet_key", "_admin_redact_wallet_keys",
        "_admin_selected_rows_have_wallet_keys", "_admin_selected_rows_digest",
        "_admin_purge_allowed",
    }
    tree = ast.parse(CONSOLE.read_text(encoding="utf-8"))
    body = []
    for node in tree.body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted:
                body.append(node)
        elif isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id in (
                    "ADMIN_HIDDEN_TABLES", "_ADMIN_WALLET_KEY_NAMES")
                for t in node.targets):
            body.append(node)
    return ast.Module(body=body, type_ignores=[])


def _setup():
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript("""
        CREATE TABLE sensitive_audit_log (
            ts INTEGER, actor_bi TEXT, actor_label TEXT, action TEXT,
            target_type TEXT, target_token TEXT, outcome TEXT, details TEXT);
        CREATE TABLE widgets (
            id INTEGER PRIMARY KEY, owner_bi TEXT, label TEXT NOT NULL,
            data TEXT);
        CREATE TABLE world_element_purchases (
            id INTEGER PRIMARY KEY, deposit_secret TEXT, token_hash TEXT);
        CREATE TABLE feeds (id INTEGER PRIMARY KEY, data TEXT);
        CREATE TABLE telemetry (id INTEGER PRIMARY KEY, summary TEXT);
        CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT);
        CREATE TABLE nodes (name_bi TEXT PRIMARY KEY, data TEXT);
        CREATE TABLE visitors (
            day TEXT, visitor_bi TEXT, PRIMARY KEY (day, visitor_bi))
            WITHOUT ROWID;
        CREATE TABLE cluster_members (node_id TEXT PRIMARY KEY,
            mesh_url TEXT, status TEXT, heartbeat_ms INTEGER, epoch INTEGER);
        CREATE TABLE host_secrets (name TEXT PRIMARY KEY, key_id TEXT,
            nonce TEXT, ciphertext TEXT, updated_at INTEGER);
        CREATE TABLE do_storage (ns TEXT, id TEXT, k TEXT, v TEXT);
        CREATE TABLE do_alarms (ns TEXT, id TEXT, at INTEGER);
        CREATE TABLE host_cron_runs (slot INTEGER, node TEXT);
        CREATE TABLE dns_health_votes (voter TEXT, target TEXT, ok INTEGER);
        CREATE TABLE dns_reconciler_state (k TEXT, v TEXT);
        CREATE TABLE repositories (
            key_bi TEXT PRIMARY KEY, owner_bi TEXT NOT NULL,
            data TEXT NOT NULL);
        CREATE TABLE chat_history (room_key TEXT, body TEXT);
        CREATE TABLE funds_received (scope TEXT, key TEXT);
    """)
    calls = []

    def recorder(name):
        async def record(*args):
            calls.append((name,) + tuple(
                a for a in args if not isinstance(a, SimpleNamespace)))
        return record

    def rows(sql, args):
        return [dict(r) for r in db.execute(sql, args).fetchall()]

    async def d1_all(env, sql, *args):
        return rows(sql, args)

    async def d1_first(env, sql, *args):
        found = rows(sql, args)
        db.commit()
        return found[0] if found else None

    async def d1_run(env, sql, *args):
        db.execute(sql, args)
        db.commit()

    async def encrypt_row(env, obj):
        return "SEALED:" + base64.b64encode(json.dumps(obj).encode()).decode()

    async def decrypt_row(env, stored, key=None):
        if not str(stored).startswith("SEALED:"):
            return None
        return json.loads(base64.b64decode(stored[len("SEALED:"):]))

    async def blind_index(env, value):
        return "bi:" + hashlib.sha256(str(value).encode()).hexdigest()[:16]

    async def admin_authorized(env, node, ts, sig, canonical):
        expected = "sig:" + hashlib.sha256(canonical).hexdigest()
        if node == ADMIN and sig == expected:
            return {"name": node, "status": "active"}
        return None

    async def audit(env, actor, action, target_type="", target="",
                    outcome="success", details=None):
        await d1_run(
            env,
            "INSERT INTO sensitive_audit_log (ts, actor_bi, actor_label, "
            "action, target_type, target_token, outcome, details) "
            "VALUES (?,?,?,?,?,?,?,?)",
            NOW, await blind_index(env, actor), actor, action, target_type,
            await blind_index(env, "audit:" + target_type + ":" + target),
            outcome, json.dumps(details or {}, sort_keys=True))

    sessions = {"token-root": ADMIN, "token-bob": "bob"}

    async def authed_account_name(env, request, data=None):
        header = str(request.headers.get("authorization") or "")
        return sessions.get(header.removeprefix("Bearer "), "")

    async def is_admin(env, name):
        return name == ADMIN

    async def counts_cached(env, tables, refresh=False):
        return {
            t: db.execute('SELECT COUNT(*) FROM "%s"' % t).fetchone()[0]
            for t in tables
        }

    async def counts_forget():
        return None

    console = {
        "json": json, "re": re, "hashlib": hashlib,
        "d1_all": d1_all, "decrypt_row": decrypt_row,
    }
    exec(compile(_console_helpers(), str(CONSOLE), "exec"), console)

    runtime = {
        "Date": SimpleNamespace(now=lambda: NOW),
        "json_response": lambda data, status=200, cache_control=None: (
            SimpleNamespace(status=status, payload=data,
                            cache_control=cache_control)),
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "MAX_NODE_NAME": 64,
        "LOGIN_MAX_SKEW_MS": 5 * 60 * 1000,
        "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
        "encrypt_row": encrypt_row, "decrypt_row": decrypt_row,
        "blind_index": blind_index,
        "_admin_authorized": admin_authorized,
        "_authed_account_name": authed_account_name,
        "_is_admin": is_admin,
        "_audit_sensitive_action": audit,
        "_admin_console": SimpleNamespace(
            ADMIN_HIDDEN_TABLES=console["ADMIN_HIDDEN_TABLES"]),
        "_admin_table_counts_cached": counts_cached,
        "_admin_table_counts_forget": counts_forget,
        "safe_segment": lambda value: str(value or "").strip(),
    }
    for name in ("invalidate_public_catalog_snapshots",
                 "_record_repo_delete_tombstone", "_delete_repo_scoped_state",
                 "_delete_repo_named_state", "_delete_bounties_namespace",
                 "schedule_public_catalog_republish", "_bump_catalog_rev"):
        runtime[name] = recorder(name)

    async def purge_caches():
        calls.append(("purge_catalog_related_caches",))

    runtime["purge_catalog_related_caches"] = purge_caches
    for name in ("_admin_contains_wallet_key", "_admin_redact_wallet_keys",
                 "_admin_selected_rows_have_wallet_keys",
                 "_admin_selected_rows_digest"):
        runtime[name] = console[name]
    spec = importlib.util.spec_from_file_location(
        "admin_database_under_test", MODULE)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module._bind_runtime(runtime)
    return module, db, SimpleNamespace(calls=calls)


def _sign(canonical):
    return "sig:" + hashlib.sha256(canonical).hexdigest()


def _read(module, env, node=ADMIN, **fields):
    ts = str(NOW)
    signed = {name: fields.get(name, "")
              for name in module.ADMIN_DATABASE_READ_FIELDS}
    query = dict(fields, node=node, ts=ts,
                 sig=_sign(module.admin_database_read_canonical(
                     node, ts, signed)))
    request = SimpleNamespace(
        method="GET",
        url="https://app.test/api/accounts/admin-database?" + urlencode(query))
    return asyncio.run(module.admin_database_handler(env, request))


def _phone(module, env, token="token-root", method="GET", **query):
    headers = {"authorization": "Bearer " + token} if token else {}
    request = SimpleNamespace(
        method=method, headers=headers,
        url="https://app.test/api/admin/phone/database?" + urlencode(query))
    return asyncio.run(module.admin_database_phone_handler(env, request))


def _write(module, env, payload, request_id="request-0000000001",
           node=ADMIN):
    ts = str(NOW)
    payload_text = json.dumps(payload)
    body = {
        "node": node, "ts": ts, "requestId": request_id,
        "payload": payload_text,
        "sig": _sign(module.admin_database_write_canonical(
            node, ts, request_id, payload_text)),
    }

    async def text():
        return json.dumps(body)

    request = SimpleNamespace(
        method="POST", url="https://app.test/api/accounts/admin-database",
        text=text)
    return asyncio.run(module.admin_database_handler(env, request))


def test_signed_texts_match_the_desktop_vectors():
    module, _, _ = _setup()
    # desktop/tests/test_control_node.cpp builds the same two byte strings.
    assert module.admin_database_read_canonical("root", "1789400000000", {
        "op": "rows", "table": "widgets", "cursor": "9", "limit": "50",
        "filterColumn": "label", "filterMode": "contains",
        "filterValue": "a b", "order": "asc",
    }) == (b"forkmesh-admin-database-read-v1\nroot\n1789400000000\nrows\n"
           b"widgets\n9\n50\nlabel\ncontains\na b\nasc")
    assert module.admin_database_write_canonical(
        "root", "1789400000000", "request-0000000001", '{"op":"delete"}'
    ) == ("forkmesh-admin-database-write-v1\nroot\n1789400000000\n"
          "request-0000000001\n"
          + hashlib.sha256(b'{"op":"delete"}').hexdigest()).encode()


def test_requests_without_an_admin_signature_are_refused():
    module, _, env = _setup()
    assert _read(module, env, node="mallory", op="inventory").status == 401
    request = SimpleNamespace(
        method="GET",
        url="https://app.test/api/accounts/admin-database?op=inventory"
            "&node=root&ts=%d&sig=sig:forged" % NOW)
    response = asyncio.run(module.admin_database_handler(env, request))
    assert response.status == 401
    # A signature over one table does not read another.
    ts = str(NOW)
    fields = {n: "" for n in module.ADMIN_DATABASE_READ_FIELDS}
    fields.update(op="rows", table="feeds")
    sig = _sign(module.admin_database_read_canonical(ADMIN, ts, fields))
    request = SimpleNamespace(
        method="GET",
        url="https://app.test/api/accounts/admin-database?" + urlencode({
            "op": "rows", "table": "widgets", "node": ADMIN, "ts": ts,
            "sig": sig}))
    assert asyncio.run(
        module.admin_database_handler(env, request)).status == 401


def test_phone_reads_need_an_admin_bearer_session():
    module, db, env = _setup()
    for label in ("a", "b", "c"):
        db.execute("INSERT INTO widgets (label) VALUES (?)", (label,))
    db.commit()
    assert _phone(module, env, token="", op="inventory").status == 401
    assert _phone(module, env, token="token-bob", op="inventory").status == 401
    assert _phone(module, env, token="forged", op="inventory").status == 401
    inventory = _phone(module, env, op="inventory")
    assert inventory.status == 200
    assert inventory.cache_control.startswith("no-store")
    assert "widgets" in {t["name"] for t in inventory.payload["tables"]}
    page = _phone(module, env, op="rows", table="widgets", limit="2")
    assert page.status == 200
    assert [r["values"]["label"] for r in page.payload["rows"]] == ["c", "b"]
    assert page.payload["nextCursor"] == "2"
    rest = _phone(module, env, op="rows", table="widgets", cursor="2")
    assert [r["values"]["label"] for r in rest.payload["rows"]] == ["a"]
    assert _phone(module, env, op="rows", table="widgets",
                  limit="500").payload["limit"] == 50
    assert _phone(module, env, op="rows",
                  table="sensitive_audit_log").status == 403
    assert _phone(module, env, op="schema", table="widgets").status == 400
    assert _phone(module, env, method="POST", op="inventory").status == 405


def test_inventory_joins_audit_findings_storage_and_encryption_gaps():
    module, db, env = _setup()
    db.execute("INSERT INTO feeds (data) VALUES ('{\"plain\": true}')")
    db.execute("INSERT INTO widgets (label, data) VALUES ('x', 'SEALED:e30=')")
    db.execute("INSERT INTO telemetry (summary) VALUES ('crash')")
    db.commit()
    response = _read(module, env, op="inventory")
    assert response.status == 200
    assert response.cache_control.startswith("no-store")
    tables = {t["name"]: t for t in response.payload["tables"]}
    assert "sensitive_audit_log" in tables and tables[
        "sensitive_audit_log"]["restricted"]
    assert tables["telemetry"]["auditCategory"] == "drop-after-export"
    assert tables["telemetry"]["rows"] == 1
    assert tables["widgets"]["storage"] == "sealed+indexed"
    assert tables["telemetry"]["storage"] == "plaintext"
    assert tables["feeds"]["plaintextData"] is True
    assert tables["widgets"]["plaintextData"] is False
    assert tables["world_element_purchases"]["gaps"] == [
        "plaintext secret-named column: deposit_secret"]
    assert not tables["users"]["writable"]
    assert not tables["visitors"]["writable"]
    assert tables["visitors"]["rowid"] is False
    assert tables["widgets"]["writable"]
    # Drop candidates already gone are reported as landed cleanup.
    assert "commit_inbox" in response.payload["droppedSinceAudit"]
    assert "relay_self" not in response.payload["droppedSinceAudit"]


def test_rows_decrypt_sealed_blobs_redact_custody_and_page_by_rowid():
    module, db, env = _setup()
    for index in range(5):
        db.execute(
            "INSERT INTO widgets (label, data) VALUES (?, ?)",
            ("w%d" % index, "SEALED:" + base64.b64encode(json.dumps(
                {"n": index, "walletSecret": "k"} if index == 4
                else {"n": index}).encode()).decode()))
    db.commit()
    first = _read(module, env, op="rows", table="widgets", limit="2")
    assert first.status == 200
    page = first.payload
    assert [r["rowid"] for r in page["rows"]] == [5, 4]
    newest = page["rows"][0]
    assert newest["dataMode"] == "sealed"
    assert newest["walletKeys"] is True
    assert newest["data"]["walletSecret"].startswith("<restricted")
    assert newest["values"]["data"] is None  # ciphertext never ships
    assert page["nextCursor"] == "4"
    second = _read(module, env, op="rows", table="widgets", limit="2",
                   cursor=page["nextCursor"]).payload
    assert [r["rowid"] for r in second["rows"]] == [3, 2]
    filtered = _read(module, env, op="rows", table="widgets",
                     filterColumn="label", filterMode="contains",
                     filterValue="w1").payload
    assert [r["values"]["label"] for r in filtered["rows"]] == ["w1"]
    assert _read(module, env, op="rows", table="nodes").status == 403
    visitors = _read(module, env, op="rows", table="visitors")
    assert visitors.status == 200 and visitors.payload["rowid"] is False


def test_insert_seals_data_indexes_and_spends_the_request_id():
    module, db, env = _setup()
    payload = {
        "op": "insert", "table": "widgets",
        "values": {"label": "new"},
        "blind": {"owner_bi": "alice"},
        "data": {"plan": "pro"},
    }
    response = _write(module, env, payload)
    assert response.status == 200, response.payload
    row = dict(db.execute("SELECT * FROM widgets").fetchone())
    assert row["label"] == "new"
    assert row["data"].startswith("SEALED:")
    assert row["owner_bi"] == asyncio.run(
        module.blind_index(env, "alice"))
    audit = [dict(r) for r in db.execute(
        "SELECT action, outcome, details FROM sensitive_audit_log")]
    assert [(a["action"], a["outcome"]) for a in audit] == [
        ("admin.database_insert", "requested"),
        ("admin.database_insert", "success")]
    assert "pro" not in audit[1]["details"]
    replay = _write(module, env, payload)
    assert replay.status == 409
    assert replay.payload["error"] == "replayed_request"
    assert db.execute("SELECT COUNT(*) FROM widgets").fetchone()[0] == 1
    # Plaintext-JSON tables stay plaintext rather than gaining a sealed row.
    db.execute("INSERT INTO feeds (data) VALUES ('{}')")
    db.commit()
    assert _write(module, env, {"op": "insert", "table": "feeds",
                                "data": {"a": 1}},
                  request_id="request-0000000002").status == 200
    assert db.execute(
        "SELECT data FROM feeds ORDER BY id DESC").fetchone()[0] == '{"a": 1}'


def test_update_needs_the_digest_it_was_read_at():
    module, db, env = _setup()
    db.execute("INSERT INTO widgets (label, data) VALUES ('old', ?)",
               ("SEALED:" + base64.b64encode(b'{"n": 1}').decode(),))
    db.commit()
    row = _read(module, env, op="rows", table="widgets").payload["rows"][0]
    stale = _write(module, env, {
        "op": "update", "table": "widgets", "rowid": row["rowid"],
        "digest": "0" * 64, "values": {"label": "lost"}})
    assert stale.status == 409 and stale.payload["error"] == "row_changed"
    saved = _write(module, env, {
        "op": "update", "table": "widgets", "rowid": row["rowid"],
        "digest": row["digest"], "values": {"label": "kept"},
        "data": {"n": 2}}, request_id="request-0000000002")
    assert saved.status == 200, saved.payload
    stored = dict(db.execute("SELECT label, data FROM widgets").fetchone())
    assert stored["label"] == "kept"
    assert json.loads(base64.b64decode(stored["data"][7:])) == {"n": 2}
    # Raw blind indexes and ciphertext are not typed in by hand.
    raw = _write(module, env, {
        "op": "update", "table": "widgets", "rowid": row["rowid"],
        "digest": _read(module, env, op="rows", table="widgets")
        .payload["rows"][0]["digest"],
        "values": {"owner_bi": "bi:forged"}},
        request_id="request-0000000003")
    assert raw.status == 400
    wallet = _write(module, env, {
        "op": "insert", "table": "widgets", "values": {"label": "x"},
        "data": {"walletSecret": "k"}}, request_id="request-0000000004")
    assert wallet.status == 400


def test_restricted_blocked_and_rowless_tables_refuse_writes_with_audit():
    module, db, env = _setup()
    for index, table in enumerate(
            ("nodes", "users", "visitors", "sensitive_audit_log")):
        response = _write(module, env, {
            "op": "delete", "table": table, "rowids": [1]},
            request_id="request-000000001%d" % index)
        assert response.status == 403, table
        assert response.payload["error"] == "write_blocked"
    outcomes = [r[0] for r in db.execute(
        "SELECT outcome FROM sensitive_audit_log")]
    assert outcomes == ["denied"] * 4


def test_host_cluster_tables_refuse_every_write_and_hide_rows():
    """Audit H3: cluster membership, the vault and Durable Object state are
    never edited (or read) through the generic Database tab."""
    module, db, env = _setup()
    tables = ("cluster_members", "host_secrets", "do_storage", "do_alarms",
              "host_cron_runs", "dns_health_votes", "dns_reconciler_state")
    index = 0
    for table in tables:
        for payload in (
                {"op": "insert", "table": table,
                 "values": {"node_id": "n9", "mesh_url": "http://evil"}},
                {"op": "update", "table": table, "rowid": 1,
                 "values": {"mesh_url": "http://evil"}, "digest": "0" * 64},
                {"op": "delete", "table": table, "rowids": [1]}):
            index += 1
            response = _write(module, env, payload,
                              request_id="request-%010d" % (500 + index))
            assert response.status == 403, (table, payload["op"])
            assert response.payload["error"] == "write_blocked"
        assert table in module._hidden_tables()
    assert db.execute("SELECT COUNT(*) FROM cluster_members").fetchone()[0] == 0


def test_delete_is_digest_checked_for_one_row_and_bounded_for_many():
    module, db, env = _setup()
    for index in range(3):
        db.execute("INSERT INTO telemetry (summary) VALUES (?)",
                   ("crash %d" % index,))
    db.commit()
    rows = _read(module, env, op="rows", table="telemetry").payload["rows"]
    assert _write(module, env, {
        "op": "delete", "table": "telemetry", "rowids": [rows[0]["rowid"]],
        "digest": "f" * 64}).status == 409
    assert _write(module, env, {
        "op": "delete", "table": "telemetry",
        "rowids": list(range(module.ADMIN_DATABASE_DELETE_MAX + 1))},
        request_id="request-0000000002").status == 400
    done = _write(module, env, {
        "op": "delete", "table": "telemetry",
        "rowids": [r["rowid"] for r in rows]},
        request_id="request-0000000003")
    assert done.status == 200 and done.payload["rowCount"] == 3
    assert db.execute("SELECT COUNT(*) FROM telemetry").fetchone()[0] == 0
    details = json.loads(db.execute(
        "SELECT details FROM sensitive_audit_log WHERE outcome='success'"
    ).fetchone()[0])
    assert details["rowCount"] == 3 and len(details["rowDigestBefore"]) == 64


def _seed_repository(db, owner, name, sealed=True):
    key_bi = "bi:" + hashlib.sha256(
        (owner + "/" + name).encode()).hexdigest()[:16]
    owner_bi = "bi:" + hashlib.sha256(owner.encode()).hexdigest()[:16]
    record = {"owner": owner, "name": name, "description": "junk"}
    data = ("SEALED:" + base64.b64encode(json.dumps(record).encode()).decode()
            if sealed else "unreadable")
    db.execute("INSERT INTO repositories (key_bi, owner_bi, data) "
               "VALUES (?,?,?)", (key_bi, owner_bi, data))
    db.commit()
    return key_bi, owner_bi


def test_repositories_are_readable_and_only_deletable():
    module, db, env = _setup()
    _seed_repository(db, "alice", "junk")
    assert "repositories" not in module._hidden_tables()
    inventory = {t["name"]: t for t in _read(module, env).payload["tables"]}
    repos = inventory["repositories"]
    assert not repos["restricted"]
    assert repos["deletable"] and not repos["writable"]
    assert inventory["telemetry"]["deletable"]
    assert not inventory["nodes"]["deletable"]
    rows = _read(module, env, op="rows", table="repositories").payload["rows"]
    assert rows[0]["data"]["name"] == "junk"
    for index, payload in enumerate((
            {"op": "insert", "table": "repositories",
             "values": {"key_bi": "x", "owner_bi": "y"}},
            {"op": "update", "table": "repositories",
             "rowid": rows[0]["rowid"], "values": {"owner_bi": "z"},
             "digest": rows[0]["digest"]})):
        response = _write(module, env, payload,
                          request_id="request-%010d" % (700 + index))
        assert response.status == 403, payload["op"]
        assert response.payload["error"] == "write_blocked"
    assert db.execute("SELECT COUNT(*) FROM repositories").fetchone()[0] == 1


def test_repository_delete_purges_the_whole_repository():
    module, db, env = _setup()
    key_bi, owner_bi = _seed_repository(db, "alice", "junk")
    lost_bi, lost_owner_bi = _seed_repository(
        db, "bob", "lost", sealed=False)
    _seed_repository(db, "carol", "keep")
    db.execute("INSERT INTO chat_history VALUES (?, ?)",
               ("repo:alice/junk:room:general", "hi"))
    db.execute("INSERT INTO funds_received VALUES ('project', 'alice/junk')")
    db.commit()
    rows = {r["values"]["key_bi"]: r for r in _read(
        module, env, op="rows", table="repositories").payload["rows"]}
    assert _write(module, env, {
        "op": "delete", "table": "repositories",
        "rowids": list(range(
            module.ADMIN_DATABASE_REPOSITORY_DELETE_MAX + 1))},
        request_id="request-0000000801").status == 400
    done = _write(module, env, {
        "op": "delete", "table": "repositories",
        "rowids": [rows[key_bi]["rowid"], rows[lost_bi]["rowid"]]},
        request_id="request-0000000802")
    assert done.status == 200 and done.payload["rowCount"] == 2
    left = [r[0] for r in db.execute("SELECT key_bi FROM repositories")]
    assert len(left) == 1 and left[0] not in (key_bi, lost_bi)
    assert db.execute("SELECT COUNT(*) FROM chat_history").fetchone()[0] == 0
    assert db.execute("SELECT COUNT(*) FROM funds_received").fetchone()[0] == 0
    calls = env.calls
    assert ("_record_repo_delete_tombstone", key_bi, owner_bi) in calls
    assert ("_record_repo_delete_tombstone", lost_bi, lost_owner_bi) in calls
    assert ("_delete_repo_scoped_state", key_bi) in calls
    assert ("_delete_repo_scoped_state", lost_bi) in calls
    assert ("_delete_repo_named_state", "alice", "junk") in calls
    assert ("_delete_bounties_namespace", "alice", "junk") in calls
    assert ("schedule_public_catalog_republish", "alice", "junk") in calls
    assert not any(c[0] == "_delete_repo_named_state" and c[1] == "bob"
                   for c in calls)
    assert calls.count(("purge_catalog_related_caches",)) == 1
    assert calls.count(("_bump_catalog_rev",)) == 1
    actions = [r[0] for r in db.execute(
        "SELECT action FROM sensitive_audit_log WHERE outcome='success'")]
    assert actions.count("repository.delete") == 2
    assert actions.count("admin.database_delete") == 1


def test_schema_lists_the_table_ddl_and_indexes():
    module, db, env = _setup()
    db.execute("CREATE INDEX idx_widgets_label ON widgets(label)")
    db.commit()
    payload = _read(module, env, op="schema", table="widgets").payload
    assert [o["type"] for o in payload["objects"]] == ["table", "index"]
    assert "CREATE TABLE widgets" in payload["objects"][0]["sql"]


def test_route_is_lazy_and_budgeted():
    entry = (ROOT / "app" / "src" / "entry.py").read_text(encoding="utf-8")
    assert '_LazyModule("admin_database")' in entry
    assert 'url.path == "/api/accounts/admin-database"' in entry
    runtime = (ROOT / "app" / "src" / "app_runtime.py").read_text(
        encoding="utf-8")
    assert '"/api/admin/phone/database"' in runtime
    assert "_admin_database_phone_handler(self.env, request)" in runtime
    assert "import admin_database" not in entry
    footprint = (ROOT / "world" / "tools" / "build_worker_footprint.py"
                 ).read_text(encoding="utf-8")
    assert '"admin_database.py"' in footprint
