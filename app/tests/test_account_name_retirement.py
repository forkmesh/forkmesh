import ast
import asyncio
import sqlite3
from pathlib import Path

ENTRY = Path(__file__).resolve().parents[1] / "src" / "entry.py"
FUNCTIONS = (
    "_account_name_retired", "_retire_account_name", "_move_account_org_grants",
    "_drop_account_org_grants", "_account_last_owner_org",
)
NOW = 1_800_000_000_000


class Database:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            """
            CREATE TABLE retired_account_names (
              name_bi TEXT PRIMARY KEY, successor_bi TEXT NOT NULL DEFAULT '',
              retired_at INTEGER NOT NULL);
            CREATE TABLE org_members (org_bi TEXT, member_bi TEXT, role TEXT, name TEXT,
              created_at INTEGER, PRIMARY KEY (org_bi, member_bi));
            CREATE TABLE org_team_members (org_bi TEXT, team TEXT, member_bi TEXT, name TEXT,
              created_at INTEGER, PRIMARY KEY (org_bi, team, member_bi));
            CREATE TABLE org_team_collaborators (org_bi TEXT, team TEXT, account_bi TEXT, name TEXT,
              added_by_bi TEXT, created_at INTEGER, PRIMARY KEY (org_bi, team, account_bi));
            CREATE TABLE org_bot_tokens (token_id TEXT, created_by_bi TEXT);
            CREATE TABLE chat_channels (channel_id TEXT, created_by_bi TEXT);
            CREATE TABLE chat_channel_webhooks (webhook_id TEXT, created_by_bi TEXT);
            """
        )

    async def run(self, _env, sql, *args):
        self.db.execute(sql, args)

    async def first(self, _env, sql, *args):
        row = self.db.execute(sql, args).fetchone()
        return dict(row) if row else None


def namespace(database):
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    wanted = [node for node in tree.body
              if isinstance(node, ast.AsyncFunctionDef) and node.name in FUNCTIONS]
    assert len(wanted) == len(FUNCTIONS)
    names = {
        "d1_first": database.first, "d1_run": database.run,
        "Date": type("Date", (), {"now": staticmethod(lambda: NOW)}),
    }
    exec(compile(ast.Module(wanted, []), str(ENTRY), "exec"), names)
    return names


def run(coroutine):
    return asyncio.run(coroutine)


def test_a_deleted_name_stays_retired_for_everyone():
    database = Database()
    ns = namespace(database)
    assert not run(ns["_account_name_retired"](None, "alice"))
    run(ns["_retire_account_name"](None, "alice"))
    assert run(ns["_account_name_retired"](None, "alice"))
    assert run(ns["_account_name_retired"](None, "alice", "mallory"))


def test_only_the_renamed_account_can_take_its_old_name_back():
    database = Database()
    ns = namespace(database)
    run(ns["_retire_account_name"](None, "alice", "bob"))
    run(ns["_retire_account_name"](None, "bob", "carol"))
    assert not run(ns["_account_name_retired"](None, "alice", "carol"))
    assert not run(ns["_account_name_retired"](None, "bob", "carol"))
    assert run(ns["_account_name_retired"](None, "alice"))
    assert run(ns["_account_name_retired"](None, "alice", "mallory"))
    run(ns["_retire_account_name"](None, "carol"))
    assert run(ns["_account_name_retired"](None, "alice", "carol"))


def test_rename_moves_org_grants_and_channel_ownership():
    database = Database()
    ns = namespace(database)
    for statement in (
            "INSERT INTO org_members VALUES ('org','alice','owner','alice',1)",
            "INSERT INTO org_team_members VALUES ('org','team','alice','alice',1)",
            "INSERT INTO org_team_collaborators VALUES ('org','team','alice','alice','x',1)",
            "INSERT INTO org_bot_tokens VALUES ('t','alice')",
            "INSERT INTO chat_channels VALUES ('c','alice')",
            "INSERT INTO chat_channel_webhooks VALUES ('w','alice')"):
        database.db.execute(statement)
    run(ns["_move_account_org_grants"](None, "alice", "bob", "bob"))
    for table, column in (("org_members", "member_bi"), ("org_team_members", "member_bi"),
                          ("org_team_collaborators", "account_bi"), ("org_bot_tokens", "created_by_bi"),
                          ("chat_channels", "created_by_bi"), ("chat_channel_webhooks", "created_by_bi")):
        assert [row[0] for row in database.db.execute(f"SELECT {column} FROM {table}")] == ["bob"], table
    assert database.db.execute("SELECT name FROM org_members").fetchone()[0] == "bob"


def test_delete_drops_org_grants_and_channel_ownership():
    database = Database()
    ns = namespace(database)
    database.db.execute("INSERT INTO org_members VALUES ('org','alice','owner','alice',1)")
    database.db.execute("INSERT INTO org_team_members VALUES ('org','team','alice','alice',1)")
    database.db.execute("INSERT INTO chat_channels VALUES ('c','alice')")
    run(ns["_drop_account_org_grants"](None, "alice"))
    assert database.db.execute("SELECT COUNT(*) FROM org_members").fetchone()[0] == 0
    assert database.db.execute("SELECT COUNT(*) FROM org_team_members").fetchone()[0] == 0
    assert database.db.execute("SELECT created_by_bi FROM chat_channels").fetchone()[0] == ""


