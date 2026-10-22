#!/usr/bin/env python3
"""Unread counts are pushed, never polled (adhoc #1604).

"How many unread things do I have?" is a question only the relay can answer,
and clients used to ask it on a beat: the desktop re-read its ping inbox every
five minutes off the heartbeat, /world re-read the ping digest every minute,
and /chat re-read the direct-message list every thirty seconds plus the chat
activity counters every sixty. All four are now one read when the surface
opens, and after that the relay says so: notify_account_event fans a
payload-free {"type":"event","topic"} frame to the account's ForkMeshNodes
Durable Object the moment the write that moved a count lands.

These checks pin that contract on all three surfaces. See
docs/operations/polling-elimination.md.
"""

import ast
import base64
import hmac
import json
import re
from pathlib import Path


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

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parent
ENTRY_TEXT = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
APP_RUNTIME_TEXT = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
# The World and Office route bodies moved into sibling modules so Cloudflare's
# Python startup validation never parses them. These contracts still span the
# whole route family, so their source is read alongside entry.py.
WORLD_ROUTE_SOURCE = "".join(
    (Path(__file__).resolve().parents[2] / "world" / "src" / name).read_text(
        encoding="utf-8")
    for name in ("world_routes.py", "world_office_routes.py")
)
ENTRY_TEXT = (
    (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
    + WORLD_ROUTE_SOURCE
)
CHAT = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
CHANNEL = (ROOT / "public" / "account-events.js").read_text(encoding="utf-8")
QT_ACTIONS = (REPO / "desktop" / "src" / "MainWindowActions.cpp").read_text(
    encoding="utf-8")
QT_REPOS = "".join(
    (REPO / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_REPOS_SOURCES
)
QT_SETUP = (REPO / "desktop" / "src" / "MainWindowSetup.cpp").read_text(
    encoding="utf-8")
QT_CHAT = "".join(
    (REPO / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
QT_INTERNAL = (REPO / "desktop" / "src" / "MainWindowInternal.h").read_text(
    encoding="utf-8")
DOC = (REPO / "www" / "docs" / "operations" / "polling-elimination.md").read_text(
    encoding="utf-8")


# --- the relay's account-scoped push -----------------------------------------

def _function_source(name):
    tree = ast.parse(ENTRY_TEXT)
    for node in tree.body:
        if (
            isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            and node.name == name
        ):
            return ast.get_source_segment(ENTRY_TEXT, node)
    raise AssertionError("no such top-level function: " + name)


def test_account_event_push_targets_the_owner_node_durable_object():
    source = _function_source("notify_account_event")
    # Same channel as notify_repo_host, addressed by account rather than repo.
    assert "env.FORKMESH_NODES.idFromName(_node_events_do_name(owner))" in source
    assert "/api/nodes/notify" in source
    # Account-scoped state belongs to no repository, so no repo is asserted.
    assert "repo=" not in source
    # Best-effort: a push must never fail the write that triggered it.
    assert "except Exception:" in source
    # Awaited directly. Wrapping a JS-backed await in asyncio.wait_for leaves
    # the cancelled Pyodide task pending forever and wedges the isolate for
    # every later request — see tests/test_worker_task_concurrency.py.
    assert "await node_object.fetch(" in source
    assert "await asyncio.wait_for(" not in source
    # The same topic must also reach suspended phones through APNs.
    assert "await push_account_apns(env, owner, topic, push_alert)" in source


def test_issue_activity_reaches_the_account_event_relay_topic():
    source = _function_source("_accept_issue_submission")
    # Comments and closed-status changes make issue state stale on the owner's
    # clients; notify_account_event fans that topic to both socket and APNs.
    assert 'event_type == "comment"' in source
    assert 'event.get("status") == "closed"' in source
    assert 'notify_account_event(env, owner, "issues")' in source


def test_account_event_push_coalesces_a_burst_for_one_account():
    source = _function_source("notify_account_event")
    assert "_ACCOUNT_EVENT_PUSH_MEMO" in source
    assert "ACCOUNT_EVENT_PUSH_COALESCE_MS" in source
    assert "ACCOUNT_EVENT_PUSH_COALESCE_MS = 2000" in ENTRY_TEXT
    # The memo is per-isolate and must not grow without bound.
    assert "ACCOUNT_EVENT_PUSH_MEMO_MAX" in source
    assert "_ACCOUNT_EVENT_PUSH_MEMO.pop(stale, None)" in source


def test_every_write_that_moves_an_unread_count_pushes():
    # A ping row is the only thing that moves the bell's unread count...
    enqueue = _function_source("enqueue_notification")
    assert ('await notify_account_event(env, recipient, "pings", push_record)'
            in enqueue)
    # The phone banner is rendered from this very row, never a guess.
    assert 'push_record["id"] = dedupe_bi' in enqueue
    # ...and the retained-message counter is the only thing that moves a
    # conversation's unreadCount, for the participant who did not send it.
    retained = _function_source("_chat_direct_message_retained")
    assert "_chat_direct_message_parties(" in retained
    assert "not await _chat_blocked_by(env, recipient, sender)" in retained
    assert 'env, recipient, "direct-messages",' in retained
    assert ("direct_message_alert(\n                conversation_id=conversation_id, ref=ref,\n"
            "                key_version=key_version, frame=frame)") in retained
    recipient = _function_source("_chat_direct_message_parties")
    assert "await blind_index(env, name) != sender_bi" in recipient
    # Participants are immutable, so the lookup is memoized per isolate — but a
    # failed read must not be remembered as "nobody to notify".
    assert "_CHAT_DIRECT_PEER_MEMO" in recipient
    assert recipient.index("except Exception:") < recipient.index(
        "_CHAT_DIRECT_PEER_MEMO[memo_key] = (peer, sender)")


def test_a_list_that_changed_for_someone_elses_reason_is_pushed_too():
    # Dropping the 30s conversation/channel re-read also dropped how an invite
    # or a newly-opened conversation was discovered. Both now push, and neither
    # notifies the actor who caused it.
    channels = (ROOT / "src" / "chat_channels_api.py").read_text(
        encoding="utf-8")
    assert channels.count(
        'await runtime.notify_account(canonical, "private-channels")') == 2
    assert channels.count("if canonical != actor:") == 2
    direct = (ROOT / "src" / "chat_direct_messages_api.py").read_text(
        encoding="utf-8")
    assert ('await runtime.notify_account(\n        target, "direct-messages", '
            'started_by=actor)') in direct
    # The capability is on the shared runtime base, so both APIs inherit it.
    base = ENTRY_TEXT[
        ENTRY_TEXT.index("class _WorldCommunityRuntime"):
        ENTRY_TEXT.index("class _OfficeMarketingTasksRuntime")
    ]
    assert 'async def notify_account(self, owner, topic, started_by=""):' in base
    assert ("await notify_account_event(self.env, owner, topic, push_alert)"
            in base)


# --- the browser's key to that channel ---------------------------------------

def _ticket_namespace():
    """Execute the account-event ticket helpers with stub primitives."""
    want = {
        "_account_event_ticket_signature",
        "_account_event_ticket_encode",
        "_account_event_ticket_owner",
    }
    tree = ast.parse(ENTRY_TEXT)
    body = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name in want
    ]
    assert {node.name for node in body} == want, "missing ticket helpers"
    clock = {"now": 1_700_000_000_000}

    class _Date:
        @staticmethod
        def now():
            return clock["now"]

    namespace = {
        "hmac": hmac,
        "base64": base64,
        "json": json,
        "re": re,
        "Date": _Date,
        "MAX_NODE_NAME": 63,
        "ACCOUNT_EVENT_TICKET_TTL_MS": 60 * 1000,
        "_account_session_secret": lambda _env: b"unit-test-secret",
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "safe_segment": lambda value: (
            str(value or "").strip().lower()
            if re.fullmatch(r"[a-z0-9][a-z0-9-]{0,62}",
                            str(value or "").strip().lower())
            else ""
        ),
    }
    module = ast.fix_missing_locations(ast.Module(body=body, type_ignores=[]))
    exec(compile(module, "entry.py", "exec"), namespace)
    return namespace, clock


def test_event_ticket_proves_one_account_for_one_minute():
    namespace, clock = _ticket_namespace()
    now = clock["now"]
    ticket = namespace["_account_event_ticket_encode"](
        None, {"name": "alice", "issuedAt": now, "expiresAt": now + 60_000})
    assert namespace["_account_event_ticket_owner"](None, ticket) == "alice"
    # Expired is refused, not merely stale.
    clock["now"] = now + 60_001
    assert namespace["_account_event_ticket_owner"](None, ticket) == ""


def test_event_ticket_refuses_forgery_and_over_long_lifetimes():
    namespace, clock = _ticket_namespace()
    now = clock["now"]
    ticket = namespace["_account_event_ticket_encode"](
        None, {"name": "alice", "issuedAt": now, "expiresAt": now + 60_000})
    payload, signature = ticket.rsplit(".", 1)
    # A tampered body cannot keep a valid signature.
    forged = base64.urlsafe_b64encode(
        json.dumps({"name": "bob", "issuedAt": now,
                    "expiresAt": now + 60_000}).encode()
    ).decode().rstrip("=")
    assert namespace["_account_event_ticket_owner"](
        None, forged + "." + signature) == ""
    assert namespace["_account_event_ticket_owner"](
        None, payload + "." + "0" * len(signature)) == ""
    assert namespace["_account_event_ticket_owner"](None, "") == ""
    assert namespace["_account_event_ticket_owner"](None, "no-dot") == ""
    # A self-minted claim asking for a year of access is rejected on TTL even
    # though only the relay could have signed it.
    forever = namespace["_account_event_ticket_encode"](
        None, {"name": "alice", "issuedAt": now,
               "expiresAt": now + 365 * 24 * 60 * 60 * 1000})
    assert namespace["_account_event_ticket_owner"](None, forever) == ""


def test_event_ticket_is_minted_from_a_session_and_accepted_on_upgrade():
    handler = _function_source("account_event_ticket_handler")
    # A session, never a self-asserted name.
    assert "_authed_account_name(env, request)" in handler
    assert '"authenticated": False' in handler
    assert 'method_name(request) != "GET"' in handler
    authorization = _function_source("_node_events_authorization")
    events = _function_source("node_events_handler")
    assert '_account_event_ticket_owner(env, ticket)' in authorization
    # The signed node token remains the other way in, and either path is
    # checked BEFORE a Durable Object is selected.
    assert "_authorize_owner(env, request, owner)" in authorization
    selector = _function_source("_node_events_object")
    assert "_node_events_authorization(env, request)" in selector
    assert "_node_events_object(env, request)" in events
    assert events.index("_node_events_object") < events.index(
        "env.FORKMESH_NODES.idFromName")
    assert '"/api/accounts/event-ticket"' in APP_RUNTIME_TEXT


def test_ticket_endpoint_is_never_cached():
    handler = _function_source("account_event_ticket_handler")
    assert handler.count("no-store, max-age=0, must-revalidate") == 3


# --- desktop: one read per run ------------------------------------------------

def test_desktop_reads_the_ping_inbox_once_per_run():
    # The five-minute throttle is gone: an unforced call after the first
    # successful read returns without touching the network.
    assert "now - m_webAlertsFetchedAtMs < 300000" not in QT_ACTIONS
    assert (
        "    if (m_webAlertsFetchedAtMs > 0 &&\n"
        "        (!force || now - m_webAlertsFetchedAtMs < "
        "kWebAlertPushFloorMs))\n"
        "        return;"
    ) in QT_ACTIONS
    # A burst of pushes still collapses into one read.
    assert "constexpr qint64 kWebAlertPushFloorMs = 5000;" in QT_INTERNAL
    # The heartbeat's call survives only as the retry for a failed first read.
    assert "refreshWebAlerts();" in QT_SETUP


def test_desktop_refreshes_pings_on_push_and_on_reconnect_catch_up():
    assert 'if (topic == QLatin1String("pings")) {' in QT_REPOS
    assert "refreshWebAlerts(true);" in QT_REPOS
    # The DO is per account, so it now also carries the web-only topic. Letting
    # that fall through would drag a signed /api/sync behind every direct
    # message the user receives.
    assert 'if (topic == QLatin1String("direct-messages")) {' in QT_REPOS
    assert QT_REPOS.index('QLatin1String("direct-messages")') < QT_REPOS.index(
        "scheduleRelaySync();")
    # No fallback poll sits behind the socket, so the (re)connect catch-up is
    # what covers a ping raised while the channel was down.
    reconnect = QT_REPOS.split(
        "&NodeEventSocket::connectedChanged", 1
    )[1].split("&NodeEventSocket::systemMessage", 1)[0]
    assert reconnect.index("scheduleRelaySync();") < reconnect.index(
        "refreshWebAlerts(true);"
    )
    # Opening the Pings page is a user action and outranks the one-shot seed.
    assert "refreshWebAlerts(true);" in QT_CHAT


def test_a_failed_first_read_is_retried_rather_than_stranding_the_badge():
    # Without this the single read of a run could fail at launch and the bell
    # would stay dark for the whole session.
    refresh = QT_ACTIONS[
        QT_ACTIONS.index("void MainWindow::refreshWebAlerts(bool force)")
        :QT_ACTIONS.index("void MainWindow::clearWebAlerts", QT_ACTIONS.index(
            "void MainWindow::refreshWebAlerts(bool force)"))
    ]
    failure = refresh[
        refresh.index("if (reply->error() != QNetworkReply::NoError")
        :refresh.index("const QJsonObject payload")
    ]
    assert "m_webAlertsFetchedAtMs = 0;" in failure
    assert "return;" in failure


# --- web: one read per page open ----------------------------------------------

def test_shared_account_event_channel_has_no_fallback_poll():
    assert 'const TICKET_ENDPOINT = "/api/accounts/event-ticket";' in CHANNEL
    assert '"/api/nodes/events"' in CHANNEL
    assert 'url.searchParams.set("ticket", ticket)' in CHANNEL
    # Reconnect is unconditional with bounded backoff, and every (re)connect
    # fires one catch-up through onConnected.
    assert "const MAX_RECONNECT_MS = 30000;" in CHANNEL
    assert "onConnected()" in CHANNEL
    # Signed out, the channel goes dark instead of retrying the ticket forever
    # (which would just be a poll of the ticket endpoint) — including the case
    # that matters most, a stale token still sitting in localStorage, which the
    # relay answers 200 + authenticated:false rather than 401.
    assert "if (!sessionToken()) {\n      started = false;\n      return;" in (
        CHANNEL)
    assert "if (data.authenticated === false) return { signedOut: true };" in (
        CHANNEL)
    assert "if (signedOut) {\n      // The stored session proves nobody." in (
        CHANNEL)
    # A transient relay failure is the opposite case and must keep retrying.
    assert "if (!ticket) {\n      scheduleReconnect();" in CHANNEL
    # Keepalive stays inside the Durable Object's staleness reaper.
    assert "const KEEPALIVE_MS = 4 * 60 * 1000;" in CHANNEL


def test_shared_channel_module_revalidates_with_the_world_module_graph():
    # /world/* is no-store so a refresh can never pin an older module graph.
    # This module is part of that graph but lives outside the tree, so without
    # its own rule it would fall to the default and could be served stale
    # against a freshly deployed world.js — the same trap /chat-moderation.js
    # already has a rule for.
    headers = (REPO / "www" / "public" / "_headers").read_text(encoding="utf-8")
    rule = headers[headers.index("/account-events.js"):]
    rule = rule[:rule.index("\n\n")]
    assert "Cache-Control: no-store" in rule
    # It stays outside public/world/ and is service-proxied from the App, so the
    # World deployment does not upload a second copy.
    assert not (
        ROOT.parent / "world" / "public" / "world" / "account-events.js"
    ).exists()
    worker = (ROOT.parent / "world" / "worker.js").read_text(
        encoding="utf-8"
    )
    assert '"/account-events.js",' in worker
    assert "return appAsset(request, env);" in worker


def test_chat_page_stops_polling_conversations_and_activity():
    # The 30s conversation re-read and its constant are gone.
    assert "PRIVATE_CHANNEL_REFRESH_MS" not in CHAT
    assert "startAccountEventChannel();" in CHAT
    assert 'if (topic === "direct-messages") {' in CHAT
    # That 30s poll also discovered lists that changed for somebody ELSE's
    # reason — a channel invite, a conversation somebody opened with us — so
    # both of those are pushed too rather than quietly becoming reload-only.
    assert '} else if (topic === "private-channels") {' in CHAT
    # The first connect needs no catch-up: the caller just read both lists.
    assert "if (!accountEventsConnectedOnce) {" in CHAT
    # The remaining interval is the (edge-cached) registered-user directory,
    # which is not an unread count and has no push behind it yet. The activity
    # counters must not ride along on it any more.
    assert "if (!document.hidden) refreshUsersDirectory();" in CHAT
    assert "}, USERS_DIRECTORY_REFRESH_MS);" in CHAT
    assert "setInterval" not in _js_slice(CHAT, "async function markChatActivitySeen()")
    # The incremental bumps keep the on-page badge at zero, but two open tabs
    # share one localStorage baseline and would each advance it for the same
    # message. The absolute re-read on the way out is the drift correction that
    # the 60s timer used to provide — event-driven, with a floor so flicking
    # between tabs cannot turn it back into a poll.
    assert "rebaselineChatActivityOnLeave" in CHAT
    assert 'document.addEventListener("visibilitychange", ' \
        "rebaselineChatActivityOnLeave)" in CHAT
    assert 'window.addEventListener("pagehide", ' \
        "rebaselineChatActivityOnLeave)" in CHAT
    assert "CHAT_ACTIVITY_REBASELINE_FLOOR_MS = 5000" in CHAT
    assert "keepalive: true," in CHAT


def test_world_stops_polling_personal_pings():
    assert "WORLD_NOTIFICATION_POLL_MS" not in WORLD
    assert "startNotificationPolling" not in WORLD
    assert "startNotificationChannel()" in WORLD
    assert 'if (topic !== "pings" && topic !== "issues") return;' in WORLD
    assert 'createAccountEventChannel } from "../account-events.js"' in WORLD
    # A push-triggered read has to see the ping that triggered it, so the
    # digest cache window is short rather than a poll interval.
    assert "const WORLD_NOTIFICATION_DIGEST_MAX_AGE_MS = 2000;" in WORLD
    # Signing in or out elsewhere re-opens the channel under the new identity.
    assert "this.startNotificationChannel();" in WORLD


def test_the_polling_inventory_records_all_four():
    # The four pollers by their old names, so a reader can find what went away.
    for eliminated in (
        "refreshWebAlerts",
        "startNotificationPolling",
        "refreshDirectMessages",
        "markChatActivitySeen",
    ):
        assert eliminated in DOC
    # And the mechanism that replaced them.
    for replacement in (
        "notify_account_event",
        "account-events.js",
        "/api/accounts/event-ticket",
        "noteSeenChatActivity",
    ):
        assert replacement in DOC
    # The desktop directory timer went the same way later; the doc still
    # names it so a reader can find what was removed.
    assert "m_chatDirectoryTimer" in DOC
    assert "USERS_DIRECTORY_REFRESH_MS" in DOC


def test_no_surface_reads_a_count_it_was_not_pushed():
    # A guard against the easy regression: re-adding a "safety net" interval
    # around any of the four reads this change removed.
    assert "setInterval" not in _js_slice(
        CHAT, "function startAccountEventChannel()")
    assert "setInterval" not in _js_slice(
        WORLD, "  startNotificationChannel() {")


def _js_slice(source, marker, lines=30):
    index = source.index(marker)
    return "\n".join(source[index:].splitlines()[:lines])
