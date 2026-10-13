#!/usr/bin/env python3
"""Contracts for the live "a new member just joined" celebration.

A completed signup rings two rooms the outer Worker can reach: the World
presence Durable Object and the shared public #general chat room. Everyone
already holding a socket — the 3D World, the website chat pages, the World's
embedded chat panel, and desktop nodes — gets fireworks and confetti, and the
web clients write "<name> just joined ForkMesh. Say hello in #general." into
their chat feed. There is no announcement card: no toasts, the text is shown
inline in the feed.

The frame is the one thing the relay itself originates, so the security shape
matters: it is HMAC-signed by the outer Worker (no browser holds that secret),
it expires within a minute so a captured frame cannot replay the party, and it
carries a single public account name that every client re-validates before it
reaches the screen.
"""

import ast
import asyncio
import hmac
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

ROOT = Path(__file__).resolve().parents[2] / "app"
ENTRY = ROOT / "src" / "entry.py"
WORLD_DURABLE_OBJECTS = (
    ROOT.parent / "world" / "src" / "world_durable_objects.py"
)
WORLD_DIR = ROOT.parent / "world" / "public" / "world"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
WORLD_DURABLE_OBJECTS_TEXT = WORLD_DURABLE_OBJECTS.read_text(encoding="utf-8")
APP = (WORLD_DIR / "world.js").read_text(encoding="utf-8")
WORLD_CSS = (WORLD_DIR / "world.css").read_text(encoding="utf-8")
WELCOME_JS = (ROOT / "public" / "member-welcome.js").read_text(encoding="utf-8")
DASHBOARD_CHAT = (
    ROOT / "public" / "dashboard-chat.js").read_text(encoding="utf-8")
CHAT_JS = (ROOT / "public" / "chat.js").read_text(encoding="utf-8")
DESKTOP = ROOT.parent / "desktop" / "src"
SERVER_NODE = (DESKTOP / "ServerNode.cpp").read_text(encoding="utf-8")
MAIN_WINDOW_CHAT = "".join(
    (DESKTOP / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
MAIN_WINDOW_INTERNAL = (
    DESKTOP / "MainWindowInternal.h").read_text(encoding="utf-8")
MAIN_WINDOW_SETTINGS = (
    DESKTOP / "MainWindowSettings.cpp").read_text(encoding="utf-8")


def _load(*names, extra_globals=None):
    trees = (
        ast.parse(ENTRY_TEXT, filename=str(ENTRY)),
        ast.parse(
            WORLD_DURABLE_OBJECTS_TEXT, filename=str(WORLD_DURABLE_OBJECTS)),
    )
    selected = [
        node for tree in trees for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))
        and node.name in names
    ]
    assert {node.name for node in selected} == set(names)
    namespace = dict(extra_globals or {})
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=selected, type_ignores=[])), str(ENTRY), "exec"),
         namespace)
    return namespace


def _run(value):
    return asyncio.run(value)


def _method_body(text, header):
    """Source of one method, cut at the next method rather than a byte count.

    A fixed-size slice silently reaches into whatever follows, which turns
    "this method never touches X" into a claim about its neighbours.
    """
    start = text.index(header)
    body = text[start + len(header):]
    ends = [
        offset for offset in (body.find("\n    def "), body.find("\n    async def "))
        if offset >= 0
    ]
    return header + (body[:min(ends)] if ends else body)


class _Headers:
    def __init__(self, values):
        self.values = {
            str(key).lower(): str(value) for key, value in values.items()
        }

    def get(self, name, default=None):
        return self.values.get(str(name).lower(), default)


class _Request:
    def __init__(self, data, headers=None, method="POST"):
        self._data = data
        self.headers = _Headers(headers or {})
        self.method = method
        self.url = "https://forkmesh.internal/api/world/new-member"

    async def json(self):
        return self._data


def _response(payload, status=200, **kwargs):
    return {"payload": payload, "status": status, **kwargs}


async def _bounded_json(request, max_bytes=None):
    return await request.json()


NOW = 1_700_000_000_000


def _valid_node_name(value):
    import re as re_module
    return bool(re_module.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?",
                                    str(value or "")))


def _runtime(now=NOW):
    import re as re_module
    return _load(
        "ForkMeshWorld", "_new_member_signature", "_new_member_announcement",
        extra_globals={
            "DurableObject": type("DurableObject", (), {}),
            "_account_session_secret": lambda env: b"unit-secret",
            "method_name": lambda request: request.method,
            "json_response": _response,
            "bounded_json_request": _bounded_json,
            "clean_string": lambda value, limit: str(value or "")[:limit],
            "valid_node_name": _valid_node_name,
            "MAX_NODE_NAME": 64,
            "NEW_MEMBER_ANNOUNCE_MAX_AGE_MS": 60 * 1000,
            "Date": type("Date", (), {"now": staticmethod(lambda: now)}),
            "re": re_module,
            "hmac": hmac,
        })


def _world(now=NOW):
    runtime = _runtime(now)
    instance = runtime["ForkMeshWorld"]()
    instance.env = None
    frames = []
    instance._broadcast = lambda frame, **_kwargs: frames.append(frame)
    return instance, runtime["_new_member_signature"], frames


