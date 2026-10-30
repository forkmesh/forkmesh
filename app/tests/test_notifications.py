#!/usr/bin/env python3
"""Worker/dashboard notification inbox contracts."""
import ast
import asyncio
import json
import re
from pathlib import Path
from urllib.parse import quote

from _dashboard_shell import assembled_dashboard, assembled_dashboard_page, deferred_dialogs
from _dashboard_bundle import assembled_dashboard_js
from _maintenance_stubs import with_maintenance_off

# MainWindowRepos.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_REPOS_SOURCES = (
    "MainWindowRepos.cpp",
    "MainWindowReposInternal.h",
    "MainWindowRepoSettings.cpp",
    "MainWindowRepoSync.cpp",
    "MainWindowMirrorPush.cpp",
    "MainWindowRepoFetch.cpp",
    "MainWindowAccounts.cpp",
)

# MainWindowAgents.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_AGENT_SOURCES = (
    "MainWindowAgents.cpp",
    "MainWindowAgentsInternal.h",
    "MainWindowAgentSync.cpp",
    "MainWindowAgentUsage.cpp",
    "MainWindowAgentList.cpp",
    "MainWindowAgentDetail.cpp",
    "MainWindowAgentLaunch.cpp",
    "MainWindowAgentCliSession.cpp",
    "MainWindowAgentEvents.cpp",
    "MainWindowAgentTranscript.cpp",
    "MainWindowAgentLanding.cpp",
    "MainWindowAgentActions.cpp",
)

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
PUBLIC = ROOT / "public"
# SCHEMA_STATEMENTS (D1 DDL) was extracted from entry.py into schema.py;
# concatenate it so the schema source-contract assertions below still resolve.
SCHEMA = ENTRY.parent / "schema.py"
ENTRY_TEXT = (
    ENTRY.read_text(encoding="utf-8") + "\n" + SCHEMA.read_text(encoding="utf-8")
    + "\n" + ENTRY.with_name("app_runtime.py").read_text(encoding="utf-8"))


def _load_notification_helpers():
    want_funcs = {
        "notification_mentions",
        "notification_payload",
        "_thread_key",
        "_html_escape",
    }
    want_assigns = {
        "NODE_NAME_RE",
        "MAX_NODE_NAME",
        "MENTION_RE",
        "NOTIFICATION_KINDS",
    }
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = []
    for node in tree.body:
        if isinstance(node, (ast.Import, ast.ImportFrom)) and any(
            alias.name == "re" for alias in node.names
        ):
            body.append(node)
        elif isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if targets & want_assigns:
                body.append(node)
        elif isinstance(node, ast.FunctionDef) and node.name in want_funcs:
            body.append(node)
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)
    ns = {}
    exec(compile(mod, str(ENTRY), "exec"), with_maintenance_off(ns))
    return ns


def _load_pending_inbox_notifier(captured, maintainers=(), node_owners=None,
                                 muted=()):
    """notify_pending_inbox with its collaborators stubbed, so the strings
    it composes for the ping inbox can be read back directly. maintainers is
    what the organization lookup answers for the repo."""
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = [node for node in tree.body
            if isinstance(node, ast.AsyncFunctionDef)
            and node.name in ("notify_pending_inbox", "_node_owner_among")
            or isinstance(node, ast.FunctionDef)
            and node.name == "pending_inbox_action"]
    assert len(body) == 3, "notify_pending_inbox not found in entry.py"
    body.extend(
        node for node in tree.body
        if isinstance(node, ast.Assign) and {
            t.id for t in node.targets if isinstance(t, ast.Name)
        } & {"PING_OPENED_KINDS", "NOTIFICATION_ITEM_URL_SEGMENTS",
             "NOTIFICATION_BODY_MAX", "PENDING_INBOX_ISSUE_ACTIONS"})
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        captured.append(dict(kwargs, recipient=recipient, kind=kind,
                             title=title))

    lookups = []

    async def org_repo_maintainers(_env, host_owner, repo):
        lookups.append((host_owner, repo))
        return list(maintainers)

    async def account_row(_env, name):
        return None, {"owner": (node_owners or {}).get(name, "")}

    async def thread_muted(_env, _owner, _repo, source, number, listener):
        return (source, number, listener) in muted

    ns = {
        "enqueue_notification": enqueue_notification,
        "_org_repo_maintainers": org_repo_maintainers,
        "_account_row": account_row,
        "_thread_muted": thread_muted,
        "clean_string": lambda value, limit=240: (
            value.strip()[:limit] if isinstance(value, str) else ""),
        "repo_web_href": lambda owner, repo: "/" + owner + "/" + repo,
        "MAX_NODE_NAME": 63,
    }
    exec(compile(mod, str(ENTRY), "exec"), with_maintenance_off(ns))
    return ns["notify_pending_inbox"]


def _read(path: Path) -> str:
    # dashboard.js is built from ordered public/dashboard/js/*.js fragments.
    if path.name == "dashboard.js":
        return assembled_dashboard_js()
    return path.read_text(encoding="utf-8")


def test_worker_exposes_first_class_notifications_route_and_schema():
    assert 'url.path in ("/api/notifications", "/api/notifications/")' in ENTRY_TEXT
    assert "async def notifications_handler" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS notifications" in ENTRY_TEXT
    assert "recipient_bi TEXT NOT NULL" in ENTRY_TEXT
    assert "dedupe_bi TEXT PRIMARY KEY" in ENTRY_TEXT
    assert "read_at INTEGER NOT NULL DEFAULT 0" in ENTRY_TEXT
    assert "idx_notifications_recipient_ts" in ENTRY_TEXT
    assert "if method == \"DELETE\":" in ENTRY_TEXT
    assert "WHERE recipient_bi=? AND dedupe_bi=?" in ENTRY_TEXT
    assert "DELETE FROM notifications WHERE recipient_bi=?" in ENTRY_TEXT
    assert '"allow": "GET, POST, DELETE"' in ENTRY_TEXT


def test_world_notification_table_has_owner_scoped_direct_delete():
    world_public = ROOT.parent / "world" / "public" / "world"
    world = _read(world_public / "world.js")
    css = _read(world_public / "world.css")
    assert "world-notification-table-header" in world
    assert "world-notification-row" in world
    assert "data-world-notification-delete" in world
    assert "async deleteWorldNotification(" in world
    assert '{ method: "DELETE" }' in world
    assert ".world-notification-table-header" in css
    delete_block = ENTRY_TEXT[
        ENTRY_TEXT.index('    if method == "DELETE":', ENTRY_TEXT.index(
            "async def notifications_handler"))
        :ENTRY_TEXT.index("async def mirror_requests_handler")
    ]
    # The owner is proven either by a session (browser) or by an account-key
    # signature that names this exact row or explicit all-inbox deletion
    # (desktop, adhoc #77).
    assert "_alert_inbox_account_name(" in delete_block
    assert 'resource=("all" if clear_all else item_id)' in delete_block
    assert "recipient_bi=? AND dedupe_bi=?" in delete_block
    assert "DELETE FROM notifications WHERE recipient_bi=?" in delete_block


def test_desktop_clear_removes_remote_operational_pings_instead_of_reading_them():
    actions = (ROOT.parents[0] / "desktop" / "src" /
               "MainWindowActions.cpp").read_text(encoding="utf-8")
    clear = actions[
        actions.index("void MainWindow::clearWebAlerts()"):
        actions.index("void MainWindow::showNotifications()")
    ]
    assert 'kAccountAlertClearProof,\n                                    QStringLiteral("all")' in clear
    assert 'QByteArrayLiteral("DELETE")' in clear
    assert 'm_webAlerts = QJsonArray();' in clear
    assert "markWebAlertsRead" not in actions


def test_desktop_pages_web_pings_and_shows_the_full_inbox_count():
    actions = (ROOT.parents[0] / "desktop" / "src" /
               "MainWindowActions.cpp").read_text(encoding="utf-8")
    header = (ROOT.parents[0] / "desktop" / "src" /
              "MainWindow.h").read_text(encoding="utf-8")
    internal = (ROOT.parents[0] / "desktop" / "src" /
                "MainWindowInternal.h").read_text(encoding="utf-8")

    assert "constexpr int kWebAlertsPageSize = 40;" in actions
    assert 'query.addQueryItem(QStringLiteral("offset")' in actions
    assert "setWebAlertsPage(m_webAlertsPage - 1)" in actions
    assert "setWebAlertsPage(m_webAlertsPage + 1)" in actions
    assert 'payload.value(QStringLiteral("total"))' in actions
    assert "m_webAlertsTotal" in header
    assert "m_badge > 99" not in internal


def test_notification_kind_contract_covers_dashboard_events():
    for kind in (
        "mention",
        "pull_submitted",
        "issue_assigned",
        "repo_shared",
        "bounty_funded",
        "bounty_paid",
        "release_published",
        "host_online",
        "host_offline",
        "pending_inbox",
    ):
        assert f'"{kind}"' in ENTRY_TEXT


def test_dashboard_uses_related_icons_on_every_ping_surface():
    dashboard = _read(PUBLIC / "dashboard.js")
    start = dashboard.index("  function notificationIcon(kind) {")
    icon_map = dashboard[start:dashboard.index(
        "\n  function notificationTimeLabel", start)]
    for kind in _load_notification_helpers()["NOTIFICATION_KINDS"]:
        assert f"      {kind}:" in icon_map
    assert 'String(kind || "").trim().toLowerCase()' in icon_map
    # Feed cards, the dropdown preview, the detail pane, and the full inbox
    # list all render the category icon rather than only the selected ping.
    assert dashboard.count("data-notification-kind-icon") == 4


def test_inbox_ping_rows_show_the_instance_logo_beside_or_under_the_icon():
    dashboard = _read(PUBLIC / "dashboard.js")
    helper = _source_block(
        dashboard, "  function notificationInstanceLogoHtml() {", "\n  }\n")
    assert 'src="/assets/logo.svg"' in helper
    assert "platformName()" in helper
    for start, end, icon in (
        ("  function renderNotificationPreview() {",
         "\n  function notificationLead", '<span class="relative">${avatar}'),
        ("  function renderNotificationModal() {",
         "\n  async function loadNotifications", "<i data-notification-kind-icon"),
    ):
        block = _source_block(dashboard, start, end)
        assert "flex-col-reverse items-center gap-1 sm:flex-row" in block
        assert block.index("${notificationInstanceLogoHtml()}") < block.index(icon)
    row = _source_block(
        _read(ROOT.parent / "native" / "ios" / "ForkMesh" / "Features" /
              "Inbox" / "InboxScreen.swift"),
        "struct InboxRow: View {", "\n}\n")
    assert "item.source == .local ? nil : settings?.project" in row
    icon = row.index("FmIcon(item.icon")
    assert icon < row.index("ProjectIcon(project: instance") < row.index(
        "VStack(alignment: .leading")


