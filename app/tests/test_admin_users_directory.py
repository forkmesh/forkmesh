"""Admin users directory: sort, paginate, edit, and cached nav counts."""

import ast
import asyncio
import hashlib
import html
import importlib.util
import json
import re
import sqlite3
from pathlib import Path
from types import SimpleNamespace
from urllib.parse import quote


ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
APP_RUNTIME = ROOT / "src" / "app_runtime.py"
ADMIN_CONSOLE = ROOT / "src" / "admin_console.py"
ENTRY_TEXT = (
    # entry.py + its lazily-split domain modules
    (ENTRY.parent / "federation.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "ssh_gateway_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "repo_sync.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + (ENTRY.parent / "agent_routes.py").read_text(encoding="utf-8")
    + "\n\n\n"
    + ENTRY.read_text(encoding="utf-8")
    + "\n\n\n"
)


def _entry_defs(*names):
    namespace = {"re": re, "quote": quote}
    wanted = set(names)
    for node in ast.parse(ENTRY_TEXT).body:
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in wanted:
                exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
        elif isinstance(node, ast.Assign) and any(
                isinstance(t, ast.Name) and t.id in wanted
                for t in node.targets):
            exec(ast.get_source_segment(ENTRY_TEXT, node), namespace)
    return namespace


class Harness:
    def __init__(self):
        self.db = sqlite3.connect(":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.executescript(
            "CREATE TABLE users (user_bi TEXT PRIMARY KEY, data TEXT NOT NULL,"
            " email_bi TEXT, username TEXT, is_admin INTEGER NOT NULL DEFAULT 0,"
            " signup_origin TEXT NOT NULL DEFAULT 'unknown',"
            " early_ios_tester INTEGER NOT NULL DEFAULT 0);"
            "CREATE UNIQUE INDEX idx_users_email ON users(email_bi);"
            "CREATE TABLE pending_verifications (name_bi TEXT PRIMARY KEY,"
            " data TEXT);"
            "CREATE TABLE error_log (ts INTEGER);"
        )
        self.queries = []
        self.cache = {}
        self.purged = []
        self.broadcasts = []
        self.referrals = []
        self.now = 1_800_000_000_000
        spec = importlib.util.spec_from_file_location(
            "admin_console_under_test", ADMIN_CONSOLE)
        self.console = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.console)
        runtime = _entry_defs(
            "_admin_href", "_admin_query", "_account_kind",
            "_WAITLIST_EMAIL_RE", "_waitlist_normalize_email",
            "_host_background", "_host_single_flight")
        runtime.update(self._stubs())
        # Nav counts ride the D1 read guard's shared copy.
        guard_spec = importlib.util.spec_from_file_location(
            "d1_read_guard_under_test", ROOT / "src" / "d1_read_guard.py")
        self.guard = importlib.util.module_from_spec(guard_spec)
        guard_spec.loader.exec_module(self.guard)
        self.guard._bind_runtime({**runtime, "time": __import__("time")})
        runtime["d1_shared_read"] = self.guard.shared_read
        runtime["d1_shared_forget"] = self.guard.shared_forget
        shell_spec = importlib.util.spec_from_file_location(
            "admin_shell_under_test", ROOT / "src" / "admin_shell.py")
        shell = importlib.util.module_from_spec(shell_spec)
        shell_spec.loader.exec_module(shell)
        runtime["_admin_shell"] = shell
        self.console._bind_runtime(runtime)

    def add_user(self, name, index, **extra):
        rec = {"name": name, "kind": "user", "status": "active",
               "pass_hash": "x", "email": name + "@example.com",
               "created_at": 1_700_000_000_000 + index}
        rec.update(extra)
        self.db.execute(
            "INSERT INTO users (user_bi,data,email_bi,username,is_admin,"
            "signup_origin) VALUES (?,?,?,?,?,?)",
            (self.bi(name), json.dumps(rec), self.bi(rec["email"]), name,
             1 if name.endswith("0") else 0,
             "referral" if index % 2 else "organic"))

    @staticmethod
    def bi(value):
        return hashlib.sha256(str(value).encode()).hexdigest()

    def record(self, name):
        row = self.db.execute(
            "SELECT data FROM users WHERE user_bi=?",
            (self.bi(name),)).fetchone()
        return json.loads(row["data"])

    def _stubs(self):
        harness = self

        async def d1_all(_env, sql, *args):
            harness.queries.append(sql)
            return [dict(r) for r in harness.db.execute(sql, args)]

        async def d1_first(_env, sql, *args):
            rows = await d1_all(_env, sql, *args)
            return rows[0] if rows else None

        async def d1_run(_env, sql, *args):
            harness.queries.append(sql)
            harness.db.execute(sql, args)

        async def blind_index(_env, value):
            return harness.bi(value)

        async def decrypt_row(_env, value):
            return json.loads(value) if value else None

        async def account_row(env, name):
            name_bi = harness.bi(name)
            row = await d1_first(
                env, "SELECT data FROM users WHERE user_bi=?", name_bi)
            return name_bi, json.loads(row["data"]) if row else None

        async def save_account(_env, name_bi, rec, email_bi=None, ip_bi=None):
            harness.db.execute(
                "UPDATE users SET data=?, email_bi=COALESCE(?, email_bi) "
                "WHERE user_bi=?", (json.dumps(rec), email_bi, name_bi))

        async def enqueue_verification(_env, name_bi, name, email):
            harness.db.execute(
                "INSERT OR REPLACE INTO pending_verifications VALUES (?,?)",
                (name_bi, json.dumps({"name": name, "email": email})))

        async def finalize_referral(_env, name_bi):
            harness.referrals.append(name_bi)

        async def on_email_verified(_env, _bi, _name):
            return None

        async def edge_cache_match(key):
            return harness.cache.get(key)

        async def edge_cache_put(key, response):
            harness.cache[key] = response

        async def edge_cache_delete(key):
            harness.purged.append(key)
            harness.cache.pop(key, None)

        async def response_json(response):
            return json.loads(response.body)

        async def broadcast(_env, _request, _kind, name):
            harness.broadcasts.append(name)

        async def best_effort(awaitable):
            await awaitable

        return {
            "Date": SimpleNamespace(now=lambda: harness.now),
            "json": json,
            "d1_all": d1_all, "d1_first": d1_first, "d1_run": d1_run,
            "blind_index": blind_index, "decrypt_row": decrypt_row,
            "clean_string": lambda value, n: str(value or "")[:n],
            "_html_escape": lambda value: html.escape(str(value)),
            "MAX_NODE_NAME": 64, "MAX_PROFILE_BIO": 500,
            "MAX_PROFILE_LOCATION": 120, "MAX_PROFILE_TIMEZONE": 80,
            "valid_node_name": lambda n: bool(re.fullmatch(r"[a-z0-9-]+", n)),
            "_clean_profile_timezone": (
                lambda raw: raw if raw in ("", "Europe/Paris") else ""),
            "_account_row": account_row, "_save_account": save_account,
            "_enqueue_verification": enqueue_verification,
            "_finalize_referral_signup": finalize_referral,
            "_hackathon": SimpleNamespace(on_email_verified=on_email_verified),
            "_users_directory_memos_clear": lambda: None,
            "ACCOUNT_LOOKUP_CACHE_PREFIX": "https://i/api/accounts/",
            "USERS_DIRECTORY_CACHE_KEY": "https://i/api/accounts/users",
            "USERS_WORLD_CACHE_KEY": "https://i/api/accounts/users?view=world",
            "edge_cache_match": edge_cache_match,
            "edge_cache_put": edge_cache_put,
            "edge_cache_delete": edge_cache_delete,
            "json_response": lambda data, cache_control=None: SimpleNamespace(
                body=json.dumps(data), cache_control=cache_control),
            "_response_json": response_json,
            "_best_effort_inbox_side_effect": best_effort,
            "_ap_broadcast_actor_update": broadcast,
            "AP_ACTOR_USER": "user",
        }

    def run(self, coro):
        return asyncio.run(coro)


