#!/usr/bin/env python3
"""Desktop-app error reports become operational rows, and so become pings.

A warning dialog on a machine nobody is sitting in front of, or an error notice
on a headless mirror, used to be the entire record of a failure: nothing was
written anywhere central, so nothing pinged. POST /api/desktop-errors puts those
reports in the same error_log as browser and Worker errors, which raises the
existing "new error group" ping on the first sighting (adhoc #1538).

Two properties matter enough to pin: an unsigned report is refused (the endpoint
must not become a second anonymous write path into D1), and a report is bounded
and redacted exactly the way the browser collector's are.
"""

import ast
import re
from pathlib import Path


# MainWindowChat.cpp was split into feature files; read them in the original
# source order so section slices that span functions still line up.
MAIN_WINDOW_CHAT_SOURCES = (
    "MainWindowChat.cpp",
    "MainWindowNetworkInternal.h",
    "MainWindowDiagnostics.cpp",
    "MainWindowLogs.cpp",
    "MainWindowNavigation.cpp",
    "MainWindowNetworkRepos.cpp",
    "MainWindowHosts.cpp",
    "MainWindowNodes.cpp",
    "MainWindowNetworkDiagnostics.cpp",
    "MainWindowUsers.cpp",
    "MainWindowVultrMirrors.cpp",
    "MainWindowHostInstall.cpp",
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
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
ADMIN = (ROOT / "src" / "admin_console.py").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
WORLD_CSS = (ROOT.parent / "world" / "public" / "world" / "world.css").read_text(
    encoding="utf-8")
QT_REPORTS_HEADER = (
    ROOT.parent / "desktop" / "src" / "ClientErrorReports.h"
).read_text(encoding="utf-8")
QT_REPORTS = (
    ROOT.parent / "desktop" / "src" / "ClientErrorReports.cpp"
).read_text(encoding="utf-8")
QT_EVENT_FILTER = (
    ROOT.parent / "desktop" / "src" / "MainWindowIssues.cpp"
).read_text(encoding="utf-8")
QT_MESSAGES = (
    ROOT.parent / "desktop" / "src" / "MainWindowMessages.cpp"
).read_text(encoding="utf-8")
QT_SETTINGS = (
    ROOT.parent / "desktop" / "src" / "MainWindowSettings.cpp"
).read_text(encoding="utf-8")
QT_ACTIONS = (
    ROOT.parent / "desktop" / "src" / "MainWindowActions.cpp"
).read_text(encoding="utf-8")
QT_AGENTS = "".join(
    (ROOT.parent / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)
QT_BRANCHES = (
    ROOT.parent / "desktop" / "src" / "MainWindowBranches.cpp"
).read_text(encoding="utf-8")
QT_TASKS = (
    ROOT.parent / "desktop" / "src" / "MainWindowTasks.cpp"
).read_text(encoding="utf-8")
QT_CHAT = "".join(
    (ROOT.parent / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
QT_ISSUES = (
    ROOT.parent / "desktop" / "src" / "MainWindowIssues.cpp"
).read_text(encoding="utf-8")
QT_PULLS = (
    ROOT.parent / "desktop" / "src" / "MainWindowPulls.cpp"
).read_text(encoding="utf-8")
QT_TIPS = (
    ROOT.parent / "desktop" / "src" / "MainWindowTips.cpp"
).read_text(encoding="utf-8")


def _desktop_error_fields():
    """Run the real validator with nothing but its own dependencies."""
    tree = ast.parse(ENTRY)
    names = {"DESKTOP_ERROR_SURFACES", "DESKTOP_ERROR_KINDS"}
    body = []
    for node in tree.body:
        if isinstance(node, ast.Assign) and any(
            isinstance(target, ast.Name) and target.id in names
            for target in node.targets
        ):
            body.append(node)
        elif (
            isinstance(node, ast.FunctionDef)
            and node.name in {
                "_sanitize_client_error_text",
                "_desktop_error_fields",
            }
        ):
            body.append(node)
    namespace = {"re": re}
    exec(compile(ast.Module(body=body, type_ignores=[]), "<entry>", "exec"),
         namespace)
    return namespace["_desktop_error_fields"]


def _expected_desktop_probe_noise():
    """Run the real compatibility filter without importing the Worker."""
    tree = ast.parse(ENTRY)
    body = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef)
        and node.name == "_expected_desktop_probe_noise"
    ]
    namespace = {}
    exec(compile(ast.Module(body=body, type_ignores=[]), "<entry>", "exec"),
         namespace)
    return namespace["_expected_desktop_probe_noise"]


def test_the_collector_is_signed_bounded_and_stored_in_error_log():
    assert "async def desktop_error_handler(env, request):" in ENTRY
    assert (
        'if url.path in ("/api/desktop-errors", "/api/desktop-errors/"):'
        in APP_RUNTIME
    )
    assert "return await desktop_error_handler(self.env, request)" in APP_RUNTIME
    # Signed owner/ts/sig proof — the same one GET /api/sync requires.
    assert "if not owner or not await _authorize_owner(env, request, owner):" in ENTRY
    assert "DESKTOP_ERROR_MAX_BODY" in ENTRY
    assert "DESKTOP_ERROR_RATE_PER_OWNER" in ENTRY
    assert "DESKTOP_ERROR_RATE_GLOBAL" in ENTRY
    assert '"/desktop-error/" + surface' in ENTRY
    body = ENTRY[ENTRY.index("async def desktop_error_handler(env, request):"):]
    body = body[:body.index("async def world_admin_errors_handler(")]
    # Cheap shape checks before the signature check, and the signature check
    # before any bounded-row read.
    assert body.index('method_name(request) != "POST"') < body.index(
        "_authorize_owner(")
    assert body.index("_authorize_owner(") < body.index("SELECT COUNT(*)")
    assert body.count("AND method='APP'") == 3  # per-owner, global, duplicate
    assert '"app:" + str(owner_hash)[:32]' in body
    assert "await _write_error_log(" in body
    assert "521," in body and '"APP",' in body
    # An unauthorized or over-quota report is never a new user-visible error.
    assert body.count('{"ok": True, "stored": False}') == 2
    assert "except Exception:" in body


def test_a_report_names_its_kind_and_install_but_no_free_text_route():
    fields = _desktop_error_fields()
    assert fields({"kind": "dialog", "surface": "app", "message": "boom"}) == (
        "app", "desktop dialog [app] boom")
    assert fields(
        {"kind": "notice", "surface": "headless", "title": "Sync inbox",
         "message": "relay is rate-limited"}
    ) == (
        "headless",
        "desktop notice [headless] Sync inbox — relay is rate-limited")
    assert fields(
        {"kind": "toast", "surface": "app", "message": "boom"}
    ) == ("app", "desktop toast [app] boom")
    # Unknown areas and kinds are refused rather than becoming their own group.
    assert fields({"kind": "dialog", "surface": "../admin", "message": "x"}) is None
    assert fields({"kind": "chatter", "surface": "app", "message": "x"}) is None
    assert fields({"kind": "dialog", "surface": "app", "message": " "}) is None
    assert fields("not a payload") is None


def test_a_report_of_a_report_never_becomes_a_second_group():
    # adhoc #1654: the first sighting of a desktop error pings the account, the
    # desktop shows that ping as a warning card, and reporting the card back
    # stored the group's own text as a brand-new error — which pinged again,
    # quoting more of itself each round. Anything already wearing this route's
    # stamp is refused, whatever the reporting node's version.
    fields = _desktop_error_fields()
    assert fields({
        "kind": "toast", "surface": "app",
        "message": ("New app error group (521) — APP /desktop-error/app — "
                    "desktop toast [app] Action failed"),
    }) is None
    assert fields({
        "kind": "toast", "surface": "headless",
        "message": "Worker errors needs attention - 521 on "
                   "/desktop-error/headless: Desktop Toast [headless] boom",
    }) is None
    assert fields({
        "kind": "notice", "surface": "app",
        "message": ("New app error group (521) — APP /desktop-error/app — "
                    "desktop notice [app] Action failed"),
    }) is None
    # A failure that merely says the word "toast" is still a real report.
    assert fields({"kind": "toast", "surface": "app",
                   "message": "toast queue overflowed"}) is not None
    # The desktop holds the same alerts back before they ever go on the wire.
    assert "bool ClientErrorReports::echoesRelayAlert(" in QT_REPORTS
    assert 'QLatin1String("error_group")' in QT_REPORTS
    assert 'QLatin1String("operational_alert")' in QT_REPORTS
    flash = QT_SETTINGS[QT_SETTINGS.index("void MainWindow::flashMessageImpl("):]
    flash = flash[:flash.index("void MainWindow::notifyIfInactive(")]
    assert "!forkmesh::ClientErrorReports::echoesRelayAlert(kind, text)" in flash


def test_old_desktop_probe_artifacts_are_accepted_without_being_stored():
    expected = _expected_desktop_probe_noise()
    assert expected(
        "app",
        "desktop toast [app] The site on app.forkmesh.com is down — "
        "The site returned HTTP 308 to this desktop.",
    )
    assert expected(
        "app",
        "desktop toast [app] The site on app.forkmesh.com is down — The site "
        "could not be reached from this desktop: Server received frame(s) on "
        "a half-closed stream",
    )
    # The desktop aborting its own probe says nothing about the site.
    assert expected(
        "app",
        "desktop toast [app] The site on app.forkmesh.com is down — The site "
        "could not be reached from this desktop: Operation canceled",
    )
    assert expected(
        "app",
        "desktop toast [app] The /status page on app.forkmesh.com is down — "
        "The /status page could not be reached from this desktop: Operation "
        "canceled",
    )
    # A site that really hangs still counts.
    assert not expected(
        "app",
        "desktop toast [app] The site on app.forkmesh.com is down — The site "
        "could not be reached from this desktop: Operation timed out",
    )
    assert not expected(
        "app",
        "desktop toast [app] The site on app.forkmesh.com is down — The site "
        "returned HTTP 521 to this desktop.",
    )
    assert not expected(
        "headless",
        "desktop toast [headless] The site on app.forkmesh.com is down — The "
        "site returned HTTP 308 to this desktop.",
    )
    handler = ENTRY[ENTRY.index("async def desktop_error_handler(env, request):"):]
    handler = handler[:handler.index("async def world_admin_errors_handler(")]
    assert "_expected_desktop_probe_noise(surface, detail)" in handler
    assert '{"ok": True, "stored": False, "expected": True}' in handler


def test_a_report_cannot_carry_identity_paths_or_credentials():
    surface, detail = _desktop_error_fields()({
        "kind": "dialog",
        "surface": "app",
        "title": "Push to alice@example.com",
        "message": (
            "token=super-secret-value could not push "
            "https://forkmesh.com/alice/private-repo "
            "from /home/alice/work/private-repo"),
    })
    assert surface == "app"
    assert "super-secret-value" not in detail
    assert "alice@example.com" not in detail
    assert "forkmesh.com" not in detail
    assert "/home/alice" not in detail
    assert "could not push" in detail


def test_recovery_flash_wins_over_an_older_unread_outage():
    # The inbox is newest first. Replaying that order left an older red outage
    # card queued behind the green recovery card, so the desktop ended red even
    # though the current /status probe had recovered.
    assert "QList<QJsonObject> pendingAlerts;" in QT_ACTIONS
    assert "std::stable_sort(" in QT_ACTIONS
    assert "pendingAlerts.begin(), pendingAlerts.end()" in QT_ACTIONS
    assert "for (const QJsonObject &alert : std::as_const(pendingAlerts))" in QT_ACTIONS
    assert "webPingIsRecovery(alert)" in QT_ACTIONS
    assert "pendingAlerts.append(alert);" in QT_ACTIONS


def test_desktop_rows_read_apart_from_browser_and_worker_rows():
    assert 'str(method or "").strip().upper() == "APP"' in ADMIN
    assert 'str(path or "").startswith("/desktop-error/")' in ADMIN
    assert 'return "Desktop"' in ADMIN
    assert 'class="src %s"' in ADMIN
    assert ".src.desktop{" in (ROOT / "src" / "admin_shell.py").read_text(
        encoding="utf-8")
    assert 'item.method === "APP"' in WORLD
    assert 'String(item.path || "").startsWith("/desktop-error/")' in WORLD
    assert '"Desktop"' in WORLD
    assert ".world-error-source--desktop {" in WORLD_CSS


def test_the_desktop_client_reports_every_error_surface_once():
    # The modal path is caught application-wide, so no call site has to remember.
    assert "qobject_cast<QMessageBox *>(obj)" in QT_EVENT_FILTER
    assert "icon == QMessageBox::Warning" in QT_EVENT_FILTER
    assert "icon == QMessageBox::Critical" in QT_EVENT_FILTER
    # A warning that asks something is a confirmation prompt, not a failure.
    assert "const bool informational =" in QT_EVENT_FILTER
    assert "box->buttons()" in QT_EVENT_FILTER
    assert "ClientErrorReports::dialogAsksQuestion(buttons)" in QT_EVENT_FILTER
    assert 'reportUserVisibleError(QStringLiteral("dialog")' in QT_EVENT_FILTER
    assert "kLocalOperationDialogProperty" in QT_REPORTS_HEADER
    assert "box->property(forkmesh::kLocalOperationDialogProperty)" in QT_EVENT_FILTER
    assert "if (failure && !localOperation)" in QT_EVENT_FILTER
    # The notice path reports from flashMessageImpl, the half that records the
    # failure, not from showPageNotice, the half that shows it inline: a
    # headless node has no notice strip at all (and its failures are the
    # invisible ones), and the logged-error hook never reports a line a second
    # time. No desktop path sends the retired "toast" kind any more.
    flash = QT_SETTINGS[QT_SETTINGS.index("void MainWindow::flashMessageImpl("):]
    flash = flash[:flash.index("void MainWindow::notifyIfInactive(")]
    show = QT_SETTINGS[QT_SETTINGS.index("void MainWindow::showPageNotice("):]
    show = show[:show.index("void MainWindow::clearPageNotice(")]
    assert 'reportUserVisibleError(QStringLiteral("notice")' in flash
    assert "reportUserVisibleError(" not in show
    assert "if (!m_pageNoticeArea)" not in flash
    assert 'QStringLiteral("toast")' not in QT_SETTINGS
    assert "/api/desktop-errors" in QT_MESSAGES
    assert "signedInboxQuery(owner)" in QT_MESSAGES
    assert "hostInCooldown(url.host())" in QT_MESSAGES
    assert "ClientErrorReports::relayAccepts(report)" in QT_MESSAGES
    assert "ClientErrorReports::permanentRefusal(status)" in QT_MESSAGES


def test_expected_desktop_guidance_stays_local():
    # These are answers to the operator's current click, not release defects.
    # Keep the warning styling but label the notice as a local operation so the
    # central error log remains reserved for failures another release can fix.
    for source, message in (
        (QT_TASKS, "Type a task first."),
        (QT_AGENTS, "no local checkout to run the agent"),
        (QT_AGENTS, "Type a task first to start an agent."),
        (QT_BRANCHES, "read-only mirror; branches can't be created"),
        (QT_BRANCHES, "checkout has uncommitted changes"),
        (QT_CHAT, "No complete JSON object or array was found"),
        # Assigning an issue is host-only, so the sidebar's refusal answers the
        # click (issue #638). The generic save failure below it is not guidance
        # - a write refused on a host that can write is a defect - and reports.
        (QT_ISSUES, "kIssueAssigneesHostOnlyNotice, true,"),
        # "Node was not deleted. Click Delete again, then type ... to
        # confirm." is a typed-confirmation gate refusing a mistyped phrase.
        # Its text quotes the node name, so the exact-message grouping opened a
        # fresh error group and pinged every administrator for every new name.
        (QT_CHAT, "exactly, including uppercase DELETE, to confirm."),
        (QT_CHAT, "Only a platform admin can delete a node from the mesh."),
        (QT_CHAT, "Sign in to your admin account from Settings first"),
        (QT_CHAT, r'Deletion of \"%1\" is already in progress.'),
    ):
        start = source.index(message)
        assert "kLocalOperationNoticeKind" in source[start:start + 500]

    # This refusal retains its modal warning. Its explicit property gives the
    # event filter the same local-operation classification as the notices above.
    start = QT_PULLS.index("Commit or stash local changes before updating the")
    assert "kLocalOperationDialogProperty" in QT_PULLS[start:start + 500]


def test_saved_payout_receipt_waits_for_deferred_startup_authentication():
    announce = QT_TIPS[QT_TIPS.index("void MainWindow::announceTip("):]
    announce = announce[:announce.index("// --- Tipping straight from")]
    wait = announce.index("automated && !m_startupAuthResolved")
    network = announce.index("if (!m_networkAccess)")
    notice_request = announce.index("prepareContributorNoticeRequest(endpoint")
    assert wait < network < notice_request
    wait_block = announce[wait:network]
    assert "kPendingReceiptStartupAuthPollMs" in wait_block
    assert "QTimer::singleShot(" in wait_block
    assert "finishContributorPayoutTransfer" not in wait_block


def test_the_client_bounds_and_redacts_before_anything_leaves_the_machine():
    assert "kMaxPerWindow = 8" in QT_REPORTS_HEADER
    assert "kMaxDeferred = 8" in QT_REPORTS_HEADER
    assert "kMaxAttempts = 3" in QT_REPORTS_HEADER
    assert 'QStringLiteral("diagnostics/reportErrors")' in QT_REPORTS_HEADER
    for placeholder in ("<secret>", "<email>", "<url>", "<id>", "<path>"):
        assert placeholder in QT_REPORTS