def _source_block(text, start, end):
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def test_phone_apps_give_every_ping_kind_its_own_icon_and_label():
    native = ROOT.parent / "native"
    ios_api = native / "ios" / "ForkMeshCore" / "Sources" / "FMApi"
    android = (native / "android" / "core" / "src" / "main" / "kotlin" /
               "com" / "forkmesh" / "nativeapp")
    ios_icons = _source_block(
        _read(ios_api / "ViewModels" / "NotificationsLogic.swift"),
        "public func notificationIconForKind", "default:")
    ios_labels = _source_block(
        _read(ios_api / "Models" / "Notifications.swift"),
        "public var kindLabel: String", "default:")
    kt_icons = _source_block(
        _read(android / "account" / "NotificationsLogic.kt"),
        "fun notificationIconForKind", "else ->")
    kt_labels = _source_block(
        _read(android / "model" / "Notifications.kt"),
        "val kindLabel: String", "else ->")
    ios_codepoints = _read(
        native / "ios" / "ForkMesh" / "Theme" / "MaterialIconCodepoints.swift")
    kt_codepoints = _read(
        native / "android" / "app" / "src" / "main" / "assets" / "fonts" /
        "MaterialIcons.codepoints")
    for kind in _load_notification_helpers()["NOTIFICATION_KINDS"]:
        for block in (ios_icons, ios_labels, kt_icons, kt_labels):
            assert f'"{kind}"' in block, kind
    for icon in set(re.findall(r'return "([a-z_]+)"', ios_icons)):
        assert f'"{icon}":' in ios_codepoints, icon
    for icon in set(re.findall(r'-> "([a-z_]+)"', kt_icons)):
        assert re.search(rf"^{icon}(_baseline)? ", kt_codepoints, re.M), icon


def test_notification_mentions_extract_node_handles_once_without_false_positives():
    ns = _load_notification_helpers()
    mentions = ns["notification_mentions"](
        "cc @demo-node and @alice @demo-node, ignore email@example.com and @bad_name"
    )
    assert mentions == ["alice", "demo-node"]



def test_worker_indexes_notifications_from_existing_event_sources():
    assert re.search(r"_best_effort_inbox_side_effect\(\s*notify_pending_inbox\(", ENTRY_TEXT)
    assert 'env, owner, repo, "issue", actor' in ENTRY_TEXT
    assert re.search(r"_best_effort_inbox_side_effect\(\s*notify_issue_assignees\(", ENTRY_TEXT)
    for marker in (
        "await notify_pending_inbox(env, owner, repo, \"pull\"",
        "await notify_mentions(env, owner, repo,",
        "await enqueue_notification(env, grantee, \"repo_shared\"",
        "async def notify_release_published",
    ):
        assert marker in ENTRY_TEXT
    assert re.search(r'_best_effort_inbox_side_effect\(notify_pending_inbox\(\s*'
                     r'env, owner, repo, "discussion", actor', ENTRY_TEXT)
    # Legacy Worker-held bounty funding/payout mutation is frozen; public
    # historical status polling must not emit a new event or move funds.
    bounty_handler = ENTRY_TEXT[
        ENTRY_TEXT.index("async def bounties_handler")
        :ENTRY_TEXT.index("\n\n# Cap on collaborators")
    ]
    assert "notify_bounty_event" not in bounty_handler



def test_dashboard_notifications_are_wired_to_real_api_not_mock_data():
    # The markers live in the shared chrome (header + modals partials), so
    # they ship on every composed page document.
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    assert "Notifications are intentionally hidden" not in dashboard
    assert "Notification reader modal is intentionally hidden" not in dashboard
    assert "Notifications disabled until notification data is wired" not in dashboard
    assert "const notifications = []" not in dashboard
    assert "notificationToggle" in dashboard
    assert "notificationModal" in deferred_dialogs()
    assert "data-notification-count" in dashboard
    assert "loadNotifications" in dashboard_js
    assert 'fetchJson(`/api/notifications?node=${encodeURIComponent(node)}`)' in dashboard_js
    assert "renderNotificationPreview" in dashboard_js
    assert "renderNotificationModal" in dashboard_js
    for mock in (
        "alice-node",
        "relay-eu-1",
        "Cloudflare had a blip",
    ):
        assert mock not in dashboard_js


def test_worker_exposes_signed_thread_subscription_route_and_schema():
    # Subscriptions (issue #361): a signed subscribe/unsubscribe endpoint plus the
    # table that backs it.
    urls_text = (ENTRY.parent / "urls.py").read_text(encoding="utf-8")
    assert 'REPO_SUBSCRIBE_RE = re.compile(r"^/api/repo/([^/]+)/([^/]+)/subscribe$")' in urls_text
    assert "async def subscribe_handler" in ENTRY_TEXT
    assert "REPO_SUBSCRIBE_RE.match(url.path)" in ENTRY_TEXT
    assert "await subscribe_handler(self.env, request, owner, repo)" in ENTRY_TEXT
    assert "CREATE TABLE IF NOT EXISTS thread_subscriptions" in ENTRY_TEXT
    assert "thread_bi TEXT NOT NULL" in ENTRY_TEXT
    assert "idx_thread_subscriptions_thread" in ENTRY_TEXT
    assert "forkmesh-subscribe-v1" in ENTRY_TEXT


def test_worker_auto_subscribes_commenters_and_fans_out_to_followers():
    # Anyone who comments is auto-subscribed and existing followers are notified,
    # for both issues and PRs.
    assert re.search(r"_best_effort_inbox_side_effect\(\s*notify_subscribers\(", ENTRY_TEXT)
    assert 'env, owner, repo, "issue", number, actor' in ENTRY_TEXT
    assert re.search(r"_best_effort_inbox_side_effect\(\s*subscribe_thread\(", ENTRY_TEXT)
    assert 'subscribe_thread(env, owner, repo, "issue", number, actor)' in ENTRY_TEXT
    for marker in (
        'await notify_subscribers(env, owner, repo, "pull"',
        'await subscribe_thread(env, owner, repo, "pull"',
        "async def subscribe_thread",
        "async def notify_subscribers",
    ):
        assert marker in ENTRY_TEXT


def test_subscribed_notification_kind_exists():
    assert '"subscribed"' in ENTRY_TEXT
    assert '"credits_refilled"' in ENTRY_TEXT  # issue #346 rides the same rail


def test_heartbeat_reports_credits_refilled_from_the_node_itself():
    # Issue #346: only the node knows when its own Claude Code usage window
    # refilled after running out, so it rides the already-signed heartbeat
    # (accounts/heartbeat) rather than a new endpoint.
    heartbeat_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_heartbeat"):
        ENTRY_TEXT.index("async def _account_treasury_address")
    ]
    for marker in (
        'data.get("creditsRefilled", "")',
        'credits_kind in ("5h", "weekly")',
        'await enqueue_notification(\n            env, name, "credits_refilled"',
        'dedupe="credits_refilled:" + credits_kind',
        # The balance probe is bounded by _solana_rpc_call's native
        # AbortSignal.timeout, never by asyncio.wait_for: cancelling a
        # JS-backed await leaves a Pyodide task pending and wedges the isolate.
        # It reads through the shared hourly display cache, so the fleet spends
        # one public-RPC subrequest per wallet per hour rather than one per
        # warm isolate.
        "balance_lamports, _ = await _display_balance_lamports(\n"
        "                env, wallet, name=name)",
    ):
        assert marker in heartbeat_body
    assert "await asyncio.wait_for(" not in heartbeat_body

    qt_src = ROOT.parent / "desktop" / "src"
    qt_text = "\n".join(
        p.read_text(encoding="utf-8")
        for p in sorted(qt_src.glob("MainWindow*.cpp"))
    )
    assert 'body.insert(QStringLiteral("creditsRefilled")' in qt_text
    assert "kEmailOnCreditsRefillSetting" in qt_text
    assert "maybeEmailCreditsRefilled" in qt_text


def test_email_digest_bridge_is_wired_to_the_cron_and_verified_email():
    for marker in (
        "async def send_notification_digests",
        "return await send_notification_digests(\n            env, include_general_chat=False)",
        "return await send_general_chat_digests(env)",
        'rec.get("email_verified")',
        'rec.get("email_notifications") is False',
        "NOTIFICATION_DIGEST_INTERVAL_MS",
        "NOTIFICATION_DIGEST_MIN_AGE_MS",
        "def _notification_digest_email",
        'env, email, subject, text, html, email_kind="notifications")',
    ):
        assert marker in ENTRY_TEXT


def test_notification_email_preferences_are_editable_and_host_alerts_default_off():
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("# --- Users vs nodes")
    ]
    heartbeat_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_heartbeat"):
        ENTRY_TEXT.index("async def _mirroring_owners")
    ]
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
        '"host_online": False',
        '"host_offline": False',
        '"general_chat": True',
        '"notificationPreferences": notification_email_preferences(rec)',
        '"emailNotifications": rec.get("email_notifications") is not False',
        '"notificationPreferences" in data',
        "notification_email_preferences(email_rec)",
    ):
        assert marker in ENTRY_TEXT

    assert '"notificationPreferences" in data' in profile_body
    assert '"notificationPreferences" in data' in heartbeat_body
    assert "prefs_bi = owner_bi" in heartbeat_body
    # The per-kind email switches are rendered from the ping kind catalog on
    # the one Pings settings page (#347); only the #general digest (not a
    # ping kind) is a static switch. Every switch auto-saves, no password.
    assert "data-ping-kind-list" in dashboard
    assert 'data-notification-pref="general_chat"' in dashboard
    assert 'data-notification-pref="${kind}"' in dashboard_js
    assert "host_online: false" in dashboard_js
    assert "host_offline: false" in dashboard_js
    assert "general_chat: true" in dashboard_js
    assert "saveNotificationPreferences" not in dashboard_js
    assert "data-notification-preferences-password" not in dashboard
    assert "Save notifications" not in dashboard
    assert "notificationPreferences: { [kind]:" in dashboard_js


def test_daily_general_chat_digest_counts_encrypted_messages_without_emailing_contents():
    for marker in (
        "GENERAL_CHAT_EMAIL_INTERVAL_MS",
        "async def send_general_chat_digests",
        "await send_general_chat_digests(env)",
        "SELECT COUNT(*) AS c FROM chat_history",
        "FLAGSHIP_ROOM_KEY",
        "Message contents are not sent over email",
        "notification_email_enabled(rec, \"general_chat\")",
    ):
        assert marker in ENTRY_TEXT
    assert "GENERAL_CHAT_ROOM_PASSPHRASE" not in ENTRY_TEXT
    assert "decrypt_general_chat" not in ENTRY_TEXT