def test_signed_signup_notice_reaches_every_visitor_in_the_world():
    instance, sign, frames = _world()
    response = _run(instance._new_member(_Request(
        {"name": "riverwren", "at": NOW},
        headers={"x-forkmesh-new-member": sign(None, "riverwren", NOW)})))
    assert response["status"] == 200
    assert frames == [{"type": "new-member", "name": "riverwren"}]


def test_forged_stale_or_malformed_welcomes_never_reach_the_world():
    instance, sign, frames = _world()
    rejected = [
        # No browser holds the control secret, so a made-up signature loses.
        _Request({"name": "riverwren", "at": NOW},
                 headers={"x-forkmesh-new-member": "f" * 64}),
        # A signature is bound to one name...
        _Request({"name": "someone-else", "at": NOW},
                 headers={"x-forkmesh-new-member": sign(
                     None, "riverwren", NOW)}),
        # ...and to one moment: a captured frame cannot re-run the party.
        _Request({"name": "riverwren", "at": NOW - 61_000},
                 headers={"x-forkmesh-new-member": sign(
                     None, "riverwren", NOW - 61_000)}),
        # A name that is not a real handle cannot ride the frame even signed.
        _Request({"name": "Not A Handle", "at": NOW},
                 headers={"x-forkmesh-new-member": sign(
                     None, "Not A Handle", NOW)}),
        _Request({"name": "", "at": NOW},
                 headers={"x-forkmesh-new-member": sign(None, "", NOW)}),
        _Request({"name": "riverwren"},
                 headers={"x-forkmesh-new-member": sign(None, "riverwren", 0)}),
    ]
    for request in rejected:
        assert _run(instance._new_member(request))["status"] == 401
    assert _run(instance._new_member(
        _Request({}, method="GET")))["status"] == 405
    assert frames == []


def test_signature_binds_the_name_and_the_moment():
    _instance, sign, _frames = _world()
    base = sign(None, "riverwren", NOW)
    assert base == sign(None, "riverwren", NOW)
    assert base != sign(None, "riverwren2", NOW)
    assert base != sign(None, "riverwren", NOW + 1)


def test_the_chat_room_relays_the_same_notice_without_retaining_it():
    # The room Durable Object lives in entry.py next to the World's copy and
    # shares one verifier, so the two rooms cannot drift apart on what counts
    # as a signed welcome.
    assert '"/api/chat/new-member"' in ENTRY_TEXT
    assert "return await self._new_member(request)" in ENTRY_TEXT
    block = _method_body(
        ENTRY_TEXT, "    async def _new_member(self, request):\n"
        "        \"\"\"Relay a signed")
    assert "_new_member_announcement(" in block
    assert '"kind": "forkmesh-new-member"' in block
    # Live-only: the announcement is never handed to the retention path, so it
    # does not replay into every later visitor's history.
    # The call form, so the docstring explaining the choice does not satisfy it.
    assert "_maybe_retain(" not in block
    assert "self._live_chat_sockets(close_stale=True)" in block


def test_only_a_genuinely_new_account_sets_off_the_celebration():
    signup = ENTRY_TEXT[ENTRY_TEXT.index("async def _account_signup("):]
    signup = signup[:signup.index("\nasync def _account_password_record(")]
    assert (
        'is_new_member = not existing or existing.get("status") != "active"'
        in signup
    )
    assert "await _broadcast_new_member(env, name)" in signup
    # The account is saved before the party, and a relay failure never turns a
    # good signup into an error.
    assert signup.index("await _save_account(") < signup.index(
        "await _broadcast_new_member(")
    tail = signup[signup.index("if is_new_member:"):]
    assert "except Exception:" in tail[:400]


def test_the_announcement_fans_out_to_both_rooms_from_one_signature():
    broadcast = ENTRY_TEXT[
        ENTRY_TEXT.index("async def _broadcast_new_member(env, name):"):]
    broadcast = broadcast[:broadcast.index("\ndef _new_member_announcement(")]
    assert '"FORKMESH_WORLD", "town-square-v1"' in broadcast
    assert '"FORKMESH_MAINNODE_ROOM", PUBLIC_WORLD_GENERAL_ROOM_KEY' in broadcast
    assert '"x-forkmesh-new-member": signature' in broadcast
    # One signature covers both legs, and a dead Durable Object on one leg
    # never stops the other.
    assert broadcast.count("_new_member_signature(") == 1
    assert "except Exception:\n            continue" in broadcast


