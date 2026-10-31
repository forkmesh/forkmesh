import asyncio
import json
import sqlite3
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "src"))
import account_data
import hosted_repos
import marketplace_reviews
import schema


class FakeRuntime:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        for statement in (schema.SCHEMA_STATEMENTS + list(marketplace_reviews.SCHEMA)
                          + list(marketplace_reviews.ACTIVITY_SCHEMA)
                          + list(hosted_repos.TABLES)):
            self.db.execute(statement)
        self.db.execute(
            "CREATE TABLE steps_keys(account_bi TEXT PRIMARY KEY, wrapped TEXT)")
        self.db.execute(
            "CREATE TABLE steps_days(account_bi TEXT, day TEXT, data TEXT)")
        self.sessions = {}
        self.method_value = "GET"
        self.actor = ""
        self.body = {}
        self.query = {}
        self.clock = 2_000_000_000_000
        self.secrets = 0
        account_data._INVENTORY_MEMO.clear()
        account_data._EXPORT_MEMO.clear()
        account_data._TABLES_MEMO.update({"at": 0, "value": None})
        account_data._SCHEMA_READY["done"] = False

    def use(self, method, actor="", body=None, query=None):
        self.method_value = method
        self.actor = actor
        self.body = body or {}
        self.query = query or {}
        return self

    def method(self): return self.method_value
    def query_params(self): return self.query
    def now(self):
        self.clock += 1
        return self.clock
    def new_secret(self):
        self.secrets += 1
        return "k" * 40 + str(self.secrets)
    def response(self, data, status=200, cache_control=None, extra_headers=None):
        return {"status": status, "data": data, "headers": extra_headers or {}}
    async def ensure_schema(self): pass
    async def json_body(self, _limit): return self.body, ""
    async def session(self, _data):
        return self.sessions.get(self.actor, ("", None))
    async def blind(self, value): return "bi:" + value
    async def seal(self, value): return "dk:" + json.dumps(value)
    async def open(self, value):
        if isinstance(value, str) and value.startswith("dk:"):
            return json.loads(value[3:])
        return None
    async def seal_with(self, secret, value):
        return "sk:" + secret + ":" + json.dumps(value)
    async def open_with(self, secret, value):
        prefix = "sk:" + secret + ":"
        if isinstance(value, str) and value.startswith(prefix):
            return json.loads(value[len(prefix):])
        return None
    async def d1_all(self, sql, *args):
        return [dict(row) for row in self.db.execute(sql, args).fetchall()]
    async def d1_first(self, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None
    async def d1_run(self, sql, *args):
        cursor = self.db.execute(sql, args)
        self.db.commit()
        return cursor
    @staticmethod
    def changes(cursor): return cursor.rowcount

    def add(self, sql, *args):
        self.db.execute(sql, args)
        self.db.commit()


def call(runtime, path):
    return asyncio.run(account_data.handle(runtime, path))


def seeded():
    rt = FakeRuntime()
    rt.sessions["alice"] = ("bi:alice", {
        "name": "alice", "email": "alice@example.com",
        "pass_hash": "pbkdf2$secret", "totp_secret": "JBSWY3DP",
        "nodes": ["alice-laptop"], "status": "active"})
    rt.sessions["bob"] = ("bi:bob", {"name": "bob", "status": "active"})
    rt.add("INSERT INTO users(user_bi,data,username) VALUES(?,?,?)",
           "bi:alice", 'dk:{"name":"alice","pass_hash":"x","bio":"hi"}', "alice")
    rt.add("INSERT INTO users(user_bi,data,username) VALUES(?,?,?)",
           "bi:bob", 'dk:{"name":"bob"}', "bob")
    rt.add("INSERT INTO nodes(node_bi,user_bi,pubkey,data,name) VALUES(?,?,?,?,?)",
           "bi:alice-laptop", "bi:alice", "pk", "dk:{}", "alice-laptop")
    rt.add("INSERT INTO notifications(dedupe_bi,recipient_bi,ts,data) VALUES(?,?,?,?)",
           "d1", "bi:alice", 1, 'dk:{"title":"Mention"}')
    rt.add("INSERT INTO notifications(dedupe_bi,recipient_bi,ts,data) VALUES(?,?,?,?)",
           "d2", "bi:bob", 1, 'dk:{"title":"Not yours"}')
    rt.add("INSERT INTO account_sessions(session_id,account_bi,token_digest,created_at,"
           "last_seen_at,expires_at,device_label) VALUES(?,?,?,?,?,?,?)",
           "s1", "bi:alice", "abcdef", 1, 1, 9, "iPhone")
    rt.add("INSERT INTO repositories(key_bi,owner_bi,data) VALUES(?,?,?)",
           "bi:alice-laptop/proj", "bi:alice-laptop", 'dk:{"name":"proj"}')
    rt.add("INSERT INTO repo_stars(account_bi,repo_bi,created_at) VALUES(?,?,?)",
           "bi:bob", "bi:alice-laptop/proj", 1)
    rt.add("INSERT INTO orgs(org_bi,name,data,created_at) VALUES(?,?,?,?)",
           "bi:org:acme", "acme", 'dk:{"plan":"team"}', 1)
    rt.add("INSERT INTO orgs(org_bi,name,data,created_at) VALUES(?,?,?,?)",
           "bi:org:beta", "beta", 'dk:{"plan":"free"}', 1)
    rt.add("INSERT INTO org_members(org_bi,member_bi,role,name,created_at) VALUES(?,?,?,?,?)",
           "bi:org:acme", "bi:alice", "owner", "alice", 1)
    rt.add("INSERT INTO org_members(org_bi,member_bi,role,name,created_at) VALUES(?,?,?,?,?)",
           "bi:org:beta", "bi:alice", "member", "alice", 1)
    rt.add("INSERT INTO org_members(org_bi,member_bi,role,name,created_at) VALUES(?,?,?,?,?)",
           "bi:org:beta", "bi:bob", "owner", "bob", 1)
    rt.add("INSERT INTO repo_action_secrets(owner,repo,name,value,updated_by,updated_at) "
           "VALUES(?,?,?,?,?,?)", "alice", "proj", "DEPLOY", "zzz", "alice", 1)
    rt.add("INSERT INTO steps_keys(account_bi,wrapped) VALUES(?,?)",
           "bi:alice", 'dk:{"key":"package-row-key"}')
    rt.add("INSERT INTO steps_days(account_bi,day,data) VALUES(?,?,?)",
           "bi:alice", "2026-10-01", "c2VhbGVkLXdpdGgtYS1wYWNrYWdlLWtleS0xMjM0NTY3ODkwYWJjZGVm")
    return rt


def groups(payload, subject):
    return {g["table"]: g for g in payload["groups"] if g["subject"] == subject}


def test_store_round_trip_is_sealed_with_a_per_account_key():
    rt = seeded()
    put = call(rt.use("PUT", "alice", {
        "kind": "layout", "label": "Home screen", "device": "iPhone",
        "value": {"order": ["chat", "code"]}}), "/api/accounts/data/store/ios.home")
    assert put["status"] == 200 and put["data"]["revision"] == 1
    stored = rt.db.execute("SELECT data FROM account_store").fetchone()["data"]
    assert stored.startswith("sk:") and "chat" in stored
    wrapped = rt.db.execute("SELECT wrapped FROM account_store_keys").fetchone()["wrapped"]
    assert wrapped.startswith("dk:")
    got = call(rt.use("GET", "alice"), "/api/accounts/data/store/ios.home")
    assert got["data"]["item"]["value"] == {"order": ["chat", "code"]}
    assert got["data"]["item"]["label"] == "Home screen"
    assert call(rt.use("GET", "bob"), "/api/accounts/data/store/ios.home")["status"] == 404
    conflict = call(rt.use("PUT", "alice", {"value": 1, "baseRevision": 7}),
                    "/api/accounts/data/store/ios.home")
    assert conflict["status"] == 409 and conflict["data"]["revision"] == 1
    listed = call(rt.use("GET", "alice"), "/api/accounts/data/store")
    assert [i["key"] for i in listed["data"]["items"]] == ["ios.home"]
    assert "value" not in listed["data"]["items"][0]
    assert call(rt.use("DELETE", "alice"), "/api/accounts/data/store/ios.home")["status"] == 200
    assert call(rt.use("GET", "alice"), "/api/accounts/data/store")["data"]["items"] == []


def test_store_rejects_bad_keys_kinds_and_oversized_values():
    rt = seeded()
    assert call(rt.use("PUT", "alice", {"value": 1}), "/api/accounts/data/store/Bad Key")["status"] == 400
    assert call(rt.use("PUT", "alice", {"value": 1, "kind": "x"}),
                "/api/accounts/data/store/ok")["status"] == 400
    big = "x" * (account_data.MAX_VALUE_BYTES + 1)
    assert call(rt.use("PUT", "alice", {"value": big}), "/api/accounts/data/store/ok")["status"] == 413
    assert call(rt.use("GET", ""), "/api/accounts/data")["status"] == 401


def test_inventory_finds_account_nodes_repositories_and_organizations():
    rt = seeded()
    payload = call(rt.use("GET", "alice"), "/api/accounts/data")["data"]
    ids = [s["id"] for s in payload["subjects"]]
    assert ids == ["account", "repos", "org:acme", "org:beta"]
    mine = groups(payload, "account")
    assert mine["notifications"]["rows"] == 1 and mine["notifications"]["deletable"]
    assert mine["users"]["rows"] == 1 and not mine["users"]["deletable"]
    assert mine["nodes"]["rows"] == 1
    assert mine["org_members"]["rows"] == 2
    assert mine["repo_action_secrets"]["withheld"]
    assert mine["steps_keys"]["withheld"]
    assert "steps_days" in mine
    assert mine["repositories"]["rows"] == 1
    assert groups(payload, "repos")["repo_stars"]["rows"] == 1
    assert not groups(payload, "repos")["repo_stars"]["deletable"]
    acme = groups(payload, "org:acme")
    assert acme["orgs"]["rows"] == 1 and acme["orgs"]["readable"]
    beta = groups(payload, "org:beta")
    assert not beta["orgs"]["readable"]
    assert "bi:" not in json.dumps(payload)


def test_rows_redact_secrets_and_hide_other_people():
    rt = seeded()
    users = call(rt.use("GET", "alice", query={"table": "users"}), "/api/accounts/data/rows")["data"]
    fields = users["rows"][0]["fields"]
    assert fields["user_bi"] == account_data.HASHED
    assert fields["data"]["bio"] == "hi" and fields["data"]["pass_hash"] == account_data.WITHHELD
    sessions = call(rt.use("GET", "alice", query={"table": "account_sessions"}),
                    "/api/accounts/data/rows")["data"]["rows"][0]["fields"]
    assert sessions["token_digest"] == account_data.WITHHELD
    assert sessions["device_label"] == "iPhone"
    notes = call(rt.use("GET", "alice", query={"table": "notifications"}),
                 "/api/accounts/data/rows")["data"]["rows"]
    assert [n["fields"]["data"]["title"] for n in notes] == ["Mention"]
    steps = call(rt.use("GET", "alice", query={"table": "steps_days"}),
                 "/api/accounts/data/rows")["data"]["rows"][0]["fields"]
    assert steps["data"] == account_data.SEALED
    keys = call(rt.use("GET", "alice", query={"table": "steps_keys"}), "/api/accounts/data/rows")["data"]
    assert keys["rows"] == [] and keys["reason"] == "security_credential"
    beta = call(rt.use("GET", "alice", query={"table": "orgs", "subject": "org:beta"}),
                "/api/accounts/data/rows")["data"]
    assert beta["readable"] is False and beta["rows"] == []
    acme = call(rt.use("GET", "alice", query={"table": "orgs", "subject": "org:acme"}),
                "/api/accounts/data/rows")["data"]
    assert acme["rows"][0]["fields"]["data"] == {"plan": "team"}
    assert call(rt.use("GET", "alice", query={"table": "nope"}), "/api/accounts/data/rows")["status"] == 404


def test_delete_one_row_only_from_allowed_tables_and_only_your_own():
    rt = seeded()
    notes = call(rt.use("GET", "alice", query={"table": "notifications"}),
                 "/api/accounts/data/rows")["data"]["rows"]
    rowid = notes[0]["rowid"]
    assert notes[0]["deletable"]
    bob_row = rt.db.execute("SELECT rowid FROM notifications WHERE recipient_bi='bi:bob'").fetchone()[0]
    assert call(rt.use("DELETE", "alice"), f"/api/accounts/data/rows/notifications/{bob_row}")["status"] == 404
    assert call(rt.use("DELETE", "alice"), f"/api/accounts/data/rows/notifications/{rowid}")["status"] == 200
    assert rt.db.execute("SELECT COUNT(*) FROM notifications").fetchone()[0] == 1
    assert call(rt.use("DELETE", "alice"), "/api/accounts/data/rows/users/1")["status"] == 403
    payload = call(rt.use("GET", "alice"), "/api/accounts/data")["data"]
    assert "notifications" not in groups(payload, "account")


def test_export_keeps_the_phone_contract_and_withholds_credentials():
    rt = seeded()
    call(rt.use("PUT", "alice", {"kind": "subscriptions", "value": {"feeds": ["https://example.com/rss"]}}),
         "/api/accounts/data/store/ios.rss")
    response = call(rt.use("GET", "alice"), "/api/accounts/export")
    assert response["status"] == 200
    assert "attachment" in response["headers"]["content-disposition"]
    payload = response["data"]
    assert payload["schema"] == "forkmesh-account-export-v1"
    assert payload["account"]["name"] == "alice"
    assert payload["account"]["pass_hash"] == account_data.WITHHELD
    assert payload["account"]["totp_secret"] == account_data.WITHHELD
    assert payload["store"][0]["value"] == {"feeds": ["https://example.com/rss"]}
    sections = {(s["subject"], s["table"]): s for s in payload["sections"]}
    assert sections[("account", "repo_action_secrets")]["rows"] == []
    assert sections[("org:beta", "orgs")]["note"] == "organization_managers_only"
    assert sections[("account", "repositories")]["rows"][0]["data"] == {"name": "proj"}
    text = json.dumps(payload)
    assert "package-row-key" not in text and "zzz" not in text and "bi:" not in text
    assert "Not yours" not in text
    assert call(rt.use("GET", "alice"), "/api/accounts/export")["status"] == 429


def test_every_table_with_a_hashed_identity_column_is_scanned():
    rt = seeded()
    catalog = asyncio.run(account_data.tables(rt))
    live = [r[0] for r in rt.db.execute(
        "SELECT name FROM sqlite_master WHERE type='table'").fetchall()]
    for table in live:
        columns = [r[1] for r in rt.db.execute(f'PRAGMA table_info("{table}")').fetchall()]
        if table in account_data.SKIPPED_TABLES:
            continue
        if any(c.endswith("_bi") for c in columns):
            assert table in catalog, table
    for table, column in account_data.DELETABLE.items():
        assert table in catalog and column in catalog[table]["columns"], table


def test_migration_matches_the_module_schema_and_reverses():
    migrated = sqlite3.connect(":memory:")
    migrated.executescript((ROOT / "migrations" / "0287_account_store.sql").read_text())
    lazy = sqlite3.connect(":memory:")
    for statement in account_data.STORE_SCHEMA:
        lazy.execute(statement)
    for table in (account_data.STORE_TABLE, account_data.STORE_KEYS_TABLE):
        info = f'PRAGMA table_info("{table}")'
        assert migrated.execute(info).fetchall() == lazy.execute(info).fetchall()
    migrated.executescript((ROOT / "migrations" / "down" / "0287_account_store.sql").read_text())
    assert migrated.execute("SELECT COUNT(*) FROM sqlite_master WHERE type='table'").fetchone()[0] == 0


def test_account_deletion_purges_the_store():
    entry = (ROOT / "src" / "entry.py").read_text()
    start = entry.index("async def _delete_account_namespace(")
    body = entry[start:entry.index("\ndef _owned_nodes(", start)]
    assert "account_data.STORE_TABLE, account_data.STORE_KEYS_TABLE" in body


def test_rows_you_only_acted_on_keep_their_contents_closed():
    rt = seeded()
    rt.add("INSERT INTO repo_terms_flags(repo_bi,active,category,data,updated_by_bi,updated_at) "
           "VALUES(?,?,?,?,?,?)", "bi:carol/x", 1, "spam", 'dk:{"terms":"carol private"}', "bi:alice", 1)
    rt.add("INSERT INTO chat_channels(channel_id,name_bi,data,created_by_bi,created_at,updated_at) "
           "VALUES(?,?,?,?,?,?)", "c1", "bi:general", 'dk:{"title":"Mine"}', "bi:alice", 1, 1)
    flags = call(rt.use("GET", "alice", query={"table": "repo_terms_flags"}),
                 "/api/accounts/data/rows")["data"]["rows"][0]["fields"]
    assert flags["data"] == account_data.OTHERS
    channels = call(rt.use("GET", "alice", query={"table": "chat_channels"}),
                    "/api/accounts/data/rows")["data"]["rows"][0]["fields"]
    assert channels["data"] == {"title": "Mine"}
    assert account_data.withheld("admin_signup_referral_code")


def test_moderator_notes_about_you_stay_with_the_moderators():
    rt = seeded()
    rt.add("INSERT INTO moderation_blocks(scope_bi,account_bi,created_at,hide_content,data) "
           "VALUES(?,?,?,?,?)", "bi:scope:acme", "bi:alice", 1, 1,
           'dk:{"name":"alice","by":"modmia","reason":"spam ring","scope":"acme"}')
    rt.add("INSERT INTO repo_terms_flags(repo_bi,active,category,data,updated_by_bi,updated_at) "
           "VALUES(?,?,?,?,?,?)", "bi:alice-laptop/proj", 1, "malware",
           'dk:{"note":"internal: ties to case 7","actor":"adminzed"}', "bi:adminzed", 1)
    blocks = call(rt.use("GET", "alice", query={"table": "moderation_blocks"}),
                  "/api/accounts/data/rows")["data"]["rows"]
    assert len(blocks) == 1
    assert blocks[0]["fields"]["data"] == account_data.STAFF_NOTE
    assert blocks[0]["fields"]["hide_content"] == 1
    flags = call(rt.use("GET", "alice", query={"table": "repo_terms_flags",
                                               "subject": "repos"}),
                 "/api/accounts/data/rows")["data"]["rows"]
    assert len(flags) == 1
    assert flags[0]["fields"]["category"] == "malware"
    assert flags[0]["fields"]["data"] == account_data.STAFF_NOTE
    text = json.dumps(call(rt.use("GET", "alice"), "/api/accounts/export")["data"])
    for secret in ("modmia", "spam ring", "adminzed", "case 7"):
        assert secret not in text


def test_inventory_scans_in_a_few_union_statements_and_falls_back_per_table():
    rt = seeded()
    statements = []
    original = rt.d1_all

    async def counting(sql, *args):
        statements.append(sql)
        return await original(sql, *args)

    rt.d1_all = counting
    payload = call(rt.use("GET", "alice"), "/api/accounts/data")["data"]
    unions = [sql for sql in statements if "UNION ALL" in sql]
    catalog = asyncio.run(account_data.tables(rt))
    assert 0 < len(unions) <= len(catalog) // 10 + 2
    assert groups(payload, "account")["notifications"]["rows"] == 1

    rt2 = seeded()

    async def broken(sql, *args):
        if "UNION ALL" in sql:
            raise RuntimeError("too many terms in compound SELECT")
        return [dict(row) for row in rt2.db.execute(sql, args).fetchall()]

    rt2.d1_all = broken
    again = call(rt2.use("GET", "alice"), "/api/accounts/data")["data"]
    assert groups(again, "account")["notifications"]["rows"] == 1
    assert groups(again, "repos")["repo_stars"]["rows"] == 1