def test_qt_node_syncs_email_notification_preferences_over_heartbeat():
    qt_src = ROOT.parent / "desktop" / "src"
    header = (qt_src / "MainWindowInternal.h").read_text(encoding="utf-8")
    setup = (qt_src / "MainWindowSetup.cpp").read_text(encoding="utf-8")
    settings = (qt_src / "MainWindowSettings.cpp").read_text(encoding="utf-8")

    assert "kEmailNotifyGeneralChatSetting" in header
    assert "kEmailNotifyHostOnlineSetting" in header
    assert "kEmailNotifyHostOfflineSetting" in header
    assert "QJsonObject MainWindow::emailNotificationPreferencesPayload() const" in setup
    assert 'body.insert(QStringLiteral("notificationPreferences")' in setup
    # Every emailable kind, from the catalog; the legacy keys still back the
    # first thirteen (#347).
    pings = (qt_src / "PingSettings.h").read_text(encoding="utf-8")
    assert "forkmesh::pings::emailKinds()" in setup
    for legacy in ("generalChat", "hostOnline", "hostOffline"):
        assert 'QStringLiteral("%s")' % legacy in pings
    assert '{"host_online", "devices", "Host came online", true, false}' in pings
    # Pushed only on the beat after a local change, then the relay's copy is
    # adopted — restating it every minute undid switches flipped elsewhere.
    assert "kSettingsPushPendingSetting" in setup
    assert "acknowledgePingSettingsDelivery();" in setup
    assert "forkmesh::pings::adoptRelaySettings(resp" in setup
    assert "Email me a daily #general count" in settings


def test_thread_key_is_stable_and_scoped():
    ns = _load_notification_helpers()
    key = ns["_thread_key"]("alice", "repo", "issue", 7)
    assert key == "thread:alice/repo:issue:7"
    # A brand-new submission (no durable number) collapses to a shared 0 key,
    # which the subscribe/notify helpers guard against.
    assert ns["_thread_key"]("alice", "repo", "issue", 0).endswith(":issue:0")
    assert ns["_thread_key"]("a", "b", "pull", None).endswith(":pull:0")


def test_pending_inbox_ping_names_the_repo_the_submitter_used():
    # issue #650: an organization alias (forkmesh/forkmesh) is backed by a
    # node account (jett) that holds the inbox row. Naming the node here made
    # the dashboard Feed contradict the ForkBot confirmation directly above
    # it, so one submission read as two repositories.
    captured = []
    notify = _load_pending_inbox_notifier(captured)
    asyncio.run(notify(object(), "jett", "forkmesh", "issue", "alice",
                       "Improve the README", 7, public_owner="forkmesh"))

    assert len(captured) == 1
    ping = captured[0]
    # Delivered to the account that holds the inbox and runs the node...
    assert ping["recipient"] == "jett"
    # ...but every string it shows names the repo the submitter used, matching
    # the confirmation, the web URL and this ping's own href.
    assert ping["title"] == "Issue submitted for forkmesh/forkmesh"
    assert ping["repo"] == "forkmesh/forkmesh"
    assert ping["href"] == "/forkmesh/forkmesh/issues/7"
    # The hosting node still matters to this recipient, who owns it, so it is
    # spelled out rather than dropped.
    assert ping["body"] == "Improve the README (hosted on your jett node)"
    assert ping["meta"]["hostOwner"] == "jett"
    # Dedupe is delivery identity, not display: keeping it on the owning
    # account keeps this ping matching the ones earlier releases wrote.
    assert ping["dedupe"].startswith("pending:jett/forkmesh:issue:alice:7:")

    # notification_payload clips the body at NOTIFICATION_BODY_MAX, so a long
    # message must not push the reconciling note off the end of it.
    captured.clear()
    asyncio.run(notify(object(), "jett", "forkmesh", "issue", "alice",
                       "R" * 200, 8, public_owner="forkmesh", text="C" * 2000))
    body_max = int(re.search(
        r"^NOTIFICATION_BODY_MAX = (\d+)$", ENTRY_TEXT, re.M).group(1))
    assert len(captured[0]["body"]) <= body_max
    assert captured[0]["body"].endswith(" (hosted on your jett node)")


def test_new_pull_request_ping_for_a_plain_node_repo():
    captured = []
    notify = _load_pending_inbox_notifier(captured)
    asyncio.run(notify(object(), "source", "forkmesh", "pull", "alice",
                       "Add relay retries", 3, opened=True))

    ping = captured[0]
    assert ping["recipient"] == "source"
    assert ping["kind"] == "pull_submitted"
    assert ping["title"] == "New pull request in source/forkmesh"
    assert ping["repo"] == "source/forkmesh"
    # A new item links straight to itself rather than to the repository.
    assert ping["href"] == "/source/forkmesh/pull/3"
    # No alias, so no parenthetical: the body stays the submitted title.
    assert ping["body"] == "Add relay retries"
    assert ping["meta"]["hostOwner"] == "source"
    assert ping["dedupe"].startswith("pending:source/forkmesh:pull:alice:3:")


def test_comment_ping_carries_what_was_written():
    # A comment has no title of its own, so its ping used to read "Issue
    # submitted" over "A signed item is waiting in your desktop inbox." and
    # the phone banner built from it said nothing either.
    captured = []
    notify = _load_pending_inbox_notifier(captured)
    asyncio.run(notify(object(), "source", "forkmesh", "issue", "alice",
                       "", 12, text="The relay retries forever on 503."))
    asyncio.run(notify(object(), "source", "forkmesh", "issue", "alice",
                       "", 12, text="Fixed by capping the backoff."))

    first, second = captured
    assert first["title"] == "New comment in source/forkmesh"
    assert first["body"] == "The relay retries forever on 503."
    assert second["body"] == "Fixed by capping the backoff."
    # Two comments from one person are two pings, not one overwritten row.
    assert first["dedupe"] != second["dedupe"]


def test_follower_ping_shows_the_comment_not_new_activity():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = [node for node in tree.body
            if isinstance(node, (ast.AsyncFunctionDef, ast.Assign)) and (
                getattr(node, "name", "") == "notify_subscribers"
                or {t.id for t in getattr(node, "targets", [])
                    if isinstance(t, ast.Name)} & {"NOTIFICATION_BODY_MAX"})]
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)
    captured = []

    async def enqueue_notification(_env, recipient, kind, title, **kwargs):
        captured.append(dict(kwargs, recipient=recipient, kind=kind,
                             title=title))

    async def noop(*_args, **_kwargs):
        return "bi"

    async def rows(*_args, **_kwargs):
        return [{"data": "bob"}]

    async def decrypt_row(_env, data):
        return {"node": data}

    ns = {
        "enqueue_notification": enqueue_notification,
        "ensure_schema": noop, "blind_index": noop, "d1_all": rows,
        "decrypt_row": decrypt_row,
        "_thread_key": lambda *parts: ":".join(map(str, parts)),
        "notification_mentions": lambda *_parts: [],
        "valid_node_name": lambda name: bool(name),
        "clean_string": lambda value, limit=240: (
            value.strip()[:limit] if isinstance(value, str) else ""),
        "MAX_NODE_NAME": 63,
    }
    exec(compile(mod, str(ENTRY), "exec"), with_maintenance_off(ns))
    asyncio.run(ns["notify_subscribers"](
        object(), "acme", "widgets", "pull", 9, "alice", "",
        "Rebased onto main, ready for another look.", "/acme/widgets"))

    ping, = captured
    assert ping["recipient"] == "bob"
    assert ping["title"] == "New pull request comment in acme/widgets"
    assert ping["body"] == "Rebased onto main, ready for another look."


def test_new_issue_ping_reaches_the_host_and_organization_maintainers():
    # Issue #714: opening an issue raises its own ping kind, and the people
    # who run the organization fronting the repo hear about it too — not only
    # the account whose node hosts it.
    captured = []
    notify = _load_pending_inbox_notifier(
        captured, maintainers=["jett", "maya", "ops"])
    asyncio.run(notify(object(), "jett", "forkmesh", "issue", "alice",
                       "Crash on launch", 714, public_owner="forkmesh",
                       opened=True))

    assert [p["recipient"] for p in captured] == ["jett", "maya", "ops"]
    for ping in captured:
        assert ping["kind"] == "issue_opened"
        assert ping["title"] == "New issue in forkmesh/forkmesh"
        assert ping["href"] == "/forkmesh/forkmesh/issues/714"
        assert ping["source"] == "issue"
        assert ping["meta"]["number"] == 714
    # Only the hosting account is told which of its nodes serves the repo.
    assert captured[0]["body"] == "Crash on launch (hosted on your jett node)"
    assert captured[1]["body"] == "Crash on launch"


def test_a_node_host_owned_by_a_maintainer_files_one_ping_per_person():
    captured = []
    notify = _load_pending_inbox_notifier(
        captured, maintainers=["jett", "maya"],
        node_owners={"fmnext-n1": "jett"})
    asyncio.run(notify(object(), "fmnext-n1", "forkmesh", "pull", "kaif",
                       "Add Forgot password", 286, public_owner="forkmesh",
                       opened=True))

    assert [p["recipient"] for p in captured] == ["jett", "maya"]
    assert captured[0]["body"] == "Add Forgot password"


def test_a_node_host_whose_owner_is_not_a_maintainer_still_hears_it():
    captured = []
    notify = _load_pending_inbox_notifier(
        captured, maintainers=["maya"], node_owners={"fmnext-n1": "jett"})
    asyncio.run(notify(object(), "fmnext-n1", "forkmesh", "pull", "kaif",
                       "Add Forgot password", 286, public_owner="forkmesh",
                       opened=True))

    assert [p["recipient"] for p in captured] == ["fmnext-n1", "maya"]


def test_inbox_activity_on_an_existing_item_stays_a_host_only_inbox_ping():
    # A comment on a pull request used to be sent as "Pull request
    # submitted", which made a new pull request indistinguishable from
    # chatter on an old one.
    captured = []
    notify = _load_pending_inbox_notifier(captured, maintainers=["maya"])
    asyncio.run(notify(object(), "source", "forkmesh", "pull", "alice",
                       "Looks good to me", 3))

    assert len(captured) == 1
    ping = captured[0]
    assert ping["recipient"] == "source"
    assert ping["kind"] == "pending_inbox"
    assert ping["title"] == "Pull request activity for source/forkmesh"
    assert ping["href"] == "/source/forkmesh/pull/3"


def test_a_pull_request_event_without_text_says_what_happened():
    captured = []
    notify = _load_pending_inbox_notifier(captured)
    for event in ({"type": "review", "state": "approved", "body": ""},
                  {"type": "status", "state": "closed", "body": ""},
                  {"type": "thread-state", "state": "resolved", "body": ""}):
        asyncio.run(notify(object(), "source", "forkmesh", "pull", "alice",
                           "Add relay retries", 3, text=event["body"],
                           event=event))

    approved, closed, resolved = captured
    assert approved["title"] == "Pull request approved in source/forkmesh"
    assert approved["body"] == (
        "Add relay retries\n\nalice approved pull request #3.")
    assert approved["href"] == "/source/forkmesh/pull/3"
    assert closed["title"] == "Pull request closed in source/forkmesh"
    assert closed["body"].endswith("alice closed pull request #3.")
    assert resolved["title"] == "Review thread resolved in source/forkmesh"
    for ping in captured:
        assert "desktop inbox" not in ping["body"]
    assert len({ping["dedupe"] for ping in captured}) == 3


def test_an_approval_with_a_note_keeps_the_note():
    captured = []
    notify = _load_pending_inbox_notifier(captured)
    asyncio.run(notify(object(), "source", "forkmesh", "pull", "alice", "", 3,
                       text="Ship it.",
                       event={"type": "review", "state": "approved",
                              "body": "Ship it."}))

    ping, = captured
    assert ping["title"] == "Pull request approved in source/forkmesh"
    assert ping["body"] == "alice approved pull request #3.\n\nShip it."