def _seeded(count=30):
    harness = Harness()
    for index in range(count):
        harness.add_user("user%02d" % index, index)
    return harness


def _rendered_names(markup):
    return re.findall(r'<td><a class="who" href="[^"]*user=(user\d+)">', markup)


def test_users_list_defaults_to_newest_first_in_pages():
    h = _seeded()
    markup = h.run(h.console._render_table_view(
        None, "users", admin_query="admin=root", params={}))
    names = _rendered_names(markup)
    assert len(names) == 30  # 50 per page by default
    assert names[0] == "user29" and names[-1] == "user00"
    assert "Showing 1–30 of 30" in markup
    assert "Page 1 of 1" in markup
    # The generic newest-100 read never runs for the users table.
    assert not any("SELECT rowid AS _rowid_, * FROM users ORDER BY" in q
                   for q in h.queries)


def test_users_list_sorts_by_username_and_pages():
    h = _seeded()
    params = {"order": ["username"], "dir": ["asc"], "per": ["25"],
              "page": ["2"]}
    markup = h.run(h.console._render_table_view(
        None, "users", admin_query="admin=root", params=params))
    assert _rendered_names(markup) == ["user25", "user26", "user27",
                                       "user28", "user29"]
    assert "Showing 26–30 of 30" in markup
    assert "Page 2 of 2" in markup
    assert 'aria-sort="ascending"' in markup
    # Header link flips direction and returns to page 1; pager keeps sort.
    assert "order=username&amp;dir=desc&amp;per=25" in markup
    assert "order=username&amp;dir=asc&amp;per=25\">« First" in markup
    assert 'aria-disabled="true" style="opacity:.5">Next ›</span>' in markup


