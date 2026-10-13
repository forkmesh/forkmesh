#!/usr/bin/env python3
"""Managed mirror bootstrap contracts after the desktop-server retirement."""

import ast
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ENTRY = ROOT / "app" / "src" / "entry.py"
BOOTSTRAP = ROOT / "server" / "bootstrap.go"
DAEMON = ROOT / "server" / "daemon.go"
INTAKE = ROOT / "server" / "intake.go"


def test_desktop_has_no_headless_server_bootstrap_or_catalog_publisher():
    desktop = ROOT / "desktop"
    cmake = (desktop / "CMakeLists.txt").read_text(encoding="utf-8")
    main = (desktop / "src" / "main.cpp").read_text(encoding="utf-8")
    sources = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in (desktop / "src").glob("*.cpp")
    )

    assert not (desktop / "src" / "RepoHost.cpp").exists()
    assert not (desktop / "src" / "HeadlessConsole.cpp").exists()
    assert "RepoHost.cpp" not in cmake
    assert 'QStringLiteral("--headless")' not in main
    assert "void MainWindow::publishRepositoryNow" not in sources
    assert "void MainWindow::startRepoHosts" not in sources


def test_go_service_bootstraps_canonical_and_node_aliases_and_native_intake():
    bootstrap = BOOTSTRAP.read_text(encoding="utf-8")
    daemon = DAEMON.read_text(encoding="utf-8")
    intake = INTAKE.read_text(encoding="utf-8")

    assert "A mirror is another origin for the canonical repository" in bootstrap
    assert "repository := Repository{Owner: options.Owner" in bootstrap
    assert '"canonicalOwner": options.Owner' in bootstrap
    assert "options.Node + \"/\" + options.Repository" not in bootstrap
    assert '"catalogUrl": options.CatalogURL' in bootstrap
    assert "isCollaborationKind(event.Topic)" in daemon
    assert "drainCollaborationKind" in daemon
    assert "materializeIssueInbox" in intake
    assert "materializePullInbox" in intake
    assert "materializeDiscussionInbox" in intake
    assert "IntakeBridge" not in daemon + intake
    assert "--headless" not in daemon + intake


def test_public_browse_failover_walks_healthy_direct_https_endpoints():
    entry = ENTRY.read_text(encoding="utf-8")
    tree = ast.parse(entry, filename=str(ENTRY))
    proxy = next(
        node for node in tree.body
        if isinstance(node, ast.AsyncFunctionDef)
        and node.name == "_https_mirror_proxy"
    )
    body = ast.unparse(proxy)

    assert "pending = list(candidates)" in body
    assert "_https_mirror_repository_proof(" in body
    assert "status in HTTPS_MIRROR_RETRY_STATUSES" in body
    assert "_https_mirror_route_advance(" in body


def test_room_stale_sockets_are_not_counted_or_forwarded():
    entry = ENTRY.read_text(encoding="utf-8")
    assert "ROOM_CLIENT_STALE_MS = 3 * 60 * 1000" in entry
    tree = ast.parse(entry)
    room = next(
        node for node in tree.body
        if isinstance(node, ast.ClassDef) and node.name == "ForkMeshRoom"
    )
    body = ast.unparse(room)

    assert "'last': int(Date.now())" in body
    assert "return len(self._live_chat_sockets(close_stale=True))" in body
    assert "def _live_chat_sockets(self, close_stale=False):" in body
    assert "self._safe_close(peer, 1001, 'stale')" in body
    assert "for peer in self._live_chat_sockets(close_stale=True):" in body


def test_public_account_row_reads_only_the_identity_tables():
    entry = ENTRY.read_text(encoding="utf-8")
    start = entry.index("async def _account_row")
    end = entry.index("async def _owner_pubkey", start)
    body = entry[start:end]

    assert "await _account_identity_rec_by_bi(env, name_bi)" in body
    assert "_mirror_account_identity_tables" not in body
    assert "FROM accounts" not in body