def test_an_issue_change_names_the_change_and_the_issue():
    captured = []
    notify = _load_pending_inbox_notifier(captured)
    asyncio.run(notify(object(), "source", "forkmesh", "issue", "alice", "", 12,
                       event={"type": "assignees", "assignees": ["bob"]}))
    asyncio.run(notify(object(), "source", "forkmesh", "issue", "alice", "", 12,
                       event={"type": "status", "status": "closed"}))
    asyncio.run(notify(object(), "source", "forkmesh", "issue", "alice", "", 12,
                       event={"type": "unknown"}))

    assigned, closed, other = captured
    assert assigned["title"] == "Issue assigned in source/forkmesh"
    assert assigned["body"] == "alice assigned issue #12 to bob."
    assert assigned["href"] == "/source/forkmesh/issues/12"
    assert closed["title"] == "Issue closed in source/forkmesh"
    assert closed["body"] == "alice closed issue #12."
    assert other["body"] == "There's new activity on issue #12."


def test_an_owner_who_muted_the_thread_hears_nothing_more_from_it():
    captured = []
    notify = _load_pending_inbox_notifier(
        captured, muted={("pull", 3, "source")})
    asyncio.run(notify(object(), "source", "forkmesh", "pull", "alice", "", 3,
                       text="Another comment."))
    assert captured == []
    asyncio.run(notify(object(), "source", "forkmesh", "pull", "alice",
                       "Add relay retries", 3, opened=True))
    assert [p["kind"] for p in captured] == ["pull_submitted"]


def test_every_inbox_creation_path_marks_its_ping_opened():
    runtime = (ROOT / "src" / "microjobs_runtime.py").read_text(encoding="utf-8")
    forkbot = (ROOT / "src" / "forkbot.py").read_text(encoding="utf-8")
    assert 'number, opened=event_type == "open",' in ENTRY_TEXT
    assert ('env, owner, repo, "pull", actor, title, number, opened=True)'
            in ENTRY_TEXT)
    assert "item.get(\"titleIfNew\", \"\"), number, public_owner=owner,\n            opened=True))" in forkbot
    assert "self.env, owner, repo, \"issue\", actor, title, number,\n                opened=True))" in runtime
    assert '"issue_opened"' in ENTRY_TEXT[
        ENTRY_TEXT.index("NOTIFICATION_EMAIL_KINDS = ("):
        ENTRY_TEXT.index("NOTIFICATION_EMAIL_DEFAULTS = {")]


def test_org_repo_maintainers_keeps_only_roles_that_triage():
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = [node for node in tree.body
            if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef))
            and node.name in ("_org_repo_maintainers", "team_permission_rank")]
    body.extend(
        node for node in tree.body
        if isinstance(node, ast.Assign) and {
            t.id for t in node.targets if isinstance(t, ast.Name)
        } & {"PING_OPENED_MAX_ORG_RECIPIENTS", "TEAM_PERMISSIONS",
             "NODE_NAME_RE", "MAX_NODE_NAME", "MAX_REPO_SEGMENT"})
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)
    queries = []

    async def d1_all(_env, sql, *args):
        queries.append((sql, args))
        return [
            {"name": "Owner1", "role": "owner"},
            {"name": "admin1", "role": "admin"},
            {"name": "keeper", "role": "maintainer"},
            {"name": "plain", "role": "member"},
            {"name": "watcher", "role": "auditor"},
            {"name": "visitor", "role": "guest"},
            {"name": "stale", "role": "retired-role"},
        ]

    async def ensure_schema(_env):
        return None

    import sys
    sys.path.insert(0, str(ROOT / "src"))
    import organization_rbac
    ns = {
        "re": re,
        "d1_all": d1_all,
        "ensure_schema": ensure_schema,
        "_org_rbac": organization_rbac,
        "clean_string": lambda value, limit=240: (
            value.strip()[:limit] if isinstance(value, str) else ""),
    }
    exec(compile(mod, str(ENTRY), "exec"), with_maintenance_off(ns))
    ns["valid_node_name"] = lambda name: bool(ns["NODE_NAME_RE"].match(name))
    found = asyncio.run(ns["_org_repo_maintainers"](object(), "Jett", "ForkMesh"))
    assert found == ["admin1", "keeper", "owner1"]
    # The org_repos alias index stores both halves folded.
    assert queries[0][1] == ("jett", "forkmesh")


def test_digest_html_escapes_untrusted_notification_text():
    ns = _load_notification_helpers()
    assert ns["_html_escape"]("<script>&\"x") == "&lt;script&gt;&amp;&quot;x"


if __name__ == "__main__":
    for test in (
        test_worker_exposes_first_class_notifications_route_and_schema,
        test_notification_kind_contract_covers_dashboard_events,
        test_notification_mentions_extract_node_handles_once_without_false_positives,
        test_worker_indexes_notifications_from_existing_event_sources,
        test_dashboard_notifications_are_wired_to_real_api_not_mock_data,
        test_worker_exposes_signed_thread_subscription_route_and_schema,
        test_worker_auto_subscribes_commenters_and_fans_out_to_followers,
        test_subscribed_notification_kind_exists,
        test_email_digest_bridge_is_wired_to_the_cron_and_verified_email,
        test_notification_email_preferences_are_editable_and_host_alerts_default_off,
        test_daily_general_chat_digest_counts_encrypted_messages_without_emailing_contents,
        test_qt_node_syncs_email_notification_preferences_over_heartbeat,
        test_thread_key_is_stable_and_scoped,
        test_pending_inbox_ping_names_the_repo_the_submitter_used,
        test_new_pull_request_ping_for_a_plain_node_repo,
        test_new_issue_ping_reaches_the_host_and_organization_maintainers,
        test_inbox_activity_on_an_existing_item_stays_a_host_only_inbox_ping,
        test_every_inbox_creation_path_marks_its_ping_opened,
        test_org_repo_maintainers_keeps_only_roles_that_triage,
        test_digest_html_escapes_untrusted_notification_text,
    ):
        test()
        print("PASS", test.__name__)


def _load_digest_sender(sent, saved):
    """send_notification_digests with its I/O collaborators faked.

    The delivery-mode branch decides pacing, subject shape, and how far the
    watermark advances, and none of that is visible to a source-text assertion —
    so this drives the real function over a fake notifications table.
    """
    want_funcs = {"send_notification_digests", "notification_delivery_mode",
                  "notification_email_preferences", "_apply_notification_delivery",
                  "_notification_queue", "_queue_instant_notification",
                  "_notification_email_recipient", "_notification_email_items",
                  "_deliver_instant_notifications", "process_notification_queue",
                  "_queue_batch_messages", "_queue_message_recipient",
                  "ping_pause_until", "pings_paused", "_apply_ping_pause",
                  "_skip_paused_notification_emails",
                  "notification_email_immediate",
                  "_notification_immediate_watermark",
                  "_send_notification_emails_singly",
                  "_deliver_immediate_notifications",
                  "_notification_pull_key", "_notification_pull_settled",
                  "_notification_email_stale", "_notification_email_send_key",
                  "_claim_notification_email", "_release_notification_emails",
                  "prune_notification_email_sends"}
    want_assigns = {
        "NOTIFICATION_EMAIL_KINDS", "NOTIFICATION_EMAIL_DEFAULTS",
        "NOTIFICATION_DELIVERY_MODES", "NOTIFICATION_DELIVERY_DEFAULT",
        "NOTIFICATION_DIGEST_INTERVAL_MS", "NOTIFICATION_DIGEST_MIN_AGE_MS",
        "NOTIFICATION_DIGEST_MAX_ITEMS", "NOTIFICATION_DIGEST_MAX_RECIPIENTS",
        "NOTIFICATION_INSTANT_MAX_RECIPIENTS", "NOTIFICATION_INSTANT_MAX_ITEMS",
        "NOTIFICATION_QUEUE_BINDING", "NOTIFICATION_QUEUE_MAX_RECIPIENTS",
        "NOTIFICATION_QUEUE_COALESCE_MS", "NOTIFICATION_QUEUE_MEMO_MAX",
        "_NOTIFICATION_QUEUE_MEMO", "MAX_NODE_NAME", "PING_PAUSE_MAX_MS",
        "NOTIFICATION_IMMEDIATE_KINDS", "NOTIFICATION_IMMEDIATE_SOURCES",
        "NOTIFICATION_IMMEDIATE_CONTEXTS", "NOTIFICATION_EMAIL_SKIP_SOURCES",
        "NOTIFICATION_IMMEDIATE_SCAN", "NOTIFICATION_IMMEDIATE_FRESH_MS",
        "NOTIFICATION_PULL_STATE_KINDS", "NOTIFICATION_PULL_STATE_MAX_READS",
        "NOTIFICATION_EMAIL_SEND_RETAIN_MS",
    }
    tree = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
    body = []
    for node in tree.body:
        if isinstance(node, ast.Assign):
            targets = {t.id for t in node.targets if isinstance(t, ast.Name)}
            if targets & want_assigns:
                body.append(node)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name in want_funcs:
                body.append(node)
    mod = ast.Module(body=body, type_ignores=[])
    ast.fix_missing_locations(mod)

    # One row per notification; the fakes below answer the two shapes of query
    # send_notification_digests issues against it.
    rows = []

    async def d1_all(_env, sql, *params):
        if "SELECT DISTINCT recipient_bi" in sql:
            cutoff, limit = params
            seen, out = set(), []
            for row in rows:
                if row["read_at"] or row["ts"] > cutoff:
                    continue
                if row["recipient_bi"] in seen:
                    continue
                seen.add(row["recipient_bi"])
                out.append({"recipient_bi": row["recipient_bi"]})
                if len(out) >= limit:
                    break
            return out
        recipient_bi, last, cutoff, limit = params
        hits = [row for row in rows
                if row["recipient_bi"] == recipient_bi and not row["read_at"]
                and last < row["ts"] <= cutoff]
        hits.sort(key=lambda row: row["ts"], reverse="DESC" in sql)
        return [{"dedupe_bi": row.get("dedupe_bi", ""), "ts": row["ts"],
                 "data": row["data"]} for row in hits[:limit]]

    email_sends = {}

    class _RunResult:
        def __init__(self, changes):
            self.meta = type("Meta", (), {"changes": changes})()

    async def d1_run(_env, sql, *params):
        if sql.startswith("INSERT OR IGNORE INTO notification_email_sends"):
            key, created_at = params
            if key in email_sends:
                return _RunResult(0)
            email_sends[key] = created_at
            return _RunResult(1)
        if sql.startswith("DELETE FROM notification_email_sends WHERE send_bi"):
            email_sends.pop(params[0], None)
            return _RunResult(1)
        if sql.startswith("DELETE FROM notification_email_sends WHERE created_at"):
            for key in [key for key, at in email_sends.items()
                        if at < params[0]]:
                email_sends.pop(key)
            return _RunResult(1)
        raise AssertionError(sql)

    queries = []

    async def d1_first(_env, sql, *params):
        queries.append(sql)
        if "FROM users" in sql and params[0] in accounts:
            return {"data": params[0]}
        return None

    accounts = {}

    async def decrypt_row(_env, data):
        return accounts.get(data)

    async def _send_email(_env, email, subject, text, html, email_kind=""):
        sent.append({"email": email, "subject": subject, "text": text,
                     "kind": email_kind})
        return True

    async def _save_account(_env, recipient_bi, rec):
        saved.append((recipient_bi, dict(rec)))

    async def blind_index(_env, name):
        return str(name) + "-bi"

    pull_status = {}
    pull_reads = []

    async def repo_gateway_json(_env, owner, repo, action):
        match = re.match(r"^blob\?path=\.forkmesh%2Fpulls%2F(\d+)%2Fpull\.md$",
                         action)
        pull_reads.append((owner, repo, action))
        status = pull_status.get((owner + "/" + repo, int(match.group(1))))
        if status is None:
            return None
        return {"ok": True, "content": "---\nstatus: %s\n---\n" % status}

    def parse_pull_frontmatter(text):
        fields = {}
        for line in (text or "").splitlines():
            key, sep, value = line.partition(":")
            if sep:
                fields[key.strip()] = value.strip()
        return fields

    ns = {
        "rows": rows,
        "pull_status": pull_status,
        "pull_reads": pull_reads,
        "_forkbot_repo_gateway_json": repo_gateway_json,
        "_forkbot_blob_text": lambda blob: (blob or {}).get("content", ""),
        "microjobs": type("Microjobs", (), {
            "parse_pull_frontmatter": staticmethod(parse_pull_frontmatter)}),
        "quote": quote,
        "accounts": accounts,
        "queries": queries,
        "d1_all": d1_all,
        "d1_first": d1_first,
        "d1_run": d1_run,
        "email_sends": email_sends,
        "decrypt_row": decrypt_row,
        "_send_email": _send_email,
        "_save_account": _save_account,
        "blind_index": blind_index,
        "json": json,
        "js_nullish": lambda value: value is None,
        "valid_node_name": lambda name: bool(re.match(r"^[a-z0-9-]{1,63}$", name or "")),
        "_cron_subrequest_charge": lambda count=1: None,
        "Date": type("Date", (), {"now": staticmethod(lambda: NOW)}),
        "_is_main_relay": lambda _env: True,
        "ensure_schema": _noop_async,
        "send_general_chat_digests": _noop_async,
        "_account_row": _no_owner_row,
        "_stamp_account_email": lambda rec, kind, ok, now=None: rec,
        # Rendering (and the identity lookups feeding it) is issue #652's
        # concern and is covered in test_notification_digest_detail.py; these
        # tests are about which mail goes out, to whom, and when.
        "_notification_digest_identity": _empty_identity,
        "_public_base_url": lambda _env: "https://forkmesh.com",
        "FORKMESH_SITE_URL": "https://forkmesh.com/",
        "_notification_digest_email": lambda node, items, identity=None, base_url="": (
            "ForkMesh: %d new notifications" % len(items), "text", "html"),
        "_notification_instant_email": lambda node, item, identity=None, base_url="": (
            "ForkMesh: " + item.get("title", ""), "text", "html"),
        "clean_string": lambda value, limit=240: (
            value.strip()[:limit] if isinstance(value, str) else ""),
    }
    exec(compile(mod, str(ENTRY), "exec"), with_maintenance_off(ns))
    return ns