def test_users_list_clamps_pages_and_searches_by_prefix():
    h = _seeded()
    markup = h.run(h.console._render_table_view(
        None, "users", params={"page": ["99"], "per": ["25"],
                               "order": ["bogus"], "dir": ["sideways"]}))
    assert "Page 2 of 2" in markup
    assert _rendered_names(markup)[0] == "user04"  # newest-first page 2

    markup = h.run(h.console._render_table_view(
        None, "users", params={"q": ["USER1"]}))
    assert sorted(_rendered_names(markup)) == [
        "user%02d" % i for i in range(10, 20)]
    assert "10 account(s) matching" in markup


def test_clicking_a_user_opens_the_editor():
    h = _seeded(3)
    markup = h.run(h.console._render_table_view(
        None, "users", admin_query="admin=root", params={}))
    assert 'data-href="?admin=root&amp;view=users&amp;user=user02"' in markup

    editor = h.run(h.console._render_table_view(
        None, "users", admin_query="admin=root", user_filter="user02",
        params={}))
    assert 'id="user-detail"' in editor
    assert "action=update_user" in editor
    assert 'name="email" value="user02@example.com"' in editor
    assert 'name="profile_bio"' in editor
    assert "Set password" in editor  # existing account tools stay beside it

    missing = h.run(h.console._render_table_view(
        None, "users", user_filter="nobody", params={}))
    assert "No user account named" in missing


def _form(**fields):
    base = {"name": "user01", "email": "user01@example.com",
            "email_notifications": "1"}
    base.update(fields)
    return {k: [v] for k, v in base.items() if v is not None}


def test_update_user_saves_validated_fields_and_audits_names_only():
    h = _seeded(3)
    banner, details = h.run(h.console._admin_update_user(
        None, None, _form(email="New@Example.com", email_verified="1",
                          profile_bio="  hello  ",
                          profile_timezone="Europe/Paris"), "root"))
    rec = h.record("user01")
    assert banner.startswith("Saved 'user01'")
    assert rec["email"] == "new@example.com"
    assert rec["profile_bio"] == "hello"
    assert rec["profile_timezone"] == "Europe/Paris"
    assert rec["email_verified"] is True
    assert rec["email_verification_method"] == "admin_override"
    assert rec["email_verified_by"] == "root"
    assert details["adminVerifiedOverride"] is True
    assert "hello" not in json.dumps(details)
    assert h.referrals == [h.bi("user01")]
    assert h.broadcasts == ["user01"]  # bio federates
    assert "https://i/api/accounts/user01" in h.purged
    email_bi = h.db.execute(
        "SELECT email_bi FROM users WHERE username='user01'").fetchone()[0]
    assert email_bi == h.bi("new@example.com")

    banner, _ = h.run(h.console._admin_update_user(
        None, None, _form(email="new@example.com", email_verified="1",
                          profile_bio="hello",
                          profile_timezone="Europe/Paris"), "root"))
    assert banner == "No changes to save for 'user01'."


