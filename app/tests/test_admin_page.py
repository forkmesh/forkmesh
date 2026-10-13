import ast
from pathlib import Path


ENTRY_PATH = Path(__file__).resolve().parents[1] / "src" / "entry.py"
APP_RUNTIME_PATH = ENTRY_PATH.with_name("app_runtime.py")
ADMIN_CONSOLE_PATH = ENTRY_PATH.with_name("admin_console.py")
ADMIN_SHELL_TEXT = ENTRY_PATH.with_name("admin_shell.py").read_text()
ENTRY_TEXT = (
    ENTRY_PATH.read_text() + "\n" + APP_RUNTIME_PATH.read_text()
    + "\n" + ADMIN_CONSOLE_PATH.read_text()
)


def _admin_function():
    module = ast.parse(APP_RUNTIME_PATH.read_text())
    for node in ast.walk(module):
        if isinstance(node, ast.AsyncFunctionDef) and node.name == "_admin":
            return node
    raise AssertionError("Relay._admin was not found")


def test_admin_page_preserves_admin_query_local():
    admin = _admin_function()
    assignments = {
        target.id
        for node in ast.walk(admin)
        for target in getattr(node, "targets", [])
        if isinstance(target, ast.Name)
    }
    assert "admin_query" in assignments

    admin_query_assignments = [
        node
        for node in ast.walk(admin)
        if isinstance(node, ast.Assign)
        and any(isinstance(target, ast.Name) and target.id == "admin_query"
                for target in node.targets)
    ]
    assert any(
        isinstance(node.value, ast.Call)
        and isinstance(node.value.func, ast.Name)
        and node.value.func.id == "_admin_query"
        for node in admin_query_assignments
    )