NOW = 1_000_000_000


async def _noop_async(*_args, **_kwargs):
    return None


async def _no_owner_row(_env, _owner):
    return None, None


async def _empty_identity(_env, _items):
    return {"repos": {}, "avatars": {}}


def _verified_account(name, delivery=""):
    rec = {"name": name, "status": "active", "email": name + "@example.com",
           "email_verified": True}
    if delivery:
        rec["notification_delivery"] = delivery
    return rec


def _row(recipient_bi, ts, title, kind="mention", source="", meta=None):
    payload = {"kind": kind, "title": title, "ts": ts}
    if source:
        payload["source"] = source
    if meta:
        payload["meta"] = meta
    return {"recipient_bi": recipient_bi, "ts": ts, "read_at": 0,
            "dedupe_bi": recipient_bi + ":" + title,
            "data": "payload:" + title, "payload": payload}


def _add_rows(ns, *rows):
    for row in rows:
        ns["rows"].append(row)
        ns["accounts"][row["data"]] = row["payload"]


def test_instant_delivery_sends_one_email_per_ping_with_no_quarantine_wait():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    # Opted into instant, digested five minutes ago — neither the hourly
    # interval nor the three-minute quarantine may hold these back.
    rec = _verified_account("ada", delivery="instant")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["ada-bi"] = rec
    fresh = NOW - 30 * 1000
    for index, ts in enumerate((fresh - 2000, fresh - 1000, fresh)):
        row = _row("ada-bi", ts, "Ping %d" % index)
        ns["rows"].append(row)
        ns["accounts"][row["data"]] = row["payload"]
    asyncio.run(ns["send_notification_digests"](object(),
                                                include_general_chat=False))

    assert [item["subject"] for item in sent] == [
        "ForkMesh: Ping 0", "ForkMesh: Ping 1", "ForkMesh: Ping 2"]
    assert all(item["kind"] == "notifications" for item in sent)
    # Watermark lands on the newest ping that actually left.
    assert saved[-1][1]["last_digest_ts"] == fresh


def test_digest_delivery_still_rolls_up_and_honours_the_quarantine():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("grace")
    ns["accounts"]["grace-bi"] = rec
    old = NOW - 10 * 60 * 1000
    for index, ts in enumerate((old, old + 1000)):
        row = _row("grace-bi", ts, "Ping %d" % index)
        ns["rows"].append(row)
        ns["accounts"][row["data"]] = row["payload"]
    # A ping younger than the quarantine stays out of this rollup.
    fresh = _row("grace-bi", NOW - 30 * 1000, "Too fresh")
    ns["rows"].append(fresh)
    ns["accounts"][fresh["data"]] = fresh["payload"]

    asyncio.run(ns["send_notification_digests"](object(),
                                                include_general_chat=False))

    assert [item["subject"] for item in sent] == [
        "ForkMesh: 2 new notifications"]
    assert saved[-1][1]["last_digest_ts"] == old + 1000


def test_ios_build_ping_is_not_emailed_again_after_the_build_email():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    ns["accounts"]["ada-bi"] = _verified_account("ada", delivery="instant")
    _add_rows(
        ns,
        _row("ada-bi", NOW - 20 * 1000, "iPhone build 1.4.0 (21) is ready",
             kind="release_published", source="ios_build"),
        _row("ada-bi", NOW - 10 * 1000, "Release v1.4.0",
             kind="release_published", source="release"),
    )

    asyncio.run(ns["send_notification_digests"](object(),
                                                include_general_chat=False))

    assert [item["subject"] for item in sent] == ["ForkMesh: Release v1.4.0"]


def test_delivery_mode_defaults_to_digest_and_rejects_unknown_values():
    ns = _load_digest_sender([], [])
    mode = ns["notification_delivery_mode"]
    apply_mode = ns["_apply_notification_delivery"]
    assert mode({}) == "digest"
    assert mode({"notification_delivery": "instant"}) == "instant"
    assert mode({"notification_delivery": "hourly"}) == "digest"
    rec = {}
    assert apply_mode(rec, "instant") is True
    assert rec["notification_delivery"] == "instant"
    # Back to the default stores absence, matching email_notifications.
    assert apply_mode(rec, "digest") is True
    assert "notification_delivery" not in rec
    assert apply_mode(rec, "nonsense") is False


def test_instant_burst_larger_than_one_slice_drains_without_skipping_pings():
    """A burst over NOTIFICATION_INSTANT_MAX_ITEMS must not lose its tail.

    Instant reads oldest-first precisely so the watermark stays contiguous: the
    slice that goes out this pass is the oldest one, and the remainder is still
    ahead of the watermark for the next pass. Reading newest-first would jump
    the watermark past the older pings and drop them for good.
    """
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    ns["accounts"]["ada-bi"] = _verified_account("ada", delivery="instant")
    base = NOW - 60 * 1000
    for index in range(5):
        row = _row("ada-bi", base + index, "Ping %d" % index)
        ns["rows"].append(row)
        ns["accounts"][row["data"]] = row["payload"]

    asyncio.run(ns["send_notification_digests"](object(),
                                                include_general_chat=False))
    assert [item["subject"] for item in sent] == [
        "ForkMesh: Ping 0", "ForkMesh: Ping 1", "ForkMesh: Ping 2"]

    # The next pass picks up exactly where the watermark stopped.
    asyncio.run(ns["send_notification_digests"](object(),
                                                include_general_chat=False))
    assert [item["subject"] for item in sent] == [
        "ForkMesh: Ping 0", "ForkMesh: Ping 1", "ForkMesh: Ping 2",
        "ForkMesh: Ping 3", "ForkMesh: Ping 4"]
    assert saved[-1][1]["last_digest_ts"] == base + 4


def _paused_account(name, until, delivery=""):
    rec = _verified_account(name, delivery=delivery)
    rec["notification_pause_until"] = until
    return rec


def test_a_live_pause_mails_nothing_and_still_moves_the_watermark():
    """Pausing must not turn into a flood the moment it lifts.

    The pings stay unread (the inbox and the bell keep them) but the watermark
    walks past them, so the first pass after the deadline has nothing old to
    deliver — which is the whole point of having asked for quiet.
    """
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _paused_account("ada", NOW + 60 * 60 * 1000, delivery="instant")
    ns["accounts"]["ada-bi"] = rec
    old = NOW - 10 * 60 * 1000
    for index, ts in enumerate((old, old + 1000, old + 2000)):
        row = _row("ada-bi", ts, "Ping %d" % index)
        ns["rows"].append(row)
        ns["accounts"][row["data"]] = row["payload"]

    asyncio.run(ns["send_notification_digests"](object(),
                                                include_general_chat=False))

    assert sent == []
    assert saved[-1][1]["last_digest_ts"] == old + 2000
    # Nothing was marked read: a pause mutes the delivery, not the inbox.
    assert all(row["read_at"] == 0 for row in ns["rows"])


def test_pings_mail_again_once_the_pause_deadline_has_passed():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    # Deadline a minute in the past — a pause that ended, which reads exactly
    # like never having set one.
    rec = _paused_account("ada", NOW - 60 * 1000, delivery="instant")
    ns["accounts"]["ada-bi"] = rec
    row = _row("ada-bi", NOW - 30 * 1000, "After the pause")
    ns["rows"].append(row)
    ns["accounts"][row["data"]] = row["payload"]

    asyncio.run(ns["send_notification_digests"](object(),
                                                include_general_chat=False))

    assert [item["subject"] for item in sent] == ["ForkMesh: After the pause"]


def test_a_paused_account_resolves_to_no_email_address_at_all():
    """The single chokepoint every mail rail gates on, including the queue."""
    ns = _load_digest_sender([], [])
    ns["accounts"]["ada-bi"] = _paused_account("ada", NOW + 60 * 60 * 1000)
    _, email_rec, email = asyncio.run(
        ns["_notification_email_recipient"](object(), "ada-bi"))
    assert email == ""
    assert ns["pings_paused"](email_rec, NOW) is True