def test_update_user_refuses_bad_input_without_writing():
    h = _seeded(3)
    before = h.record("user01")
    for fields, reason in (
            ({"email": "user02@example.com"}, "email_taken"),
            ({"email": "not-an-address"}, "bad_email"),
            ({"email": ""}, "email_required"),
            ({"profile_timezone": "Mars/Base"}, "bad_profile_timezone")):
        banner, details = h.run(h.console._admin_update_user(
            None, None, _form(**fields), "root"))
        assert "failed" in banner.lower()
        assert details["reason"] == reason
    assert h.record("user01") == before

    banner, details = h.run(h.console._admin_update_user(
        None, None, _form(name="node-a"), "root"))
    assert details["reason"] == "no_such_user"


def _is_admin_column(h, name):
    return h.db.execute(
        "SELECT is_admin FROM users WHERE username=?", (name,)).fetchone()[0]


def test_editor_offers_the_administrator_toggle():
    h = _seeded(3)
    for name, checked in (("user00", True), ("user01", False)):
        editor = h.run(h.console._render_table_view(
            None, "users", admin_query="admin=root", user_filter=name,
            params={}))
        box = re.search(r'<input type="checkbox" name="is_admin"[^>]*>',
                        editor).group(0)
        assert (" checked" in box) is checked


def test_update_user_grants_and_revokes_administrator():
    h = _seeded(3)
    before = h.record("user01")
    banner, details = h.run(h.console._admin_update_user(
        None, None, _form(is_admin="1"), "user00"))
    assert banner == "Saved 'user01': is_admin."
    assert details["adminGranted"] is True
    assert details["adminRevoked"] is False
    assert _is_admin_column(h, "user01") == 1
    assert h.record("user01") == before  # flag lives outside the blob
    assert "https://i/api/accounts/user01" in h.purged

    banner, _ = h.run(h.console._admin_update_user(
        None, None, _form(is_admin="1"), "user00"))
    assert banner == "No changes to save for 'user01'."

    banner, details = h.run(h.console._admin_update_user(
        None, None, _form(), "user00"))
    assert details["adminRevoked"] is True
    assert _is_admin_column(h, "user01") == 0


def test_update_user_refuses_self_admin_revocation():
    h = _seeded(3)
    banner, details = h.run(h.console._admin_update_user(
        None, None, _form(name="user00", email="user00@example.com",
                          profile_bio="changed"), "user00"))
    assert details["reason"] == "self_admin_revoke"
    assert _is_admin_column(h, "user00") == 1
    assert "profile_bio" not in h.record("user00")


def _enroll_totp(h, name):
    rec = h.record(name)
    rec.update(totp_enrolled=True, totp_secret="SECRET",
               totp_enrolled_at=1, totp_pending_secret="OLD")
    h.db.execute("UPDATE users SET data=? WHERE user_bi=?",
                 (json.dumps(rec), h.bi(name)))


def test_editor_offers_two_factor_reset_only_when_enrolled():
    h = _seeded(3)
    editor = h.run(h.console._render_table_view(
        None, "users", admin_query="admin=root", user_filter="user01",
        params={}))
    assert 'name="reset_totp"' not in editor
    _enroll_totp(h, "user01")
    editor = h.run(h.console._render_table_view(
        None, "users", admin_query="admin=root", user_filter="user01",
        params={}))
    assert 'name="reset_totp"' in editor


def test_update_user_resets_two_factor():
    h = _seeded(3)
    _enroll_totp(h, "user01")
    banner, _ = h.run(h.console._admin_update_user(
        None, None, _form(reset_totp="1"), "user00"))
    assert banner == "Saved 'user01': totp."
    rec = h.record("user01")
    for key in ("totp_enrolled", "totp_secret", "totp_enrolled_at",
                "totp_pending_secret"):
        assert key not in rec
    assert "SECRET" not in banner


def test_update_user_refuses_self_two_factor_reset():
    h = _seeded(3)
    _enroll_totp(h, "user00")
    banner, details = h.run(h.console._admin_update_user(
        None, None, _form(name="user00", email="user00@example.com",
                          is_admin="1", reset_totp="1"), "user00"))
    assert details["reason"] == "self_totp_reset"
    assert h.record("user00")["totp_enrolled"] is True


def _early_ios_tester_column(h, name):
    return h.db.execute(
        "SELECT early_ios_tester FROM users WHERE username=?",
        (name,)).fetchone()[0]