def test_admin_page_requires_revocable_account_cookie():
    module = ast.parse(ENTRY_TEXT)
    auth = next(
        node for node in ast.walk(module)
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_check_admin_page_auth"
    )
    calls = {
        node.func.id
        for node in ast.walk(auth)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    assert "_is_admin" in calls
    assert "_account_session_lookup" in calls
    assert "_cookie_value" in calls

    assert "Set-Cookie" in ENTRY_TEXT
    assert "_account_session_cookie(payload[\"sessionToken\"])" in ENTRY_TEXT
    assert "_clear_account_session_cookie()" in ENTRY_TEXT
    assert 'url.path == "/api/accounts/logout"' in ENTRY_TEXT
    assert '\"location\": \"/login?next=\" + quote(next_path)' in ENTRY_TEXT


def test_every_web_logout_calls_the_logout_endpoint():
    # Logout is universal: every client logout goes through
    # POST /api/accounts/logout, because only the Worker can clear the
    # HttpOnly forkmesh_admin cookie. The marketing-page header logout used to
    # skip this and only clear localStorage, so a logged-out admin could still
    # open the admin page (adhoc #184).
    repository = ENTRY_PATH.parents[2]
    surfaces = (
        (
            repository / "www" / "public" / "site-header.js",
            'fetch(appApiUrl("/api/accounts/logout"), {',
        ),
        (
            repository / "app" / "public" / "dashboard" / "js" / "02-helpers.js",
            'fetch("/api/accounts/logout", { method: "POST"',
        ),
        (
            repository / "app" / "public" / "dashboard.js",
            'fetch("/api/accounts/logout", { method: "POST"',
        ),
    )
    for surface, expected_call in surfaces:
        text = surface.read_text()
        assert expected_call in text, surface

    # The static marketing site and the App have separate origins in
    # production. Its direct App request must therefore include credentials so
    # the Worker receives and clears the App origin's HttpOnly cookie.
    marketing_header = surfaces[0][0].read_text()
    assert 'credentials: "include"' in marketing_header


def test_admin_session_reissued_from_session_token():
    # A logged-in admin whose short-lived admin-page cookie has lapsed can
    # re-mint it from their still-valid account session token, so the admin
    # dashboard no longer bounces them to a password re-entry (adhoc #163).
    assert "async def _account_admin_session" in ENTRY_TEXT
    assert 'url.path == "/api/accounts/admin-session"' in ENTRY_TEXT
    assert "_account_session_token_name(env, token)" in ENTRY_TEXT
    assert "_account_session_cookie(token)" in ENTRY_TEXT

    # The grant is gated on is_admin, never on a self-asserted name, and it
    # never mints a session token — it only trades an existing one for the cookie.
    module = ast.parse(ENTRY_TEXT)
    fn = next(
        node for node in ast.walk(module)
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_account_admin_session"
    )
    calls = {
        node.func.id
        for node in ast.walk(fn)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    assert "_is_admin" in calls
    assert "_account_session_cookie" in calls
    assert "_account_session_token" not in calls


def test_admin_page_auth_derives_admin_from_cookie():
    # The signed cookie is the source of truth for who the admin is; auth must
    # not require the ?admin= query param. Requiring it made a bare admin-path
    # visit always fail, and /login's silent admin-session resume then
    # redirect-looped between /login and the admin page forever (adhoc #168).
    module = ast.parse(ENTRY_TEXT)
    auth = next(
        node for node in ast.walk(module)
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_check_admin_page_auth"
    )
    calls = {
        node.func.id
        for node in ast.walk(auth)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    assert "_account_session_lookup" in calls
    assert "_cookie_value" in calls
    assert "parse_qs" not in calls

    # The login page never auto-resumes into a redirect loop: a resume attempt
    # seconds ago that bounced back must fall through to the password form.
    login_js = (Path(__file__).resolve().parents[1]
                / "public" / "login.js").read_text()
    assert "forkmesh.adminResumeAt" in login_js


def test_admin_console_dropped_legacy_accounts_migration_tools():
    # The legacy accounts table was dropped (migration 0042), so the console's
    # accounts-drain tooling (Move to users/nodes buttons, bulk verified-user
    # migration) is gone with it and nothing touches the accounts table.
    assert "_admin_account_migration_cell" not in ENTRY_TEXT
    assert "_admin_migrate_account_kind" not in ENTRY_TEXT
    assert "_admin_migrate_verified_users" not in ENTRY_TEXT
    assert 'action="migrate_account"' not in ENTRY_TEXT
    assert 'action="migrate_verified_users"' not in ENTRY_TEXT
    assert "FROM accounts" not in ENTRY_TEXT
    assert "DELETE FROM accounts" not in ENTRY_TEXT


def test_admin_set_password_tool_lives_on_users_table():
    # The password-reset tool moved with the records to the users table view.
    assert 'if table == "users":' in ENTRY_TEXT
    assert 'action="set_password"' in ENTRY_TEXT
    assert "def _admin_set_password" in ENTRY_TEXT


def test_admin_resend_verify_tool_on_users_table():
    # The users table exposes a "Resend verify email" button that dispatches to
    # ?action=resend_verify and re-sends the confirmation link, falling back to
    # the pending_verifications queue when email is not configured.
    assert 'action="resend_verify"' in ENTRY_TEXT
    assert ">Resend verify email</button>" in ENTRY_TEXT
    assert "def _admin_resend_verification" in ENTRY_TEXT

    module = ast.parse(ENTRY_TEXT)
    fn = next(
        node for node in ast.walk(module)
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_admin_resend_verification"
    )
    calls = {
        node.func.id
        for node in ast.walk(fn)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    assert "_account_row" in calls
    assert "_send_verification_email" in calls
    assert "_enqueue_verification" in calls

    # It is wired into the admin POST dispatcher alongside the other actions.
    admin = _admin_function()
    admin_calls = {
        node.func.id
        for node in ast.walk(admin)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    assert "_admin_resend_verification" in admin_calls


def test_admin_users_supports_a_direct_filtered_detail_view():
    table_view = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _render_table_view("):
        ENTRY_TEXT.index("def _render_admin_stats(")
    ]
    assert 'user_filter=""' in table_view
    assert 'if table == "users" else ""' in table_view
    assert "requested_user_bi = await blind_index(env, requested_user)" in table_view
    assert '"WHERE user_bi=? LIMIT 1"' in table_view
    assert 'id="user-detail"' in table_view
    assert "Back to all users" in table_view

    admin = ENTRY_TEXT[
        ENTRY_TEXT.index("    async def _admin(self, request):"):
        ENTRY_TEXT.index("\n    async def _route(", ENTRY_TEXT.index(
            "    async def _admin(self, request):"))
    ]
    assert 'params.get("user", [""])[0]' in admin


def test_admin_database_rows_open_read_only_vertical_detail_pages():
    for contract in (
        "async def _render_record_detail(",
        '"SELECT rowid AS _rowid_, * FROM " + table + " WHERE rowid=? LIMIT 1"',
        'class="card record-detail"',
        '<dl class="kv">%s</dl>',
        "Read-only vertical view",
        'action="detail"',
        'class="record-row"',
        'data-href="%s"',
        "location.href=this.dataset.href",
        'if action == "detail" and active:',
        'params.get("rowid", [""])[0]',
    ):
        assert contract in ENTRY_TEXT


def test_admin_error_log_has_grouped_24_hour_occurrence_analytics():
    for contract in (
        "Previous 24 hours",
        "Equivalent errors",
        "chart = shell.hour_bars(",
        "shell.sparkline(",
        "<th>24-hour frequency</th>",
        "WHERE ts>=? ORDER BY ts DESC LIMIT 5000",
        '"hours": [0] * 24',
        'group["hours"][23 - int(age_hours)] += 1',
        'aria-labelledby="error-analytics-title"',
    ):
        assert contract in ENTRY_TEXT


def test_admin_error_log_labels_sources_and_deletes_rows_or_groups():
    for contract in (
        "def _admin_error_source(method, path):",
        'return "JavaScript" if is_javascript else "Worker"',
        'class="src %s"',
        "<th>Source</th>",
        "<th>First seen</th><th>Last seen</th>",
        '"firstSeen": ts',
        '"lastSeen": ts',
        'action="delete_error_row"',
        'action="delete_error_group"',
        "Delete group</button></form>",
        'name="error_id"',
        "DELETE FROM error_log WHERE rowid=?",
        '"WHERE CAST(status AS TEXT)=? AND UPPER(method)=? "',
        '"AND path=? AND message=?"',
        '"delete_error_row", "delete_error_group"',
    ):
        assert contract in ENTRY_TEXT


def test_admin_timestamps_show_exact_and_relative_time():
    # Times render on the server: the relative label is computed against the
    # request, and the exact UTC moment rides in the title and datetime.
    assert "shell.time_html(" in ENTRY_TEXT
    for contract in (
        "def time_html(ms, now_ms):",
        "def relative_label(ms, now_ms):",
        'text + " ago"',
        '<time class="nowrap" datetime="%s" title="%s">%s</time>',
    ):
        assert contract in ADMIN_SHELL_TEXT


def test_admin_error_log_names_the_related_user_per_row_and_group():
    for contract in (
        "async def _error_log_actor(env, request):",
        "_account_session_record(env, request)",
        '"ALTER TABLE error_log ADD COLUMN actor TEXT NOT NULL DEFAULT \'\'"',
        "SELECT ts,status,method,path,message,actor FROM error_log ",
        'group["actors"][actor] = group["actors"].get(actor, 0) + 1',
        'group["anonymous"] += 1',
        "def _admin_error_related_users(actors, anonymous=0):",
        "def _admin_error_row_user_cell(actor):",
        "<th>Related users</th>",
        "<th>Related user</th>",
    ):
        assert contract in ENTRY_TEXT

    schema_text = (
        ENTRY_PATH.parent / "schema.py"
    ).read_text(encoding="utf-8")
    assert "actor TEXT NOT NULL DEFAULT ''" in schema_text


def test_admin_error_log_copies_messages_and_files_bot_tasks():
    for contract in (
        "def _admin_error_copy_button(text):",
        'data-copy="%s"',
        "navigator.clipboard.writeText(text)",
        "document.execCommand('copy')",
        'action="create_bot_task"',
        "Send to task</button>",
        "async def _admin_error_create_bot_task(env, form, requester):",
        '"INSERT INTO organization_tasks "',
        '"assignee": "agent"',
        '"delete_error_group", "create_bot_task"',
        '<th class=\\"r\\">Actions</th>',
    ):
        assert contract in ENTRY_TEXT