def test_world_visitors_get_fireworks_confetti_and_a_chat_feed_greeting():
    assert 'message.type === "new-member"' in APP
    assert "this.celebrateNewMember(message)" in APP
    handler = APP[APP.index("  celebrateNewMember(message) {"):]
    handler = handler[:handler.index("\n  }\n")]
    # The relayed name is sanitized again before it can reach the screen.
    assert "sanitizePresenceText(message?.name" in handler
    assert "world-instance-fireworks" in handler
    assert "world-member-confetti" in handler
    assert "world-instance-announcement" not in handler
    assert (
        "this.notifyChatArea(`🎉 ${name} just joined ForkMesh. "
        'Say hello in #general.`, "status");'
    ) in handler
    # One party per newcomer even inside the World, where the presence socket
    # and the embedded chat panel's room socket both carry the same news: the
    # two celebrations share a marker and whichever arrives first keeps it.
    assert "layer.dataset.forkmeshMemberWelcome = name" in handler
    assert '"[data-forkmesh-member-welcome]"' in handler
    assert "existing?.dataset.forkmeshMemberWelcome === name" in handler
    # Motion is opt-out, and the party clears itself off the screen.
    assert '"(prefers-reduced-motion: reduce)"' in handler
    assert "layer.remove();" in handler
    assert ".world-member-confetti {" in WORLD_CSS
    assert "@keyframes world-member-confetti-fall {" in WORLD_CSS
    reduced = WORLD_CSS[WORLD_CSS.index(
        "@media (prefers-reduced-motion: reduce) {"):]
    assert ".world-member-confetti {\n    display: none;\n  }" in reduced


def test_chat_clients_share_one_celebration_and_cannot_render_a_name_as_markup():
    # Every chat surface renders the same overlay from one module.
    assert "export function celebrateNewMember(" in WELCOME_JS
    assert 'kind === "forkmesh-new-member"' in DASHBOARD_CHAT
    assert "celebrateNewMember(envelope.name)" in DASHBOARD_CHAT
    assert 'import("./member-welcome.js")' in DASHBOARD_CHAT
    assert 'from "./member-welcome.js"' in CHAT_JS
    assert 'kind === "forkmesh-new-member"' in CHAT_JS
    # A relayed handle is re-validated, and only ever set as text.
    assert "HANDLE_RE.test(name)" in WELCOME_JS
    assert "innerHTML" not in WELCOME_JS
    assert "fm-member-welcome-card" not in WELCOME_JS
    assert "just joined ForkMesh. Say hello in #general." in WELCOME_JS
    assert "if (celebrateNewMember(envelope.name)) {" in CHAT_JS
    assert "if (!module?.celebrateNewMember?.(name)) return;" in DASHBOARD_CHAT
    assert "module.NEW_MEMBER_GREETING(" in DASHBOARD_CHAT
    assert "fm-spark" in WELCOME_JS and "fm-flake" in WELCOME_JS
    # One party per newcomer: the World's copy and the chat copy share a marker.
    assert '"[data-forkmesh-member-welcome]"' in WELCOME_JS
    assert "prefers-reduced-motion: reduce" in WELCOME_JS


def test_the_new_module_is_proxied_from_the_app_worker():
    worker = (ROOT.parent / "world" / "worker.js").read_text(
        encoding="utf-8"
    )
    wrangler = (ROOT.parent / "world" / "wrangler.toml").read_text(
        encoding="utf-8"
    )
    targets = (ROOT / "tools" / "deploy_targets.py").read_text(
        encoding="utf-8"
    )
    # dashboard-chat.js imports this relative URL from the World's origin. The
    # World keeps that same-origin URL but service-proxies the App-owned bytes.
    assert '"/member-welcome.js",' in worker
    assert '"/member-welcome.js",' in wrangler
    assert '"app/public/member-welcome.js",' not in targets


def test_desktop_nodes_celebrate_from_the_same_relayed_frame():
    # The desktop holds the same public #general room socket, so the frame
    # arrives there too. It is the one plaintext frame the client accepts, and
    # the handle is validated exactly as the Worker validates it.
    assert '"forkmesh-new-member"' in SERVER_NODE
    assert "^[a-z0-9](?:[a-z0-9-]{0,38}[a-z0-9])?$" in SERVER_NODE
    assert "emit newMemberJoined(name)" in SERVER_NODE
    assert "&ChatBackend::newMemberJoined" in MAIN_WINDOW_SETTINGS
    handler = MAIN_WINDOW_CHAT[
        MAIN_WINDOW_CHAT.index("void MainWindow::onNewMemberJoined("):]
    handler = handler[:handler.index("\n}\n")]
    assert "forkmesh::ui::CelebrationOverlay(this)" in handler
    assert "Welcome %1" in handler
    assert 'QStringLiteral("fm:chat:general")' in handler
    # ...and that toast link actually lands in #general.
    link = MAIN_WINDOW_SETTINGS[
        MAIN_WINDOW_SETTINGS.index(
            '    if (href == QLatin1String("fm:chat:general")) {'):]
    link = link[:link.index("return true;")]
    assert "showChatView();" in link
    assert 'switchConversation(QStringLiteral("#general"));' in link
    overlay = MAIN_WINDOW_INTERNAL[
        MAIN_WINDOW_INTERNAL.index("class CelebrationOverlay final"):]
    overlay = overlay[:overlay.index("\nclass RingSpinner")]
    # Click-through, focus-free, and self-disposing: a celebration must never
    # become something the user has to dismiss to keep working.
    assert "Qt::WA_TransparentForMouseEvents" in overlay
    assert "setFocusPolicy(Qt::NoFocus)" in overlay
    assert "deleteLater()" in overlay
