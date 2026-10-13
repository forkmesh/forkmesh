"""Durable six-stage Vultr mirror deployment UI contracts."""

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
CHAT = "".join(
    (ROOT / "desktop/src" / name).read_text()
    for name in MAIN_WINDOW_CHAT_SOURCES
)
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text()
CONTROL = (ROOT / "desktop/src/ControlNode.cpp").read_text()


def _slice(source, start, end):
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def test_vultr_deploy_page_has_six_numbered_stages_and_live_badge():
    page = _slice(
        CHAT,
        "QWidget *MainWindow::buildHostsSection()",
        "void MainWindow::refreshHostsTable()",
    )
    assert "kVultrProvisionStageCount = 6" in CHAT
    for label in (
        "Credentials",
        "Plan + image",
        "Create server",
        "Boot + connect",
        "Install",
        "Live traffic",
    ):
        assert f'QStringLiteral("{label}")' in page
    assert "Deployment stage %1" in page
    assert "vultrMirrorLiveBadge" in page
    assert "mirror is serving repository traffic" in CHAT


def test_checkpoint_is_durable_and_contains_no_credentials():
    persist = _slice(
        CHAT,
        "void MainWindow::persistVultrProvisionState",
        "void MainWindow::setVultrProvisionStage",
    )
    for field in (
        "state",
        "stage",
        "node",
        "dnsHostname",
        "instanceId",
        "ip",
        "identityFile",
        "installAgentClis",
        "hostMetadata",
        "updatedAt",
    ):
        assert f'QStringLiteral("{field}")' in persist
    assert "QSettings" in persist
    assert "settings.sync()" in persist
    for secret in (
        "apiKey",
        "cloudflareToken",
        "m_vultrTunnelApiToken",
        "VULTR_API_KEY",
    ):
        assert secret not in persist


def test_restart_restores_log_and_resumes_from_each_safe_boundary():
    restore = _slice(
        CHAT,
        "void MainWindow::restoreVultrProvision()",
        "void MainWindow::resumeVultrProvision()",
    )
    assert "vultrProvisionLogPath()" in restore
    assert "setPlainText" in restore
    assert "m_vultrResumeRequested" in restore
    assert "QTimer::singleShot(0, this, &MainWindow::resumeVultrProvision)" in restore

    resume = _slice(
        CHAT,
        "void MainWindow::resumeVultrProvision()",
        "void MainWindow::findVultrProvisionInstance",
    )
    assert "m_vultrProvisionStage >= 6" in resume
    assert "seedVultrMirrorRepositoryThenVerify" in resume
    assert "m_vultrProvisionStage >= 5" in resume
    assert "startVultrHostInstall" in resume
    assert "pollVultrInstance" in resume
    assert "m_vultrResumeChain = true" in resume
    assert "createVultrMirrorFromForm()" in resume


def test_restored_retry_blocks_autoscaling_before_and_after_catalog_reply():
    restore = _slice(
        CHAT,
        "void MainWindow::restoreVultrProvision()",
        "void MainWindow::resumeVultrProvision()",
    )
    assert (
        'm_vultrProvisionState == QLatin1String("active") || failed'
        in restore
    )

    reconcile = _slice(
        CHAT,
        "void MainWindow::reconcileDesiredMirrorFleet()",
        "void MainWindow::destroyDesiredMirrorFleetNode(",
    )
    guard = "vultrProvisionBlocksFleetReconciliation("
    assert reconcile.count(guard) == 2
    first_guard = reconcile.index(guard)
    request = reconcile.index("m_networkAccess->get(request)")
    reply = reconcile.index("QNetworkReply::finished")
    second_guard = reconcile.index(guard, first_guard + len(guard))
    create = reconcile.index("createVultrMirrorFromForm()")
    assert first_guard < request
    assert reply < second_guard < create


def test_restart_cannot_duplicate_a_billable_instance():
    lookup = _slice(
        CHAT,
        "void MainWindow::findVultrProvisionInstance",
        "void MainWindow::finishVultrProvision",
    )
    assert 'QStringLiteral("/v2/instances?per_page=500")' in lookup
    assert 'QStringLiteral("label")' in lookup
    assert 'QStringLiteral("hostname")' in lookup

    create = _slice(
        CHAT,
        "void MainWindow::createVultrMirrorFromForm()",
        "void MainWindow::pollVultrInstance",
    )
    lookup_at = create.index("findVultrProvisionInstance(")
    post_at = create.index('QStringLiteral("/v2/instances")', lookup_at)
    assert lookup_at < post_at
    assert "resuming instead of creating a duplicate" in create


