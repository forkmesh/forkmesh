"""The streamed provider sign-in: frame grammar, relay wiring, node halves.

The website's "Sign in to Claude Code on <node>" control used to be a
``forkmesh://agent-login/...`` link, which a browser can only complete on a
machine that has the desktop app installed. The terminal now streams to the
browser over the per-owner node socket instead, so these tests pin the parts
that have to agree byte for byte across the relay, the two node
implementations, and the page.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = ROOT.parent
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import node_terminal  # noqa: E402


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

ENTRY = (SRC / "entry.py").read_text(encoding="utf-8")
TERMINAL_JS = (ROOT / "public" / "agent-terminal.js").read_text(encoding="utf-8")
EVENTS_JS = (ROOT / "public" / "account-events.js").read_text(encoding="utf-8")
DASHBOARD = (
    ROOT / "public" / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")
MODALS = (
    ROOT / "public" / "dashboard" / "partials" / "modals.html"
).read_text(encoding="utf-8")
# The "Start an agent" popover, added under the header's toggle on first open
# (dashboard_bundle.DEFERRED_MARKUP).
HEADER = (
    ROOT / "public" / "dashboard" / "partials" / "agent-modal.html"
).read_text(encoding="utf-8")
GO_TERMINAL = (REPOSITORY / "server" / "terminal.go").read_text(encoding="utf-8")
GO_EVENTS = (REPOSITORY / "server" / "nodeevents.go").read_text(encoding="utf-8")
DESKTOP_CHAT = "".join(
    (REPOSITORY / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
DESKTOP_PTY = (
    REPOSITORY / "desktop" / "src" / "AgentLoginPty.cpp"
).read_text(encoding="utf-8")


def _open_frame(**overrides):
    frame = {
        "action": "open",
        "session": "0123456789abcdef",
        "node": "mirror35",
        "provider": "claude-code",
        "cols": 100,
        "rows": 30,
    }
    frame.update(overrides)
    return frame


def test_browser_open_is_rebuilt_field_by_field_and_labelled():
    forwarded = node_terminal.normalize_terminal_frame(
        _open_frame(node="Mirror35", extra="not forwarded"), "browser")
    assert forwarded == {
        "type": "terminal",
        "action": "open",
        "session": "0123456789abcdef",
        "node": "mirror35",
        "from": "browser",
        "provider": "claude-code",
        "cols": 100,
        "rows": 30,
    }


def test_each_role_may_only_send_its_own_half_of_the_stream():
    # A node cannot ask a browser to open a session, and a browser cannot
    # claim to be pty output: the relay decides which direction a frame is
    # allowed to travel before it forwards anything.
    assert node_terminal.normalize_terminal_frame(_open_frame(), "node") is None
    assert node_terminal.normalize_terminal_frame(
        {"action": "output", "session": "0123456789abcdef",
         "node": "mirror35", "data": "aGk="}, "browser") is None
    assert node_terminal.normalize_terminal_frame(
        {"action": "output", "session": "0123456789abcdef",
         "node": "mirror35", "data": "aGk="}, "node")["from"] == "node"


def test_a_frame_that_does_not_name_a_session_and_node_is_refused():
    assert node_terminal.normalize_terminal_frame(
        _open_frame(session="short"), "browser") is None
    assert node_terminal.normalize_terminal_frame(
        _open_frame(session="not a session id"), "browser") is None
    assert node_terminal.normalize_terminal_frame(
        _open_frame(node="mirror 35"), "browser") is None
    assert node_terminal.normalize_terminal_frame(
        _open_frame(node=""), "browser") is None
    assert node_terminal.normalize_terminal_frame(_open_frame(), "relay") is None


def test_only_the_two_provider_logins_can_be_opened():
    assert node_terminal.normalize_terminal_frame(
        _open_frame(provider="bash"), "browser") is None
    assert node_terminal.normalize_terminal_frame(
        _open_frame(provider="codex"), "browser")["provider"] == "codex"


def test_payloads_are_bounded_base64_and_sizes_are_clamped():
    oversize = "A" * (node_terminal.MAX_DATA_CHARS + 4)
    assert node_terminal.normalize_terminal_frame(
        {"action": "input", "session": "0123456789abcdef",
         "node": "mirror35", "data": oversize}, "browser") is None
    assert node_terminal.normalize_terminal_frame(
        {"action": "input", "session": "0123456789abcdef",
         "node": "mirror35", "data": "<script>"}, "browser") is None
    resized = node_terminal.normalize_terminal_frame(
        {"action": "resize", "session": "0123456789abcdef",
         "node": "mirror35", "cols": 9000, "rows": 1}, "browser")
    assert (resized["cols"], resized["rows"]) == (
        node_terminal.MAX_COLS, node_terminal.MIN_ROWS)


def test_a_node_frame_carries_a_message_or_an_exit_status_but_not_both():
    error = node_terminal.normalize_terminal_frame(
        {"action": "error", "session": "0123456789abcdef", "node": "mirror35",
         "message": "x" * 400}, "node")
    assert len(error["message"]) == node_terminal.MAX_MESSAGE_CHARS
    assert "code" not in error
    exited = node_terminal.normalize_terminal_frame(
        {"action": "exit", "session": "0123456789abcdef", "node": "mirror35",
         "code": 4096}, "node")
    assert exited["code"] == 255
    assert "message" not in exited


def test_a_terminal_frame_fits_the_node_clients_own_socket_limit():
    # server/nodeevents.go drops the WHOLE socket on an oversize frame, so the
    # relay's payload ceiling has to leave room for the envelope inside it.
    assert "eventMaxFrameBytes  = 16 << 10" in GO_EVENTS
    assert node_terminal.MAX_DATA_CHARS + 512 < 16 * 1024
    # The node's own output chunk must survive base64 within that ceiling.
    assert "terminalMaxOutputChunk = 6144" in GO_TERMINAL
    assert 6144 * 4 // 3 < node_terminal.MAX_DATA_CHARS


def test_terminal_frames_are_metered_apart_from_the_event_budget():
    window, maximum = 10_000, 3
    start, count, allowed = node_terminal.terminal_rate_state(
        1_000, 0, 0, window, maximum)
    assert (start, count, allowed) == (1_000, 1, True)
    for expected in (2, 3):
        start, count, allowed = node_terminal.terminal_rate_state(
            1_000, start, count, window, maximum)
        assert (count, allowed) == (expected, True)
    _, _, allowed = node_terminal.terminal_rate_state(
        1_000, start, count, window, maximum)
    assert allowed is False
    # The window rolls, and a garbage attachment restarts it rather than
    # locking the socket out.
    start, count, allowed = node_terminal.terminal_rate_state(
        20_000, start, count, window, maximum)
    assert (start, count, allowed) == (20_000, 1, True)
    assert node_terminal.terminal_rate_state(
        5, "junk", None, window, maximum) == (5, 1, True)


def test_the_relay_forwards_to_the_opposite_role_and_keeps_nothing():
    relay = ENTRY.split("def _relay_terminal(self, ws, frame):", 1)[1][:2000]
    assert 'target = "node" if role == "browser" else "browser"' in relay
    assert "normalize_terminal_frame(frame, role)" in relay
    assert "self._live_node_sockets(role=target)" in relay
    # No storage, no D1, no replay: the object is a wire for these frames.
    for persistence in ("storage", "d1_run", "encrypt_row"):
        assert persistence not in relay


def test_socket_role_comes_from_how_the_account_was_proven():
    assert 'role = "browser" if parse_qs(parsed_url.query).get(' in ENTRY
    assert '"ticket", [""])[0] else "node"' in ENTRY
    # The role has to survive every attachment rewrite, or a browser socket
    # would read as a node after its first keepalive.
    assert '"role": _ws_attr(ws, "role", "node"),' in ENTRY
    assert 'NODE_FRAME_RELAYS = ("terminal", "control")' in ENTRY
    assert '"relays": list(NODE_FRAME_RELAYS), "role": role,' in ENTRY


def test_both_node_implementations_run_only_the_provider_login():
    # Go managed mirror.
    assert 'return "claude", []string{"auth", "login"}, true' in GO_TERMINAL
    assert 'return "codex", []string{"login"}, true' in GO_TERMINAL
    assert 'frame.Node != r.node' in GO_TERMINAL
    assert 'if frame.From != "browser"' in GO_EVENTS
    # Qt desktop.
    assert 'QStringLiteral("auth"), QStringLiteral("login")' in DESKTOP_PTY
    assert "kPublishAgentsToWebSetting" in DESKTOP_CHAT.split(
        "void MainWindow::onNodeTerminalFrame", 1)[1][:6000]
    assert "kNodeTerminalMaxLifetimeMs" in DESKTOP_CHAT


def test_the_stream_joins_the_object_the_named_machine_is_attached_to():
    # The relay can only forward between two sockets of ONE per-owner object,
    # and the node halves do not agree on which object that is: a desktop
    # signs its drain token as the account (nodes:<account>) while publishing
    # a machine name of its own, and a managed mirror is its own node account
    # (nodes:<mirror>). A tab that only ever joined its own account's object
    # therefore reached desktops and never mirrors, which is the
    # "<node> did not answer" the dialog reported (issue #681).
    assert 'if (nodeName) url.searchParams.set("node", nodeName);' in EVENTS_JS
    assert "nodeName: node," in TERMINAL_JS
    authorization = ENTRY.split(
        "async def _node_events_authorization(env, request):", 1)[1][:2500]
    assert 'requested = safe_segment(params.get("node", [""])[0]) or ""' in \
        authorization
    # Ownership first: the machine has to be one this account owns, which is
    # the same relation the agent target list is built from.
    assert "_account_owns_node(env, owner, requested)" in authorization
    assert '{"error": "forbidden"}, status=403' in authorization
    # Then presence: an owned machine that is not attached to its own object
    # is a desktop, and the tab stays on the account's object where that node
    # actually is.
    assert "_node_events_object_holds_node(env, requested)" in authorization
    # The node half proves an owner for itself and is never redirected.
    assert authorization.index("requested") < authorization.index(
        "_authorize_owner")


def test_the_presence_probe_counts_the_node_side_and_not_the_tabs():
    # A browser streaming a terminal holds a socket on the machine's object
    # too, so the role-blind total cannot answer "is the machine here?".
    snapshot = ENTRY.split(
        "# One-shot snapshot of the live socket counts", 1)[1][:700]
    assert '"nodes": len(live)' in snapshot
    assert '_ws_attr(peer, "role", "node") or "node") == "node"' in snapshot
    probe = ENTRY.split(
        "async def _node_events_object_holds_node(env, name):", 1)[1][:1200]
    assert '"agents"' in probe
    # A probe that cannot be read leaves the caller on the account's object
    # rather than refusing a sign-in outright.
    assert "except Exception:\n        return False" in probe


def test_the_website_button_opens_the_stream_and_keeps_the_desktop_handoff():
    assert "data-agent-modal-login-start" in HEADER
    assert "data-agent-modal-login-desktop" in HEADER
    assert "openAgentLoginTerminal" in DASHBOARD
    assert 'import("/agent-terminal.js")' in DASHBOARD
    assert "forkmesh://agent-login/" in DASHBOARD
    assert "data-agent-login-surface" in MODALS
    # The page half must speak exactly the actions the relay forwards.
    for action in node_terminal.BROWSER_ACTIONS:
        assert 'action: "%s"' % action in TERMINAL_JS
    for action in node_terminal.NODE_ACTIONS:
        assert 'frame.action === "%s"' % action in TERMINAL_JS
    # And it rides the account channel rather than a second bespoke socket.
    assert "onFrame(frame)" in EVENTS_JS
    assert "socket.send(JSON.stringify(frame))" in EVENTS_JS


def test_the_dialog_has_a_definite_height_the_stylesheet_actually_carries():
    """The terminal asks for as many rows as its box is tall.

    So the dialog must be sized by the viewport, never by its content: with a
    content-sized box every line the node draws makes the box taller, which
    makes the next ``measure()`` ask for more rows, which draws more lines —
    the window grows until it runs off the screen. ``dashboard/tailwind.css``
    is a checked-in artifact rather than a per-build compile, so an arbitrary
    utility written in the markup can be silently absent from it; these rules
    are hand-written in the shell's own stylesheet for that reason.
    """
    shell = (ROOT / "public" / "dashboard" / "shell.html").read_text(
        encoding="utf-8")
    for selector, declaration in (
        (".fm-agent-login-box", "height: min(680px, calc(100vh - 3rem));"),
        (".fm-agent-login-modal",
         "background: rgb(var(--dashboard-background-rgb) / 0.85);"),
        (".fm-agent-login-surface", "background: #0d0d10;"),
    ):
        assert selector in shell
        assert declaration in shell
        assert selector[1:] in MODALS
    # The utilities those rules replace resolved to nothing at all.
    assert "h-[min(680px,calc(100vh-3rem))]" not in MODALS
    assert "bg-background/85" not in MODALS
    assert "bg-[#0d0d10]" not in MODALS