def test_pause_deadline_is_clamped_stored_and_cleared_like_a_snooze():
    ns = _load_digest_sender([], [])
    read = ns["ping_pause_until"]
    apply_pause = ns["_apply_ping_pause"]
    assert read({}) == 0
    # A deadline already in the past is not a pause, it is a pause that ended.
    assert read({"notification_pause_until": NOW - 1}) == NOW - 1
    assert ns["pings_paused"]({"notification_pause_until": NOW - 1}, NOW) is False

    rec = {}
    assert apply_pause(rec, NOW + 60 * 60 * 1000) is True
    assert rec["notification_pause_until"] == NOW + 60 * 60 * 1000
    # Resuming stores absence, matching email_notifications and delivery mode.
    assert apply_pause(rec, 0) is True
    assert "notification_pause_until" not in rec
    assert apply_pause(rec, 0) is False
    # Anything past the cap is clamped rather than rejected, so a mistyped
    # custom deadline still ends up as a pause somebody wakes up from.
    assert apply_pause(rec, NOW + 10 * ns["PING_PAUSE_MAX_MS"]) is True
    assert rec["notification_pause_until"] == NOW + ns["PING_PAUSE_MAX_MS"]
    assert apply_pause(rec, "not-a-number") is False


def test_pause_notifications_is_wired_through_worker_dashboard_and_desktop():
    settings_page = assembled_dashboard_page("settings")
    dashboard_js = _read(PUBLIC / "dashboard.js")

    for marker in (
        "def ping_pause_until",
        "def pings_paused",
        "def _apply_ping_pause",
        "async def _skip_paused_notification_emails",
        "PING_PAUSE_MAX_MS",
        '"notificationPauseUntil" in data',
        '"notificationPauseUntil": ping_pause_until(rec)',
        '"pauseUntil" in data',
    ):
        assert marker in ENTRY_TEXT

    # Both write rails accept a pause: the browser's session-authed settings
    # save and the desktop's signed heartbeat.
    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("# --- Users vs nodes")
    ]
    heartbeat_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_heartbeat"):
        ENTRY_TEXT.index("async def _mirroring_owners")
    ]
    assert '"notificationPauseUntil" in data' in profile_body
    assert '"notificationPauseUntil" in data' in heartbeat_body

    # Dashboard: the presets the request named, offered twice on the settings
    # page — once in the header bell every page carries, once in the Pings
    # section — plus the custom deadline field beside each.
    assert settings_page.count('data-ping-pause-preset="3600000"') == 2   # 1 hour
    assert settings_page.count('data-ping-pause-preset="86400000"') == 2  # 1 day
    assert settings_page.count('data-ping-pause-preset="604800000"') == 2 # 1 week
    assert settings_page.count('data-ping-pause-custom type="datetime-local"') == 2
    # The bell is reachable from every page, so its quick pause is too.
    assert 'data-ping-pause-preset="3600000"' in assembled_dashboard_page("home")
    assert "data-ping-pause-resume" in settings_page
    assert "renderPingPause" in dashboard_js
    assert "savePingPause" in dashboard_js
    assert "Pause notifications" in settings_page
    assert "Notifications paused until" in dashboard_js
    assert "Pause pings" not in settings_page

    # Desktop offers the same four choices and mutes both card rails.
    qt_src = ROOT.parent / "desktop" / "src"
    header = (qt_src / "MainWindowInternal.h").read_text(encoding="utf-8")
    actions = (qt_src / "MainWindowActions.cpp").read_text(encoding="utf-8")
    setup = (qt_src / "MainWindowSetup.cpp").read_text(encoding="utf-8")
    settings = (qt_src / "MainWindowSettings.cpp").read_text(encoding="utf-8")
    agents = "".join(
        (qt_src / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_AGENT_SOURCES
    )
    tray_sources = [
        (qt_src / name).read_text(encoding="utf-8")
        for name in ("MainWindowCoves.cpp", "MainWindowIssues.cpp",
                     "MainWindowPulls.cpp", "MainWindowSetup.cpp")
    ] + [
        "".join(
            (qt_src / name).read_text(encoding="utf-8")
            for name in MAIN_WINDOW_REPOS_SOURCES
        )
    ]
    assert "kPingPauseUntilSetting" in header
    assert "bool pingsPaused();" in header
    internal = (qt_src / "MainWindowInternal1.cpp").read_text(encoding="utf-8")
    assert "bool pingsPaused()\n{\n    return pingPauseUntilMs() > 0" in internal
    for choice in ("For 1 hour", "For 1 day", "Until next week", "Custom"):
        assert choice in actions
    assert "Resume notifications" in actions
    assert "Pause notifications" in settings
    # Native notification-centre delivery is opt-in, and every native rail
    # shares that gate with the pause. In addition, unsolicited Pings
    # (especially an agent completion) route through a dedicated helper that
    # files the Ping and never raises a card or an inline notice.
    assert "kNativeSystemNotificationsSetting" in header
    assert "nativeSystemNotificationsEnabled()" in header
    assert "kSystemAlertSetting" in header
    assert "QSettings().value(kSystemAlertSetting, false)" in internal
    assert "pingsPaused() || !nativeSystemNotificationsEnabled()" in actions
    assert "void MainWindow::flashPingMessage" in settings
    assert "NoticeRoute::Ping" in settings
    assert "if (!pingsPaused() && forkmesh::pings::pingKindEnabled(kind))" in settings
    assert "!nativeSystemNotificationsEnabled()" in settings
    assert "flashPingMessage(summary" in agents
    assert "if (!pingsPaused())\n        flashCelebrationBorder();" in agents
    # The older native-tray callers must not bypass the pause. They all now go
    # through the one pause-aware delivery helper instead of calling Qt direct.
    assert "void MainWindow::showPingTrayMessage" in actions
    assert all("showPingTrayMessage(" in source for source in tray_sources)
    assert all("m_trayIcon->showMessage(" not in source for source in tray_sources)
    assert 'body.insert(QStringLiteral("notificationPauseUntil")' in setup
    assert "kPingPausePushPendingSetting" in setup
    assert "deliveredCurrentPause" in setup


def test_instant_delivery_option_is_wired_through_worker_dashboard_and_desktop():
    dashboard = assembled_dashboard()
    dashboard_js = _read(PUBLIC / "dashboard.js")
    login_js = _read(PUBLIC / "login.js")

    for marker in (
        "def notification_delivery_mode",
        "def _apply_notification_delivery",
        'NOTIFICATION_DELIVERY_MODES = ("digest", "instant")',
        'NOTIFICATION_DELIVERY_DEFAULT = "digest"',
        "NOTIFICATION_INSTANT_MAX_RECIPIENTS",
        "NOTIFICATION_INSTANT_MAX_ITEMS",
        "def _notification_instant_email",
        '"notificationDelivery" in data',
        '"notificationDelivery": notification_delivery_mode(rec)',
    ):
        assert marker in ENTRY_TEXT

    profile_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_profile"):
        ENTRY_TEXT.index("# --- Users vs nodes")
    ]
    heartbeat_body = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _account_heartbeat"):
        ENTRY_TEXT.index("async def _mirroring_owners")
    ]
    assert '"notificationDelivery" in data' in profile_body
    assert '"notificationDelivery" in data' in heartbeat_body

    # Dashboard: a control to pick with, and the field carried on the session
    # so the control renders the account's real choice.
    assert 'data-notification-delivery-mode="digest"' in dashboard
    assert 'data-notification-delivery-mode="instant"' in dashboard
    assert "collectNotificationDelivery" in dashboard_js
    assert "renderNotificationDelivery" in dashboard_js
    assert "notificationDelivery: collectNotificationDelivery()" in dashboard_js
    assert "notificationDelivery" in login_js

    # Desktop mirrors the same choice over the signed heartbeat.
    qt_src = ROOT.parent / "desktop" / "src"
    header = (qt_src / "MainWindowInternal.h").read_text(encoding="utf-8")
    setup = (qt_src / "MainWindowSetup.cpp").read_text(encoding="utf-8")
    settings = (qt_src / "MainWindowSettings.cpp").read_text(encoding="utf-8")
    assert "kEmailNotifyDeliverySetting" in header
    assert "QString MainWindow::emailNotificationDelivery() const" in setup
    assert 'body.insert(QStringLiteral("notificationDelivery")' in setup
    assert "Email pacing: instant, one per ping" in settings


class _FakeQueue:
    """Stand-in for the NOTIFICATION_QUEUE binding."""

    def __init__(self):
        self.sent = []

    async def send(self, body):
        self.sent.append(body)


class _FakeEnv:
    """Env carrying (or not carrying) the queue binding."""

    def __init__(self, queue=None):
        if queue is not None:
            self.NOTIFICATION_QUEUE = queue


class _JsArray:
    """Minimal stand-in for the JS array proxy batch.messages arrives as."""

    def __init__(self, items):
        self._items = list(items)
        self.length = len(items)

    def __getitem__(self, index):
        return self._items[index]


class _FakeMessage:
    def __init__(self, body):
        self.body = body


def _fake_batch(recipients, js_array=False):
    messages = [_FakeMessage(json.dumps({"recipient": name, "ts": NOW}))
                for name in recipients]
    return type("Batch", (), {
        "messages": _JsArray(messages) if js_array else messages})()


def test_queue_doorbell_names_the_account_and_carries_no_ping_content():
    ns = _load_digest_sender([], [])
    queue = _FakeQueue()
    ns["_NOTIFICATION_QUEUE_MEMO"].clear()
    sent = asyncio.run(ns["_queue_instant_notification"](
        _FakeEnv(queue), "ada", NOW))
    assert sent is True
    # A JSON string, not a JS object, and it names the account only — the ping
    # payload stays encrypted in D1 for the consumer to re-read.
    assert isinstance(queue.sent[0], str)
    body = json.loads(queue.sent[0])
    assert body == {"recipient": "ada", "ts": NOW}
    assert set(body) == {"recipient", "ts"}


def test_queue_doorbell_is_a_no_op_without_the_binding():
    # Queues are a paid-plan product; a deployment without one must fall back
    # to the cron rail rather than raising on every notification write.
    ns = _load_digest_sender([], [])
    ns["_NOTIFICATION_QUEUE_MEMO"].clear()
    assert asyncio.run(ns["_queue_instant_notification"](
        _FakeEnv(), "ada", NOW)) is False


def test_queue_doorbell_coalesces_repeats_for_one_recipient():
    # The cron alert path loops admins x alerts and would otherwise ring once
    # per pair against a bounded phase subrequest budget.
    ns = _load_digest_sender([], [])
    queue = _FakeQueue()
    ns["_NOTIFICATION_QUEUE_MEMO"].clear()
    env = _FakeEnv(queue)
    assert asyncio.run(ns["_queue_instant_notification"](env, "ada", NOW))
    assert not asyncio.run(ns["_queue_instant_notification"](env, "ada", NOW + 1))
    # A different recipient in the same window still gets its own doorbell.
    assert asyncio.run(ns["_queue_instant_notification"](env, "grace", NOW))
    # And the same recipient again once the window has passed.
    later = NOW + ns["NOTIFICATION_QUEUE_COALESCE_MS"] + 1
    assert asyncio.run(ns["_queue_instant_notification"](env, "ada", later))
    assert [json.loads(b)["recipient"] for b in queue.sent] == [
        "ada", "grace", "ada"]