def test_update_user_flags_and_unflags_an_early_ios_tester():
    h = _seeded(3)
    before = h.record("user01")
    banner, details = h.run(h.console._admin_update_user(
        None, None, _form(early_ios_tester="1"), "user00"))
    assert banner == "Saved 'user01': early_ios_tester."
    assert details["earlyIosTesterGranted"] is True
    assert details["earlyIosTesterRevoked"] is False
    assert _early_ios_tester_column(h, "user01") == 1
    assert h.record("user01") == before

    editor = h.run(h.console._render_table_view(
        None, "users", admin_query="admin=root", user_filter="user01",
        params={}))
    box = re.search(r'<input type="checkbox" name="early_ios_tester"[^>]*>',
                    editor).group(0)
    assert " checked" in box
    assert "<dt>Early iOS tester</dt><dd>yes</dd>" in editor

    listing = h.run(h.console._render_table_view(
        None, "users", admin_query="admin=root",
        params={"order": ["ios"], "dir": ["desc"]}))
    assert _rendered_names(listing)[0] == "user01"
    assert "early iOS" in listing

    banner, _ = h.run(h.console._admin_update_user(
        None, None, _form(early_ios_tester="1"), "user00"))
    assert banner == "No changes to save for 'user01'."

    banner, details = h.run(h.console._admin_update_user(
        None, None, _form(), "user00"))
    assert details["earlyIosTesterRevoked"] is True
    assert _early_ios_tester_column(h, "user01") == 0


def test_nav_counts_are_shared_until_an_admin_write_forgets_them():
    h = _seeded(2)
    tables = ["error_log", "users"]
    first = h.run(h.console._admin_table_counts_cached(None, tables))
    assert first == {"error_log": 0, "users": 2}
    counted = len(h.queries)
    # A cold isolate still reuses the shared edge copy.
    h.guard._READ_GUARD_LOCAL.clear()
    assert h.run(h.console._admin_table_counts_cached(None, tables)) == first
    assert len(h.queries) == counted

    h.add_user("user99", 99)
    h.run(h.console._admin_table_counts_forget())
    assert h.run(h.console._admin_table_counts_cached(
        None, tables))["users"] == 3
    h.add_user("user98", 98)
    assert h.run(h.console._admin_table_counts_cached(
        None, tables, refresh=True))["users"] == 4


def test_nav_count_copy_serves_callers_asking_for_different_tables():
    h = _seeded(2)
    # The console nav asks for a subset; the Database tab for every table.
    assert h.run(h.console._admin_table_counts_cached(None, ["users"])) == {
        "users": 2}
    counted = len(h.queries)
    h.guard._READ_GUARD_LOCAL.clear()
    wider = ["error_log", "pending_verifications", "users"]
    assert h.run(h.console._admin_table_counts_cached(None, wider)) == {
        "error_log": 0, "pending_verifications": 0, "users": 2}
    assert len(h.queries) == counted

    # A table the copy has never seen is counted once, then shared.
    h.db.execute("CREATE TABLE added_by_migration (id INTEGER)")
    h.db.execute("INSERT INTO added_by_migration VALUES (1)")
    assert h.run(h.console._admin_table_counts_cached(
        None, ["added_by_migration", "users"])) == {
            "added_by_migration": 1, "users": 2}
    counted = len(h.queries)
    h.guard._READ_GUARD_LOCAL.clear()
    assert h.run(h.console._admin_table_counts_cached(
        None, ["added_by_migration"])) == {"added_by_migration": 1}
    assert len(h.queries) == counted


def test_admin_route_wires_the_user_edit_action_and_cheap_views():
    runtime = APP_RUNTIME.read_text(encoding="utf-8")
    start = runtime.index("    async def _admin(self, request):")
    admin = runtime[start:runtime.index("\n    async def _route(", start)]
    assert 'elif action == "update_user":' in admin
    assert "await _admin_update_user(" in admin
    assert '"set_password", "resend_verify", "update_user",' in admin
    assert "await _admin_table_counts_forget()" in admin
    # Sections that show no table never pay for the table inventory.
    assert admin.index('if view == "controls":') < admin.index(
        "tables = await _admin_list_tables(self.env)",
        admin.index('elif view == "database":'))
    assert "await admin_stats(self.env)" in admin[
        admin.index("        else:\n            content = await _render_admin_overview("):]
    assert "params=params" in admin
    assert '"_admin_update_user",' in ENTRY_TEXT
