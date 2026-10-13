"""Desktop mirror access stays direct-first and client-only."""

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

ROOT = Path(__file__).resolve().parents[2]
RELEASES = (ROOT / "desktop/src/MainWindowReleases.cpp").read_text()
PULLS = (ROOT / "desktop/src/MainWindowPulls.cpp").read_text()
CHAT = "".join(
    (ROOT / "desktop/src" / name).read_text()
    for name in MAIN_WINDOW_CHAT_SOURCES
)
MAIN = (ROOT / "desktop/src/main.cpp").read_text()
CMAKE = (ROOT / "desktop/CMakeLists.txt").read_text()


def test_mirror_status_uses_saved_hosts_and_signed_direct_health_only():
    assert "loadSavedHosts" in RELEASES
    assert 'setPath(QStringLiteral("/health"))' in RELEASES
    assert "verifiedRepositoryHealth" in RELEASES
    assert "ForkMeshIdentity::verifySignature" in RELEASES
    assert "fetchCatalogMirrorsFromWorker" not in RELEASES
    assert "fetchMirrorReachability" not in RELEASES
    assert "/reachability" not in RELEASES
    assert "fetchMirrorPendingCounts" not in RELEASES
    assert 'QStringLiteral("refsSha256")' in RELEASES
    assert 'QStringLiteral("publicRepositoryCount")' in RELEASES
    assert 'QStringLiteral("/v1/public/repositories/%1/%2/branches")' in RELEASES
    assert "const bool refsBehind" in RELEASES
    assert "const bool commitBehind" in RELEASES
    assert "const bool behind = refsBehind || commitBehind" in RELEASES
    assert 'QStringLiteral("Online now")' in RELEASES
    assert 'QString::fromUtf8("Online \\xC2\\xB7 out of sync")' in RELEASES
    assert 'QStringLiteral("Verified")' in RELEASES
    assert "panelRefresh" not in RELEASES
    assert "No repair timer" in RELEASES


def test_pull_ledger_tries_direct_ssh_before_relay_fallback():
    start = PULLS.index("void MainWindow::reconcilePullLedgerFor(")
    end = PULLS.index("void MainWindow::sweepPullChecksFor(", start)
    reconcile = PULLS[start:end]
    direct = reconcile.index("remoteUrls.append(candidate)")
    relay = reconcile.index("remoteUrls.append(relayRemoteUrl)", direct)
    fetch = reconcile.index(
        "for (const QString &remoteUrl : remoteUrls)", relay
    )
    assert direct < relay < fetch


def test_pull_ledger_remote_discovery_runs_on_the_worker():
    start = PULLS.index("void MainWindow::reconcilePullLedgerFor(")
    end = PULLS.index("void MainWindow::sweepPullChecksFor(", start)
    reconcile = PULLS[start:end]
    dispatch = reconcile.index("runOffThread<PullLedgerFetch>(")
    worker = reconcile[dispatch:]

    assert "runGitCapture(" not in reconcile[:dispatch]
    assert "runGitCapture(remoteConfigPath" in worker
    assert worker.index("runGitCapture(remoteConfigPath") < worker.index(
        "prefetchForeignLedger(remoteUrl)"
    )
    assert "[store, remoteConfigPath, activeHosts, relayRemoteUrl]" in worker


def test_managed_fleet_health_is_direct_and_signed():
    fleet = CHAT[
        CHAT.index("void MainWindow::reconcileDesiredMirrorFleet()"):
        CHAT.index("void MainWindow::adoptSavedMirrorVultrInstances")
    ]
    assert 'url.setPath(QStringLiteral("/health"))' in fleet
    assert "verifiedFlagshipMirrorHealth" in fleet
    assert "/api/repo/forkmesh/forkmesh/mirrors" not in fleet
    assert "public health catalog" not in fleet


def test_desktop_has_no_repository_server_or_headless_launch_mode():
    assert "src/RepoHost.cpp" not in CMAKE
    assert "src/MainWindowMirrorNode.cpp" not in CMAKE
    assert "src/HeadlessConsole.cpp" not in CMAKE
    assert 'QStringLiteral("--headless")' not in MAIN
    assert 'QStringLiteral("--cli")' not in MAIN
    assert 'QStringLiteral("--verify-mirror-capability")' not in MAIN
    assert 'QStringLiteral("--sign-mirror-health")' not in MAIN