def test_queue_consumer_delivers_instant_mail_for_the_named_recipient():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    ns["accounts"]["ada-bi"] = _verified_account("ada", delivery="instant")
    fresh = NOW - 5 * 1000  # far inside the digest quarantine
    row = _row("ada-bi", fresh, "Ping now")
    ns["rows"].append(row)
    ns["accounts"][row["data"]] = row["payload"]

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["ada"])))

    assert [item["subject"] for item in sent] == ["ForkMesh: Ping now"]
    assert saved[-1][1]["last_digest_ts"] == fresh


def test_queue_consumer_leaves_digest_recipients_to_the_rollup():
    # The producer doorbells every ping because deciding on the write path
    # would cost a decrypt per notification; the consumer is where a digest
    # recipient gets filtered back out.
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    ns["accounts"]["grace-bi"] = _verified_account("grace")
    row = _row("grace-bi", NOW - 5 * 1000, "Ping now")
    ns["rows"].append(row)
    ns["accounts"][row["data"]] = row["payload"]

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["grace"])))

    assert sent == []
    assert all("last_digest_ts" not in rec for _bi, rec in saved)


def test_issue_pull_and_discussion_pings_are_the_immediate_ones():
    ns = _load_digest_sender([], [])
    immediate = ns["notification_email_immediate"]
    assert immediate({"kind": "issue_opened"})
    assert immediate({"kind": "pull_submitted"})
    assert immediate({"kind": "issue_assigned", "source": "issue_assigned"})
    for source in ("issue", "pull", "discussion"):
        assert immediate({"kind": "subscribed", "source": source})
        assert immediate({"kind": "mention", "source": source})
        assert immediate({"kind": "pending_inbox", "source": source})
    assert not immediate({"kind": "mention"})
    assert not immediate({"kind": "release_published", "source": "release"})
    assert not immediate({"kind": "tip_sent"})
    assert not immediate(None)


def test_task_check_in_reminders_are_immediate():
    ns = _load_digest_sender([], [])
    immediate = ns["notification_email_immediate"]
    assert immediate({"kind": "mention", "source": "task-1",
                      "meta": {"action": "reminded", "context": "reminder"}})
    assert not immediate({"kind": "mention", "source": "task-1",
                          "meta": {"action": "mentioned",
                                   "context": "comment"}})
    assert not immediate({"kind": "mention", "meta": "reminder"})


def test_queue_consumer_mails_a_task_reminder_now():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("grace")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["grace-bi"] = rec
    fresh = NOW - 5 * 1000
    _add_rows(
        ns,
        _row("grace-bi", fresh - 1, "Task comment", source="task-1",
             meta={"action": "mentioned", "context": "comment"}),
        _row("grace-bi", fresh, "Check-in please", source="task-1",
             meta={"action": "reminded", "context": "reminder"}),
    )

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["grace"])))

    assert [item["subject"] for item in sent] == [
        "ForkMesh: Check-in please"]
    assert rec["last_immediate_ts"] == fresh


def test_queue_consumer_mails_digest_recipients_issue_pull_and_discussion_pings_now():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("grace")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["grace-bi"] = rec
    fresh = NOW - 5 * 1000
    _add_rows(
        ns,
        _row("grace-bi", fresh - 3, "New issue", kind="issue_opened",
             source="issue"),
        _row("grace-bi", fresh - 2, "A tip", kind="tip_sent", source="tip"),
        _row("grace-bi", fresh - 1, "PR comment", kind="subscribed",
             source="pull"),
        _row("grace-bi", fresh, "Discussion reply", kind="pending_inbox",
             source="discussion"),
    )

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["grace"])))

    assert [item["subject"] for item in sent] == [
        "ForkMesh: New issue", "ForkMesh: PR comment",
        "ForkMesh: Discussion reply"]
    assert rec["last_immediate_ts"] == fresh
    assert rec["last_digest_ts"] == NOW - 5 * 60 * 1000


def test_digest_leaves_out_what_the_immediate_rail_already_mailed():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("grace")
    ns["accounts"]["grace-bi"] = rec
    old = NOW - 10 * 60 * 1000
    _add_rows(
        ns,
        _row("grace-bi", old, "New pull request", kind="pull_submitted",
             source="pull"),
        _row("grace-bi", old + 1, "A tip", kind="tip_sent", source="tip"),
        _row("grace-bi", old + 2, "Release", kind="release_published",
             source="release"),
    )

    asyncio.run(ns["send_notification_digests"](
        _FakeEnv(_FakeQueue()), include_general_chat=False))

    assert [item["subject"] for item in sent] == [
        "ForkMesh: New pull request", "ForkMesh: 2 new notifications"]
    assert rec["last_digest_ts"] == old + 2
    assert rec["last_immediate_ts"] == old + 2


def test_cron_mails_a_fresh_issue_ping_at_once_when_no_queue_is_configured():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("grace")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["grace-bi"] = rec
    _add_rows(
        ns,
        _row("grace-bi", NOW - 30 * 1000, "New issue", kind="issue_opened",
             source="issue"),
        _row("grace-bi", NOW - 20 * 1000, "A tip", kind="tip_sent",
             source="tip"),
    )

    asyncio.run(ns["send_notification_digests"](
        _FakeEnv(), include_general_chat=False))

    assert [item["subject"] for item in sent] == ["ForkMesh: New issue"]


def test_immediate_rail_retries_from_the_first_ping_that_failed_to_send():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("grace")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["grace-bi"] = rec
    base = NOW - 10 * 1000
    _add_rows(
        ns,
        _row("grace-bi", base, "Issue one", kind="issue_opened",
             source="issue"),
        _row("grace-bi", base + 1, "A tip", kind="tip_sent", source="tip"),
        _row("grace-bi", base + 2, "Issue two", kind="issue_opened",
             source="issue"),
    )
    outcomes = iter((True, False))

    async def flaky_send(_env, email, subject, text, html, email_kind=""):
        ok = next(outcomes, True)
        if ok:
            sent.append({"subject": subject})
        return ok

    ns["_send_email"] = flaky_send
    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["grace"])))
    assert [item["subject"] for item in sent] == ["ForkMesh: Issue one"]
    assert rec["last_immediate_ts"] == base + 1

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["grace"])))
    assert [item["subject"] for item in sent] == [
        "ForkMesh: Issue one", "ForkMesh: Issue two"]
    assert rec["last_immediate_ts"] == base + 2


def test_switching_to_instant_does_not_resend_immediate_pings():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("grace", delivery="instant")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    rec["last_immediate_ts"] = NOW - 20 * 1000
    ns["accounts"]["grace-bi"] = rec
    _add_rows(
        ns,
        _row("grace-bi", NOW - 30 * 1000, "Already mailed",
             kind="issue_opened", source="issue"),
        _row("grace-bi", NOW - 25 * 1000, "A tip", kind="tip_sent",
             source="tip"),
    )

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["grace"])))

    assert [item["subject"] for item in sent] == ["ForkMesh: A tip"]
    assert rec["last_digest_ts"] == NOW - 25 * 1000
    assert rec["last_immediate_ts"] == NOW - 20 * 1000


def _pull_row(recipient_bi, ts, title, number, kind="pull_submitted",
              repo="forkmesh/forkmesh"):
    row = _row(recipient_bi, ts, title, kind=kind, source="pull")
    row["payload"]["repo"] = repo
    row["payload"]["meta"] = {"number": number, "source": "pull"}
    return row


def test_immediate_rail_does_not_mail_a_pull_request_that_was_already_merged():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("jett")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["jett-bi"] = rec
    ns["pull_status"].update({
        ("forkmesh/forkmesh", 41): "merged",
        ("forkmesh/forkmesh", 42): "closed",
        ("forkmesh/forkmesh", 43): "open",
    })
    base = NOW - 60 * 1000
    _add_rows(
        ns,
        _pull_row("jett-bi", base, "Merged PR", 41),
        _pull_row("jett-bi", base + 1, "Merged PR activity", 41,
                  kind="pending_inbox"),
        _pull_row("jett-bi", base + 2, "Closed PR", 42),
        _pull_row("jett-bi", base + 3, "Open PR", 43),
    )

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["jett"])))

    assert [item["subject"] for item in sent] == ["ForkMesh: Open PR"]
    assert rec["last_immediate_ts"] == base + 3
    assert len(ns["pull_reads"]) == 3


def test_unreadable_pull_state_still_mails_the_pull_request():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("jett")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["jett-bi"] = rec
    _add_rows(ns, _pull_row("jett-bi", NOW - 60 * 1000, "Private PR", 7))

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["jett"])))

    assert [item["subject"] for item in sent] == ["ForkMesh: Private PR"]


def test_immediate_backlog_older_than_the_fresh_window_is_not_mailed():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("jett")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["jett-bi"] = rec
    stale = NOW - ns["NOTIFICATION_IMMEDIATE_FRESH_MS"] - 1000
    _add_rows(
        ns,
        _pull_row("jett-bi", stale, "Old PR", 10),
        _pull_row("jett-bi", stale + 1, "Old PR 2", 11),
        _row("jett-bi", stale + 2, "Old issue", kind="issue_opened",
             source="issue"),
        _row("jett-bi", NOW - 30 * 1000, "New issue", kind="issue_opened",
             source="issue"),
    )

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["jett"])))

    assert [item["subject"] for item in sent] == ["ForkMesh: New issue"]
    assert rec["last_immediate_ts"] == NOW - 30 * 1000
    assert ns["pull_reads"] == []


def test_pull_state_reads_are_bounded_and_the_rest_waits_for_the_next_pass():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("jett")
    rec["last_digest_ts"] = NOW - 5 * 60 * 1000
    ns["accounts"]["jett-bi"] = rec
    limit = ns["NOTIFICATION_PULL_STATE_MAX_READS"]
    base = NOW - 60 * 1000
    for number in range(1, limit + 3):
        ns["pull_status"][("forkmesh/forkmesh", number)] = "merged"
        _add_rows(ns, _pull_row("jett-bi", base + number, "PR %d" % number,
                                number))

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["jett"])))
    assert sent == []
    assert len(ns["pull_reads"]) == limit
    assert rec["last_immediate_ts"] == base + limit

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["jett"])))
    assert sent == []
    assert rec["last_immediate_ts"] == base + limit + 2