def test_log_is_bounded_and_saved_while_deployment_runs():
    save = _slice(
        CHAT,
        "void MainWindow::saveVultrProvisionLog()",
        "void MainWindow::scheduleVultrProvisionLogSave()",
    )
    assert "1024 * 1024" in save
    assert "QSaveFile" in save
    assert "toPlainText()" in save
    append = _slice(
        CHAT,
        "void MainWindow::appendHostInstallLog",
        "void MainWindow::appendHostDeployLog",
    )
    assert "m_vultrProvisionActive" in append
    assert "scheduleVultrProvisionLogSave()" in append


def test_deployment_transcript_has_zoned_milestones_and_attempt_duration():
    helpers = _slice(
        CHAT,
        "QString hostInstallEventTimestamp()",
        "// Shared by the detached writers below",
    )
    assert "Qt::ISODate" in helpers
    assert 'QStringLiteral("[%1] %2")' in helpers
    assert "QString conciseElapsedTime" in helpers

    create = _slice(
        CHAT,
        "void MainWindow::createVultrMirrorFromForm()",
        "void MainWindow::pollVultrInstance",
    )
    for milestone in (
        "Creating Vultr mirror",
        "Cheapest supported plan:",
        "Operating system:",
        "Instance %1 created",
    ):
        location = create.index(milestone)
        assert (
            "timestampedHostInstallEvent" in create[max(0, location - 1000) : location]
        )

    boot = _slice(
        CHAT,
        "void MainWindow::pollVultrInstance",
        "void MainWindow::appendVultrAttemptHistory",
    )
    assert "timestampedHostInstallEvent" in boot
    assert "Instance is up at %1" in boot
    assert "SSH answered on %1" in boot

    install = _slice(
        CHAT,
        "void MainWindow::startVultrHostInstall",
        "namespace {\nint installLogAnsiFg",
    )
    assert "Attempt %1 of %2 started at %3" in install
    assert "attemptTimer->elapsed()" in install
    assert "finished at %2 after %3" in install


def test_green_live_state_requires_real_public_traffic_health():
    verify = _slice(
        CHAT,
        "void MainWindow::waitForVultrMirrorPublication",
        "void MainWindow::vultrApiCall",
    )
    assert 'url.setPath(QStringLiteral("/health"))' in verify
    assert 'healthQuery.addQueryItem(QStringLiteral("nonce")' in verify
    assert 'healthQuery.addQueryItem(QStringLiteral("issuedAt")' in verify
    assert "verifiedFlagshipMirrorHealth(" in verify
    assert "No relay " in verify
    assert '"Worker was used.\\n"' in verify
    assert "finishVultrProvision(true" in verify
    # Verification is bounded at both ends: it slows to a minute per poll after
    # the fast phase, then ends the deployment rather than retrying forever.
    assert "finishVultrProvision(false" in verify
    assert '"ended after thirty minutes.' in verify
    assert "60000" in verify
    assert 'QStringLiteral("endpoint"))' not in verify


def test_header_declares_durable_ui_and_resume_state():
    for symbol in (
        "setVultrProvisionStage",
        "persistVultrProvisionState",
        "restoreVultrProvision",
        "resumeVultrProvision",
        "findVultrProvisionInstance",
        "m_vultrStageNumbers",
        "m_vultrLiveBadge",
        "m_vultrProvisionStage",
        "m_vultrInstanceId",
        "m_vultrInstanceIp",
    ):
        assert symbol in HEADER


def test_each_vultr_deploy_stage_reports_one_ping_outcome():
    ping = _slice(
        CHAT,
        "void MainWindow::pingVultrProvisionStage",
        "void MainWindow::restoreVultrProvision",
    )
    for label in (
        "Credentials",
        "Plan + image",
        "Create server",
        "Boot + connect",
        "Install",
        "Live traffic",
    ):
        assert f'QStringLiteral("{label}")' in ping
    assert "addNotification(" in ping
    assert "Vultr mirror deployment" in ping

    transition = _slice(
        CHAT,
        "void MainWindow::setVultrProvisionStage",
        "void MainWindow::pingVultrProvisionStage",
    )
    assert "m_vultrProvisionStage > previousStage" in transition
    assert "pingVultrProvisionStage(previousStage, true" in transition
    assert "pingVultrProvisionStage(m_vultrProvisionStage, false" in transition

    finish = _slice(
        CHAT,
        "void MainWindow::finishVultrProvision",
        "void MainWindow::waitForVultrMirrorPublication",
    )
    assert "pingVultrProvisionStage(m_vultrProvisionStage, ok, message)" in finish

    install = _slice(
        CHAT,
        "void MainWindow::startVultrHostInstall",
        "namespace {\nint installLogAnsiFg",
    )
    assert "setVultrProvisionStage(\n        5" in install
