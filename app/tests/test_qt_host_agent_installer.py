"""Contracts for the Hosts-tab Claude Code/Codex mirror installer."""

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
    (ROOT / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")
# The remote installer script itself lives in the pure control-node helper, so
# the Hosts button and the one-click Vultr flow cannot drift apart (adhoc #418).
CONTROL = (ROOT / "desktop/src/ControlNode.cpp").read_text(encoding="utf-8")
INSTALLER = (ROOT / "www/public/install.sh").read_text(
    encoding="utf-8"
)


def test_hosts_table_has_a_selected_host_agent_cli_install_action():
    assert "hostInstallAgentClisButton" in CHAT
    assert "Install Claude + Codex" in CHAT
    assert "installAgentClisForHost(i)" in CHAT
    assert "void MainWindow::installAgentClisForHost(int row)" in CHAT
    assert "m_hostAgentInstallProcess" in HEADER
    assert 'QStringLiteral("Claude"), QStringLiteral("Codex")' in CHAT


def test_agent_install_uses_pinned_ssh_and_official_user_scoped_installers():
    installer = CHAT.split(
        "void MainWindow::installAgentClisForHost(int row)", 1
    )[1][:12000]
    assert "forkmesh::control::buildHostSshCommand" in installer
    assert "savedHostIdentityFile(node, ip, user)" in installer
    assert "forkmesh::control::agentCliBootstrapRemoteCommand" in installer
    assert "does not copy tokens" in CHAT
    # The whole fleet authorizes one shared key, so it is always offered; a
    # session password is only ssh's fallback when that key is refused.
    assert "const QString sshPassword = pass;" in installer
    assert "Using the shared ForkMesh SSH identity" in installer
    assert "SSH could not reach %1 on port 22" in installer
    assert "The mirror rejected the shared ForkMesh SSH " in installer
    assert '"key. Add that key to %1\'s authorized_keys, "' in installer
    # The per-host button installs binaries only; it never copies a login.
    assert "/*copyCredentials=*/false" in installer

    script = CONTROL.split(
        "QString agentCliBootstrapRemoteCommand(bool withCredentials)", 1
    )[1][:6000]
    assert "https://claude.ai/install.sh" in script
    assert "https://chatgpt.com/codex/install.sh" in script
    assert "claude --version" in script
    assert "codex --version" in script
    # Split over two source lines, so match the half that carries the meaning.
    assert "still required on this mirror." in script


def test_agent_logins_are_copied_only_on_stdin_and_only_when_asked():
    bootstrap = CONTROL.split(
        "QByteArray buildAgentCliBootstrapPayload(", 1
    )[1][:4000]
    # Credentials are base64 sections on stdin; nothing reaches argv.
    assert "toBase64()" in bootstrap
    script = CONTROL.split(
        "QString agentCliBootstrapRemoteCommand(bool withCredentials)", 1
    )[1][:6000]
    assert 'cat > \\"$tmp\\"' in script
    assert "$agent_home/.claude/.credentials.json" in script
    assert "$agent_home/.codex/auth.json" in script
    assert "id forkmesh-node" in script
    assert "/usr/local/bin/$program" in script
    assert "$agent_home/.forkmesh/agent-env" in script
    assert "umask 077" in script
    assert "chmod 600" in script

    driver = CHAT.split("void MainWindow::runAgentCliInstall(", 1)[1][:8000]
    assert "proc->write(payload)" in driver
    assert "payload.fill('\\0')" in driver
    # Only names are logged, never a credential value.
    assert "forkmesh::control::describeAgentCliCredentials" in driver


def test_one_click_vultr_mirror_can_sign_the_new_node_in():
    assert "vultrInstallAgentClisCheck" in CHAT
    assert "m_vultrAgentClisCheck" in HEADER
    vultr = CHAT.split("void MainWindow::startVultrHostInstall(", 1)[1][:8000]
    assert "m_vultrInstallAgentClis" in vultr
    assert "runAgentCliInstall(" in vultr
    # A missing login installs the binaries anyway; it never blocks the mirror.
    assert "agentCliCredentialsAreEmpty" in vultr


def test_vultr_api_key_is_saved_once_vultr_accepts_it():
    """The key is asked for once, then lives in both stores (adhoc #127)."""
    settings = (ROOT / "desktop/src/MainWindowSettings.cpp").read_text(
        encoding="utf-8"
    )
    control = (ROOT / "desktop/src/MainWindowControlNode.cpp").read_text(
        encoding="utf-8"
    )
    assert "QStringList rememberVultrApiKey(" in HEADER

    remember = control.split("QStringList MainWindow::rememberVultrApiKey(", 1)[
        1
    ].split("\nvoid MainWindow::adoptRotatedCloudflareToken(", 1)[0]
    # Both stores: the device variable and the deploy machine's .env.production.
    assert "ActionStore::setVariables(variables)" in remember
    assert "forkmesh::control::siteDeployEnvFilePath(" in remember
    assert "writeEnvAssignments(envPath, {{canonical, key}}, &envError," in remember
    # A stale alias must not keep shadowing the key that was just proven good.
    assert "forkmesh::control::vultrApiKeyVariableNames()" in remember
    assert "haveCanonical" in remember
    # A failed file write is retried by the next call, not recorded as done.
    assert "if (!retryable)" in remember

    # Saved only after Vultr itself authenticated the key, never on a 4xx.
    api_call = CHAT.split("void MainWindow::vultrApiCall(", 1)[1][:4000]
    assert "Vultr API error (HTTP %1): %2" in api_call
    assert api_call.index(
        "rememberVultrApiKey(apiKey, &rememberError)"
    ) > api_call.index("Vultr API error (HTTP %1): %2")
    # Both entry points prefill from the store, so neither asks twice.
    assert (
        "forkmesh::control::vultrApiKeyFromVariables(ActionStore::variables())"
        in CHAT
    )
    assert "forkmesh::control::vultrApiKeyFromVariables(storedVars)" in settings
    assert "rememberVultrApiKey(vultrTokenEdit->text().trimmed()" in settings


def test_saved_host_key_path_fails_closed_instead_of_falling_back_to_password():
    lookup = CHAT.split(
        "QString MainWindow::savedHostIdentityFile(", 1
    )[1].split("\nQString MainWindow::vultrProvisionLogPath()", 1)[0]
    assert "return identity;" in lookup
    assert "QFileInfo(identity).isFile()" not in lookup


def test_every_saved_host_defaults_to_the_shared_fleet_ssh_key():
    # A row without its own recorded key — added by hand, or saved before the
    # fleet moved to one shared key — still connects with the shared key.
    lookup = CHAT.split(
        "QString MainWindow::savedHostIdentityFile(", 1
    )[1].split("\nQString MainWindow::vultrProvisionLogPath()", 1)[0]
    assert (
        "return forkmesh::control::existingSharedHostIdentityFile();" in lookup
    )
    # Adding a host records it against that same key.
    add = CHAT.split("void MainWindow::addHostFromForm()", 1)[1][:2500]
    assert "forkmesh::control::existingSharedHostIdentityFile()" in add
    assert (
        'rememberHost(node, ip, user, pass, QStringLiteral("added"), '
        "identityFile);" in add
    )
    # One key for the whole fleet, generated or adopted once per device.
    keypair = CHAT.split("void MainWindow::ensureSharedHostKeypair(", 1)[1][
        :4000
    ]
    assert "forkmesh::control::sharedHostKeyPath()" in keypair
    assert 'QStringLiteral("forkmesh-shared-host-key")' in keypair
    # The provider registration reuses that one key rather than minting a
    # per-deployment key.
    assert "ensureSharedHostKeypair([this, apiKey, node](" in CHAT
    shared = CONTROL.split("QString sharedHostKeyPath()", 1)[1][:1500]
    assert 'QStringLiteral("forkmesh_shared_ed25519")' in shared
    assert 'QStringLiteral("vultr_mirror_ed25519")' in shared


def test_saved_hosts_are_reprobed_until_online_with_agent_capabilities():
    for contract in (
        "void MainWindow::probeSavedHosts()",
        "void MainWindow::probeSavedHost(",
        "m_hostProbeTimer->setInterval(30000)",
        "m_hostProbesInFlight.contains(key)",
        "FORKMESH=%s MIRROR=%s MIRRORSVC=%s CLAUDE=%s CODEX=%s",
        'probe_home=\\"$HOME\\"',
        'export PATH=\\"$probe_home/.local/bin:$probe_home/.claude/bin:$PATH\\"',
        "Provisioning \\xC2\\xB7 waiting for SSH",
        "Attention \\xC2\\xB7 SSH key rejected",
        "Online \\xC2\\xB7 ForkMesh missing",
        "\\xE2\\x9C\\x93 Installed",
        "Not installed",
        "QTimer::singleShot(0, this, &MainWindow::probeSavedHosts)",
    ):
        assert contract in CHAT
    for contract in (
        "QTimer *m_hostProbeTimer",
        "QSet<QString> m_hostProbesInFlight",
        "QHash<QString, QString> m_hostReachability",
        "QHash<QString, QString> m_hostClaudeAvailability",
        "QHash<QString, QString> m_hostCodexAvailability",
    ):
        assert contract in HEADER


def test_probe_counts_a_running_mirror_node_as_forkmesh_installed():
    # Managed mirrors never receive the desktop/CLI `forkmesh` binary — the
    # Vultr deploy stages /usr/local/bin/forkmesh-mirror-node and supervises it
    # with forkmesh-mirror-node.service. Probing only for `forkmesh` reported
    # every healthy mirror in the fleet as "Online · ForkMesh missing".
    probe = CHAT.split("void MainWindow::probeSavedHost(", 1)[1].split(
        "\nvoid MainWindow::", 1
    )[0]
    for contract in (
        "test -x /usr/local/bin/forkmesh-mirror-node; then mirror=1; fi;",
        "systemctl is-active --quiet forkmesh-mirror-node.service",
        "then mirror_service=1; fi;",
        '\\"$mirror\\" \\"$mirror_service\\" ',
        'output.contains(QStringLiteral("MIRROR=1"))',
        'output.contains(QStringLiteral("MIRRORSVC=1"))',
        "const bool healthy = forkmesh || (mirrorNode && mirrorRunning);",
        "Online \\xC2\\xB7 Mirror node",
        "Attention \\xC2\\xB7 Mirror ",
        "node stopped",
    ):
        assert contract in probe
    # The status column feeds the fleet summary, which counts anything that
    # says "Online" without "missing" as healthy.
    summary = CHAT.split("void MainWindow::updateHostsFleetSummary()", 1)[
        1
    ].split("\nvoid MainWindow::", 1)[0]
    assert 'status.contains(QStringLiteral("Online"))' in summary
    assert 'QStringLiteral("missing")' in summary


def test_native_mirror_node_registers_its_installer_link_code():
    setup = (
        ROOT / "desktop/src/MainWindowSetup.cpp"
    ).read_text(encoding="utf-8")
    assert "installerLinkPending" not in setup
    assert '--register-link-code "$FORKMESH_LINK_CODE"' in INSTALLER
    assert '--accounts-url "https://$relay_host/api/accounts"' in INSTALLER
    assert 'runuser -u "$service_user" -- "$node_binary"' in INSTALLER


def test_vultr_success_verifies_the_mirror_directly_without_relay_catalog():
    for contract in (
        "void MainWindow::waitForVultrMirrorPublication(",
        'url.setPath(QStringLiteral("/health"))',
        'payload.value(QStringLiteral("repositoryProof"))',
        "verifiedFlagshipMirrorHealth(",
        "kFastPublicationPolls = 90",
        '"a valid signed forkmesh/forkmesh repository proof. No relay "',
        '"Worker was used.\\n"',
    ):
        assert contract in CHAT
    wait = CHAT.split("void MainWindow::waitForVultrMirrorPublication(", 1)[1]
    wait = wait.split("\nvoid MainWindow::", 1)[0]
    assert "/api/repo/forkmesh/forkmesh/mirrors" not in wait
    assert "waitForVultrMirrorPublication" in HEADER


def test_vultr_publication_wait_starts_only_after_a_local_repository_seed():
    """Provisioning cannot bootstrap a new mirror through an unhealthy mesh."""
    seed = CHAT.split(
        "void MainWindow::seedVultrMirrorRepositoryThenVerify(", 1
    )[1].split("\nvoid MainWindow::", 1)[0]
    assert "runVultrMirrorSeedPush(" in seed
    assert "waitForVultrMirrorPublication(node, successMessage);" in seed
    assert seed.index("runVultrMirrorSeedPush(") < seed.index(
        "waitForVultrMirrorPublication(node, successMessage);"
    )

    resume = CHAT.split(
        "void MainWindow::resumeVultrProvision()", 1
    )[1].split("\nvoid MainWindow::", 1)[0]
    install = CHAT.split(
        "void MainWindow::startVultrHostInstall", 1
    )[1].split("\nnamespace {\nint installLogAnsiFg", 1)[0]
    assert "seedVultrMirrorRepositoryThenVerify(" in resume
    assert install.count("seedVultrMirrorRepositoryThenVerify(") == 2

    wait = CHAT.split(
        "void MainWindow::waitForVultrMirrorPublication(", 1
    )[1].split("\nvoid MainWindow::", 1)[0]
    nudge = wait.split("if (attempt + 1 == kFastPublicationPolls)", 1)[1]
    assert nudge.index("runVultrMirrorSeedPush(") < nudge.index(
        "diagnoseVultrMirrorNode("
    )
    assert (
        "git init --bare --quiet --initial-branch=main "
        "/var/lib/forkmesh/repositories/forkmesh-forkmesh.git"
        in CHAT
    )


def test_publication_verification_ends_the_deployment_instead_of_looping():
    # A mirror that never publishes used to keep this stage retrying once a
    # minute forever: the deployment stayed "running" with no cause shown, and
    # the fleet reconciler waited behind it. It now nudges the node once, says
    # what the node reported, and ends the deployment so it can be retried.
    wait = CHAT.split(
        "void MainWindow::waitForVultrMirrorPublication(", 1
    )[1].split("\nvoid MainWindow::", 1)[0]
    for contract in (
        "kFastPublicationPolls = 90",
        "kSlowPublicationPolls = 15",
        "kMaxPublicationPolls =\n        kFastPublicationPolls + "
        "kSlowPublicationPolls;",
        "if (attempt + 1 >= kMaxPublicationPolls) {",
        "diagnoseVultrMirrorNode(\n                node, "
        "/*restartService=*/false,",
        "finishVultrProvision(false,",
        "if (attempt + 1 == kFastPublicationPolls) {",
        "runVultrMirrorSeedPush([this, node](bool)",
        "node, /*restartService=*/true,",
        "attempt + 1 < kFastPublicationPolls ? 10000 : 60000",
    ):
        assert contract in wait
    # The old unbounded continuation restarted the counter at zero forever.
    assert "waitForVultrMirrorPublication(node, successMessage, 0);" not in wait

    diagnose = CHAT.split(
        "void MainWindow::diagnoseVultrMirrorNode(", 1
    )[1].split("\nvoid MainWindow::", 1)[0]
    for contract in (
        "systemctl is-active forkmesh-mirror-node",
        "journalctl -u forkmesh-mirror-node -n 200 --no-pager",
        "systemctl restart forkmesh-mirror-node",
        # Never throw away a clone that is still running.
        "restart: skipped, a clone is still running",
        "forkmesh::control::buildHostSshCommand(",
        "forkmesh::control::redactProcessOutput(",
        "deadline->setInterval(45000);",
    ):
        assert contract in diagnose
    assert "diagnoseVultrMirrorNode" in HEADER
    assert "savedHostAddressNamed" in HEADER


def test_vultr_requires_and_bootstraps_direct_https_without_leaking_token():
    create = CHAT.split("void MainWindow::createVultrMirrorFromForm()", 1)[1].split(
        "\nvoid MainWindow::pollVultrInstance", 1
    )[0]
    assert "cloudflareApiTokenFromVariables" in create
    assert "cloudflareZoneNameFromVariables" in create
    assert "vultrMirrorDnsHostname" in create
    assert create.index("if (cloudflareToken.isEmpty()") < create.index(
        "findVultrProvisionInstance("
    )
    assert create.index("findVultrProvisionInstance(") < create.index(
        'QStringLiteral("/v2/instances")'
    )

    command = CHAT.split("bool MainWindow::buildHostInstallCommand(", 1)[1][
        :18000
    ]
    assert "FORKMESH_PUBLIC_HOSTNAME" in command
    assert "FORKMESH_REQUIRE_DIRECT_HTTPS=1" in command
    assert "FORKMESH_TUNNEL_HOSTNAME=%1" not in command
    assert "FORKMESH_REQUIRE_TUNNEL=1" not in command
    assert 'IFS= read -r fm_cf_token' in command
    assert 'CLOUDFLARE_API_TOKEN=\\\"$fm_cf_token\\\"' in command
    # Secret value is stdin-only; only its member name may occur in the
    # generated command implementation.
    assert "m_vultrTunnelApiToken" in command
    driver = CHAT.split("void MainWindow::runHostInstall(", 1)[1][:15000]
    assert 'proc->write((m_vultrTunnelApiToken + QStringLiteral("\\n"))' in driver
    assert "rememberCloudflareApiToken(cloudflareToken" in create
    assert "QStringList MainWindow::rememberCloudflareApiToken(" in (
        ROOT / "desktop/src/MainWindowControlNode.cpp"
    ).read_text(encoding="utf-8")
    assert "m_vultrTunnelApiToken.fill" not in CHAT
    assert "FORKMESH_REQUIRE_DIRECT_HTTPS" in INSTALLER
    assert 'if [ "$FORKMESH_REQUIRE_DIRECT_HTTPS" = "1" ]; then' in INSTALLER
    assert "this install requires a live independent endpoint" in INSTALLER