def test_instant_delivery_skips_merged_pull_requests_and_stale_backlog():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    rec = _verified_account("jett", delivery="instant")
    rec["last_digest_ts"] = NOW - 2 * ns["NOTIFICATION_IMMEDIATE_FRESH_MS"]
    ns["accounts"]["jett-bi"] = rec
    ns["pull_status"][("forkmesh/forkmesh", 5)] = "merged"
    stale = NOW - ns["NOTIFICATION_IMMEDIATE_FRESH_MS"] - 1000
    _add_rows(
        ns,
        _row("jett-bi", stale, "Old issue", kind="issue_opened",
             source="issue"),
        _pull_row("jett-bi", NOW - 20 * 1000, "Merged PR", 5),
        _row("jett-bi", NOW - 10 * 1000, "A tip", kind="tip_sent",
             source="tip"),
    )

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["jett"])))

    assert [item["subject"] for item in sent] == ["ForkMesh: A tip"]
    assert rec["last_digest_ts"] == NOW - 10 * 1000


def test_queue_consumer_collapses_repeat_recipients_in_one_batch():
    """A thread's fan-out puts the same account in a batch more than once.

    The mail alone cannot show this: a repeat pass reads from the watermark the
    first one advanced and finds nothing to send. What dedupe actually saves is
    the account lookup and decrypt per repeat, so that is what is asserted.
    """
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    ns["accounts"]["ada-bi"] = _verified_account("ada", delivery="instant")
    row = _row("ada-bi", NOW - 5 * 1000, "Only once")
    ns["rows"].append(row)
    ns["accounts"][row["data"]] = row["payload"]

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["ada", "ada", "ada"])))

    assert [item["subject"] for item in sent] == ["ForkMesh: Only once"]
    account_lookups = [sql for sql in ns["queries"] if "FROM users" in sql]
    assert len(account_lookups) == 1


def test_queue_consumer_reads_the_js_array_proxy_shape():
    # batch.messages arrives as a JS array proxy, which is indexable and has
    # .length but is not a Python sequence.
    sent, _saved = [], []
    ns = _load_digest_sender(sent, [])
    ns["accounts"]["ada-bi"] = _verified_account("ada", delivery="instant")
    row = _row("ada-bi", NOW - 5 * 1000, "Via proxy")
    ns["rows"].append(row)
    ns["accounts"][row["data"]] = row["payload"]

    asyncio.run(ns["process_notification_queue"](
        _FakeEnv(_FakeQueue()), _fake_batch(["ada"], js_array=True)))

    assert [item["subject"] for item in sent] == ["ForkMesh: Via proxy"]


def test_queue_consumer_ignores_unparseable_or_unnamed_messages():
    # A malformed body must be dropped, not turned into a lookup on a junk
    # account name — so this checks the consumer did no work at all, rather
    # than only that it did not raise.
    sent = []
    ns = _load_digest_sender(sent, [])
    batch = type("Batch", (), {"messages": [
        _FakeMessage("not json"),
        _FakeMessage(json.dumps(["not", "a", "dict"])),
        _FakeMessage(json.dumps({"recipient": "Not A Name!"})),
        _FakeMessage(json.dumps({})),
        _FakeMessage(None),
    ]})()
    asyncio.run(ns["process_notification_queue"](_FakeEnv(_FakeQueue()), batch))
    assert sent == []
    assert ns["queries"] == []


def test_cron_defers_the_fresh_window_to_the_queue_when_one_is_configured():
    """With a queue, the cron must not race the consumer over a fresh ping.

    Its instant cutoff stays at the quarantine, so it only picks up the
    backlog a failed or undelivered queue message left behind.
    """
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    ns["accounts"]["ada-bi"] = _verified_account("ada", delivery="instant")
    fresh = _row("ada-bi", NOW - 30 * 1000, "Queue owns this")
    stale = _row("ada-bi", NOW - 10 * 60 * 1000, "Queue missed this")
    for row in (stale, fresh):
        ns["rows"].append(row)
        ns["accounts"][row["data"]] = row["payload"]

    asyncio.run(ns["send_notification_digests"](
        _FakeEnv(_FakeQueue()), include_general_chat=False))

    # Only the ping already past the quarantine — the fresh one is the
    # consumer's to deliver.
    assert [item["subject"] for item in sent] == [
        "ForkMesh: Queue missed this"]


def test_cron_owns_the_whole_instant_rail_when_no_queue_is_configured():
    sent, saved = [], []
    ns = _load_digest_sender(sent, saved)
    ns["accounts"]["ada-bi"] = _verified_account("ada", delivery="instant")
    fresh = _row("ada-bi", NOW - 30 * 1000, "No queue, cron sends")
    ns["rows"].append(fresh)
    ns["accounts"][fresh["data"]] = fresh["payload"]

    asyncio.run(ns["send_notification_digests"](
        _FakeEnv(), include_general_chat=False))

    assert [item["subject"] for item in sent] == [
        "ForkMesh: No queue, cron sends"]


def test_instant_queue_is_declared_in_the_worker_manifest_and_created_on_deploy():
    """The rail only exists if the manifest declares it and deploy provisions it.

    wrangler refuses to publish a Worker naming a queue the account does not
    have, so the deploy has to create it first or every deploy breaks.
    """
    wrangler = (ROOT / "wrangler.toml").read_text(encoding="utf-8")
    deploy = (ROOT / "deploy.sh").read_text(encoding="utf-8")

    import tomllib
    manifest = tomllib.loads(wrangler)
    producers = manifest["queues"]["producers"]
    consumers = manifest["queues"]["consumers"]
    assert any(p["binding"] == "NOTIFICATION_QUEUE"
               and p["queue"] == "forkmesh-notification-email"
               for p in producers)
    consumer = next(c for c in consumers
                    if c["queue"] == "forkmesh-notification-email")
    # The batch timeout is the latency floor for "instant" — a large value
    # would quietly turn this rail back into a digest.
    assert consumer["max_batch_timeout"] <= 2
    # Producer and consumer must name the same queue as the code.
    assert 'NOTIFICATION_QUEUE_BINDING = "NOTIFICATION_QUEUE"' in ENTRY_TEXT
    assert ('NOTIFICATION_QUEUE_NAME = "forkmesh-notification-email"'
            in ENTRY_TEXT)

    assert "ensure_notification_queue" in deploy
    assert "queues create" in deploy
    # Creation must be idempotent, or the second deploy fails.
    assert "queues list" in deploy
    assert "already exists" in deploy


def test_instant_queue_rail_is_wired_end_to_end_in_the_worker():
    for marker in (
        "async def queue(self, batch, env, ctx):",
        "async def process_notification_queue",
        "async def _queue_instant_notification",
        "def _notification_queue",
        "await _queue_instant_notification(env, recipient, now)",
        # The cron keeps its own instant rail as the backstop.
        "await _deliver_instant_notifications(",
    ):
        assert marker in ENTRY_TEXT
    # The doorbell must stay payload-free: the queue never carries ping text.
    producer = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _queue_instant_notification"):
        ENTRY_TEXT.index("async def _notification_email_recipient")
    ]
    assert '"recipient": recipient' in producer
    for leaked in ('"title"', '"body"', '"payload"', "encrypt_row"):
        assert leaked not in producer


def test_one_pings_settings_page_covers_every_ping_switch_on_the_web():
    # Issue #347: the web settings page is the single home for every ping
    # setting — per-kind on/off, per-kind email, pause, quiet hours, email
    # pacing and (administrators only) the /status monitor alert matrix that
    # used to be the admin console's "Alert delivery" page.
    settings_page = assembled_dashboard_page("settings")
    home_page = assembled_dashboard_page("home")
    dashboard_js = _read(PUBLIC / "dashboard.js")
    section = settings_page[
        settings_page.index('id="settings-notifications"'):
        settings_page.index('id="settings-account"')
    ]

    # Still the "Pings" tab, with one card per concern.
    assert 'data-settings-section-link="notifications"' in settings_page
    assert ">Pings</h2>" in section
    for marker in (
        "data-ping-pause-form",
        'id="ping-quiet-hours"',
        'id="ping-kinds"',
        "data-ping-kind-list",
        'id="ping-email"',
        "data-ping-email-master",
        'data-notification-delivery-mode="digest"',
        'data-notification-pref="general_chat"',
        'id="admin-alerts"',
        "data-ping-admin-alert-rows",
        "data-ping-settings-status",
    ):
        assert marker in section, marker
    # Auto-save replaced the password-gated save button.
    assert 'type="password"' not in section
    assert "data-notification-preferences-save" not in section

    # Ping types come from the Worker's catalog (with a static fallback), one
    # deep-linkable row per kind with a Ping switch and, when emailable, an
    # Email switch that is disabled while the ping or master email is off.
    assert "session?.pingKindCatalog" in dashboard_js
    assert "PING_KIND_CATALOG_FALLBACK" in dashboard_js
    assert 'id="ping-kind-${kind}"' in dashboard_js
    assert 'data-ping-kind-toggle="${kind}"' in dashboard_js
    assert "disabled: !on || !emailOn" in dashboard_js
    assert "pingMutedKinds: { [kind]: enabled }" in dashboard_js

    # Quiet hours: switch, start/end, Monday=0 weekday toggles, a timezone
    # defaulting to the browser's, and a "Quiet until" status line.
    for day in range(7):
        assert f'data-ping-quiet-day="{day}"' in section
    assert 'data-ping-quiet-start type="time"' in section
    assert 'data-ping-quiet-end type="time"' in section
    assert "data-ping-quiet-timezone" in section
    assert "data-ping-quiet-enabled" in section
    assert "Intl.DateTimeFormat().resolvedOptions().timeZone" in dashboard_js
    assert "Quiet until ${time}" in dashboard_js
    assert "notificationQuietHours: { ...extra, timezone: zone }" in dashboard_js

    # Every switch auto-saves through the settings API and re-renders from
    # the reply; the section loads its own read when shown.
    assert 'fetch("/api/notifications", {' in dashboard_js
    assert "pingSettings: patch," in dashboard_js
    assert "applyPingSettingsPayload(reply)" in dashboard_js
    assert "&settings=1`" in dashboard_js
    assert 'if (activeSection === "notifications") loadPingSettings();' in dashboard_js
    assert "savePingSettings({ notificationDelivery: collectNotificationDelivery() })" in dashboard_js
    assert "savePingSettings({ emailNotifications:" in dashboard_js

    # Administrator alerts: only when the payload carries the matrix.
    assert "Continual ping" in section
    assert "every five minutes" in section
    assert "statusMonitorAlerts: { [id]: { [channel]:" in dashboard_js
    assert 'card.hidden = !rows;' in dashboard_js

    # Getting there from the pings: a "Ping settings" link in the bell (every
    # page) and the pings modal header, a trailing gear column on each modal
    # row, and "Turn off … pings" in the detail pane — all real links to the
    # section, switched in place when already on the settings page.
    assert home_page.count('href="/dashboard/settings/notifications" data-ping-settings-link=""') == 1
    assert deferred_dialogs().count('href="/dashboard/settings/notifications" data-ping-settings-link=""') == 1
    assert "Ping settings" in home_page
    assert "/dashboard/settings/notifications#ping-kind-${encodeURIComponent(key)}" in dashboard_js
    assert "data-notification-row" in dashboard_js
    assert "Turn off “${escapeHtml(label)}” pings" in dashboard_js
    assert "openPingSettingsInPlace(pingSettingsLink)" in dashboard_js
    assert "focusPingSettingsTarget" in dashboard_js
    assert "ping-highlight" in section