def test_the_last_owner_is_found_and_a_second_owner_clears_it():
    database = Database()
    ns = namespace(database)
    database.db.execute("INSERT INTO org_members VALUES ('org','alice','owner','alice',1)")
    database.db.execute("INSERT INTO org_members VALUES ('org','bob','member','bob',1)")
    assert run(ns["_account_last_owner_org"](None, "alice"))
    assert not run(ns["_account_last_owner_org"](None, "bob"))
    database.db.execute("UPDATE org_members SET role='owner' WHERE member_bi='bob'")
    assert not run(ns["_account_last_owner_org"](None, "alice"))


def test_every_path_that_hands_out_a_name_checks_retirement():
    source = ENTRY.read_text(encoding="utf-8")
    assert source.count("await _account_name_retired(env,") == 4
    for start, end in (("async def _delete_account_namespace", "_drop_account_org_grants(env, name_bi)"),
                       ("async def _delete_account_namespace", "_retire_account_name(env, name_bi)"),
                       ("async def _rename_account_namespace", "_retire_account_name(env, name_bi, new_name_bi)"),
                       ("async def _rename_account_namespace", "_move_account_org_grants(env, name_bi, new_name_bi")):
        assert end in source[source.index(start):source.index(start) + 9000], end


def test_a_reset_revokes_other_keys_but_keeps_the_primary_and_node_keys():
    tree = ast.parse(ENTRY.read_text(encoding="utf-8"))
    wanted = [node for node in tree.body
              if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
              and node.name in ("_account_revoke_other_keys", "_owned_nodes")]
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(
        "CREATE TABLE account_devices (account_bi TEXT, device_bi TEXT, pubkey TEXT,"
        " enabled INTEGER DEFAULT 1, revoked_at INTEGER DEFAULT 0);"
        "CREATE TABLE account_ssh_keys (account_bi TEXT, fingerprint TEXT);"
        "INSERT INTO account_devices (account_bi,device_bi,pubkey) VALUES"
        " ('a','primary','PRIMARY'),('a','node','NODE'),('a','intruder','INTRUDER'),('b','other','INTRUDER');"
        "INSERT INTO account_ssh_keys VALUES ('a','x'),('b','y');")

    async def d1_run(_env, sql, *args):
        db.execute(sql, args)

    async def d1_all(_env, sql, *args):
        return [dict(row) for row in db.execute(sql, args)]

    async def account_row(_env, name):
        return "node-bi", {"pubkey": "NODE"}

    names = {"d1_run": d1_run, "d1_all": d1_all, "_account_row": account_row,
             "clean_string": lambda value, limit: str(value or "")[:limit],
             "Date": type("Date", (), {"now": staticmethod(lambda: NOW)})}
    exec(compile(ast.Module(wanted, []), str(ENTRY), "exec"), names)
    run(names["_account_revoke_other_keys"](None, "a", {"pubkey": "PRIMARY", "nodes": ["studio"]}))
    live = {row[0] for row in db.execute("SELECT device_bi FROM account_devices WHERE revoked_at=0")}
    assert live == {"primary", "node", "other"}
    assert [row[0] for row in db.execute("SELECT account_bi FROM account_ssh_keys")] == ["b"]
    source = ENTRY.read_text(encoding="utf-8")
    reset = source[source.index("async def _account_reset_password"):]
    assert "_account_revoke_other_keys(env, name_bi, rec)" in reset[:reset.index("def _verify_email_page")]
    recovery = (ENTRY.parent / "account_recovery.py").read_text(encoding="utf-8")
    assert "_account_revoke_other_keys(env, name_bi, rec)" in recovery


def test_weak_ed25519_keys_never_verify():
    import base64
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

    source = (ENTRY.parent / "events.py").read_text(encoding="utf-8")
    tree = ast.parse(source)
    wanted = [node for node in tree.body
              if (isinstance(node, ast.Assign) and getattr(node.targets[0], "id", "") == "ED25519_WEAK_KEYS")
              or (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                  and node.name in ("b64url_decode", "ed25519_verify"))]
    names = {"base64": base64}
    exec(compile(ast.Module(wanted, []), "events.py", "exec"), names)
    identity = bytes.fromhex("01" + "00" * 31)
    forged = identity + b"\x00" * 32
    Ed25519PublicKey.from_public_bytes(identity).verify(forged, b"any message")
    encode = lambda raw: base64.urlsafe_b64encode(raw).rstrip(b"=").decode()
    assert len(names["ED25519_WEAK_KEYS"]) == 14
    for key in names["ED25519_WEAK_KEYS"]:
        assert run(names["ed25519_verify"](encode(key), encode(forged), b"any message")) is False
