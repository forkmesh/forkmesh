#!/usr/bin/env python3
"""Launch-time federation join ping (adhoc #97).

A freshly launched instance immediately registers with its configured main
relay via POST /api/federation/announce (instead of waiting for the first
staggered cron), and the admin's signed heartbeat reply carries the pending
join-request count that lights the desktop's red dot + Approve button.
"""

import ast
import asyncio
from pathlib import Path
from types import SimpleNamespace


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
ENTRY = ROOT / "src" / "entry.py"
SOURCE = (
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
QT_SETUP = ROOT.parent / "desktop" / "src" / "MainWindowSetup.cpp"
QT_CHAT = tuple(
    ROOT.parent / "desktop" / "src" / name for name in MAIN_WINDOW_CHAT_SOURCES
)
BOOTSTRAP = ROOT.parent / "tools" / "cloudflare_bootstrap.py"
NOW = 1_700_000_000_000


def run(value):
    return asyncio.run(value)


def _load(*, main_relay, register_reply, calls, last_announce=0):
    tree = ast.parse(SOURCE, filename=str(ENTRY))
    nodes = [
        node for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name == "_federation_announce"
    ]
    assert len(nodes) == 1

    async def call_main_relay(_env, path, body):
        calls.append((path, body))
        return register_reply

    def response(data, status=200, cache_control=None, extra_headers=None):
        return {
            "data": data,
            "status": status,
            "cacheControl": cache_control,
            "headers": extra_headers or {},
        }

    namespace = {
        "_is_main_relay": lambda _env: main_relay,
        "_call_main_relay": call_main_relay,
        "clean_string": lambda value, limit: str(value or "")[:limit],
        "json_response": response,
        "Date": SimpleNamespace(now=lambda: NOW),
        "_FEDERATION_ANNOUNCE_MIN_MS": 60 * 1000,
        "_federation_announce_at": [last_announce],
        "EXPECTED_DEGRADED_HEADERS": {"x-expected-degraded": "1"},
    }
    exec(compile(ast.fix_missing_locations(
        ast.Module(body=nodes, type_ignores=[])), str(ENTRY), "exec"),
        namespace)
    return namespace


def test_main_relay_does_not_announce():
    calls = []
    module = _load(main_relay=True, register_reply={"ok": True}, calls=calls)
    result = run(module["_federation_announce"](object(), object()))
    assert result["status"] == 404
    assert calls == []


def test_fresh_instance_pings_main_relay_register_as_join_request():
    calls = []
    env = SimpleNamespace(
        RELAY_LABEL="Acme Commons",
        PUBLIC_BASE_URL="https://mesh.example.com",
    )
    module = _load(
        main_relay=False,
        register_reply={"ok": True, "status": "pending"},
        calls=calls,
    )
    result = run(module["_federation_announce"](env, object()))
    assert result["status"] == 200
    assert result["data"] == {"ok": True, "status": "pending"}
    assert result["cacheControl"] == "no-store"
    assert calls == [(
        "/api/federation/register",
        {"label": "Acme Commons", "baseUrl": "https://mesh.example.com"},
    )]


def test_unreachable_main_relay_is_a_502():
    calls = []
    module = _load(main_relay=False, register_reply=None, calls=calls)
    result = run(module["_federation_announce"](SimpleNamespace(), object()))
    assert result["status"] == 502
    assert result["headers"] == {"x-expected-degraded": "1"}


def test_announce_is_throttled_per_isolate():
    calls = []
    module = _load(
        main_relay=False,
        register_reply={"ok": True, "status": "pending"},
        calls=calls,
        last_announce=NOW - 1_000,
    )
    result = run(module["_federation_announce"](SimpleNamespace(), object()))
    assert result["status"] == 200
    assert result["data"] == {"ok": True, "status": "throttled"}
    assert calls == []


def test_route_and_heartbeat_wiring():
    # The announce route is dispatched by the federation handler (which also
    # serves it under the canonical /api/relay-mesh/* alias)...
    assert 'if url.path == "/api/federation/announce":' in SOURCE
    assert "return await _federation_announce(env, request)" in SOURCE
    # ...and the admin heartbeat reply carries the pending join-request count
    # (main relay only) that lights the desktop's red dot + Approve button.
    assert "if is_admin and _is_main_relay(env):" in SOURCE
    assert "SELECT COUNT(*) AS n FROM relays WHERE status='pending'" in SOURCE
    assert 'response["pendingRelays"]' in SOURCE


def test_wire_contracts_across_worker_qt_and_bootstrap():
    qt_setup = QT_SETUP.read_text(encoding="utf-8")
    qt_chat = "".join(path.read_text(encoding="utf-8") for path in QT_CHAT)
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")

    # The launch bootstrap pings the fresh instance's announce endpoint so its
    # join request reaches the main relay the moment it launches.
    assert "/api/federation/announce" in bootstrap
    assert "announce: Callable[[str], str] = _announce_join" in bootstrap

    # The desktop reads the heartbeat's pending count and drives the red dot
    # over the relay favicon plus the Approve button beside it.
    assert 'resp.value(QStringLiteral("pendingRelays")).toInt()' in qt_setup
    assert "setPendingRelayJoins" in qt_setup
    assert "m_relayJoinDot = new QLabel(m_relayMenuButton);" in qt_chat
    assert "m_relayJoinApproveButton" in qt_chat

    # The signed admin canonicals match on both ends.
    assert '"forkmesh-admin-relays-v1\\n" + node + "\\n" + ts' in SOURCE
    assert '("forkmesh-admin-relays-v1\\n" + node + "\\n" + ts)' in qt_setup
    assert ('"forkmesh-admin-relay-approve-v1\\n" + node + "\\n" + pubkey +'
            in SOURCE)
    assert ('"forkmesh-admin-relay-approve-v1\\n" + node + "\\n" + pubkey +'
            in qt_setup)
