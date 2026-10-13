"""Cross-runtime agent owner-encryption contract regressions.

The executable Qt crypto test proves seal/open authentication.  These source
contract checks make sure the bytes produced by that helper are the bytes the
Worker accepts/stores and that no browser plaintext fallback is reintroduced.
"""

from pathlib import Path


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

ROOT = Path(__file__).resolve().parents[2]
WORKER = ROOT / "app" / "src" / "entry.py"
AGENT_ROUTES = ROOT / "app" / "src" / "agent_routes.py"
SECURITY = ROOT / "app" / "src" / "security_controls.py"
QT_CRYPTO = ROOT / "desktop" / "src" / "MirrorCrypto.cpp"
QT_AGENTS = ROOT / "desktop" / "src" / "MainWindowAgents.cpp"
QT_SETTINGS = ROOT / "desktop" / "src" / "MainWindowSettings.cpp"
QT_HEADLESS = ROOT / "desktop" / "src" / "HeadlessConsole.cpp"
QT_RUNNER = ROOT / "desktop" / "src" / "AgentRunner.cpp"
QT_AGENT_STORE = ROOT / "desktop" / "src" / "AgentStore.cpp"
QT_CMAKE = ROOT / "desktop" / "CMakeLists.txt"
QT_TRANSFER_SOURCE = ROOT / "desktop" / "src" / "ClaudeAccountTransfer.cpp"
QT_TRANSFER_HEADER = ROOT / "desktop" / "src" / "ClaudeAccountTransfer.h"
DASHBOARD_SOURCE = (
    ROOT / "app" / "public" / "dashboard" / "js"
    / "06-repo-content.js"
)
DASHBOARD_BUNDLE = ROOT / "app" / "public" / "dashboard.js"
DASHBOARD_HEADER = (
    ROOT / "app" / "public" / "dashboard" / "partials"
    / "header.html"
)


def _section(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


def test_qt_and_worker_share_complete_single_recipient_hybrid_envelope():
    qt = QT_CRYPTO.read_text(encoding="utf-8")
    security = SECURITY.read_text(encoding="utf-8")

    qt_seal = _section(
        qt,
        "QJsonObject MirrorCrypto::sealOwnerPayload(",
        "QString MirrorCrypto::ownerPayloadKeyId(",
    )
    for field in (
        '"kind"), QStringLiteral("forkmesh.owner-sealed")',
        '"x25519"',
        '"mlkem768"',
        '"nonce"',
        '"tag"',
        '"key"',
        '"body"',
        '"recipients"',
    ):
        assert field in qt_seal
    assert '"kid"), recipient.value' in qt_seal

    validator = _section(
        security,
        "def validate_owner_envelope(",
        "def owner_envelope_key_id(",
    )
    # Bounded owner-device recipient list: every entry fully validated,
    # duplicate recipient ids refused, count capped.
    assert "MAX_OWNER_ENVELOPE_RECIPIENTS" in validator
    assert "kid in seen_kids" in validator
    assert "_valid_b64url(ephemeral, 32, 32)" in validator
    assert "_valid_b64url(kem_ciphertext, 1088, 1088)" in validator
    assert "_valid_b64url(wrapped_key, 32, 32)" in validator
    assert '"recipients": validated_recipients' in validator
    # The obsolete kid/nonce/tag/body-only framing cannot pass.
    assert 'kid = str(value.get("kid")' not in validator
    # Authorization is membership of the required key, not equality with a
    # single recipient, so companion owner devices can share the envelope.
    worker = (
        WORKER.read_text(encoding="utf-8")
        + "\n\n\n"
        + AGENT_ROUTES.read_text(encoding="utf-8")
    )
    assert "owner_envelope_key_ids(envelope)" in worker
    assert "required_kid not in envelope_kids" in worker


def test_qt_posts_only_encrypted_sessions_and_locally_opens_prompts():
    qt = "".join(
        (QT_AGENTS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_AGENT_SOURCES
    )
    push = _section(
        qt,
        "void MainWindow::pushAgentSessionsForRepo(",
        "void MainWindow::scheduleAgentSessionsPush(",
    )
    assert "MirrorCrypto::sealOwnerPayload(" in push
    assert 'QStringLiteral("encryptedSessions")' in push
    body = _section(
        push,
        "const QByteArray body =",
        "const QString owner =",
    )
    assert "encryptedSessions" in body
    assert '"sessions"' not in body
    assert "m_networkAccess->post(request, body)" in push
    assert "ensureAgentE2EEControlPlane(" in push
    assert '"forkmesh.agent-session"' in push
    assert '"repositoryOwner"' in push
    assert '"repositoryName"' in push

    apply_prompt = _section(
        qt,
        "void MainWindow::applyAgentPromptsPayload(",
        "void MainWindow::deliverQueuedAgentPrompt(",
    )
    assert "MirrorCrypto::openOwnerPayload(" in apply_prompt
    assert "rememberAcceptedAgentPrompt(" in apply_prompt
    assert "acknowledgeAgentPrompts(" in apply_prompt
    assert "Rejected a relay-readable or malformed agent prompt." in apply_prompt
    assert "routedAgentId" in apply_prompt
    assert '"forkmesh.agent-prompt"' in apply_prompt
    assert "repositoryOwner" in apply_prompt
    assert "repositoryName" in apply_prompt


def test_qt_seals_sessions_to_every_enrolled_owner_device():
    """A companion owner device (the mobile node) must be able to read what the
    desktop seals, without the relay ever holding a key for either of them."""
    qt = "".join(
        (QT_AGENTS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_AGENT_SOURCES
    )
    push = _section(
        qt,
        "void MainWindow::pushAgentSessionsForRepoWithTranscripts(",
        "ClaudeIdeBridge *MainWindow::ensureIdeBridge(",
    )
    # The vault key stays first: it is the policy key the relay requires.
    assert "recipientBundles{ownerIdentity.publicBundle()}" in push
    assert "companionOwnerBundles(ownerKeyId, nowMs)" in push
    assert "recipientBundles, &sealError)" in push

    companions = _section(
        qt,
        "void MainWindow::refreshCompanionOwnerBundles(",
        "void MainWindow::pushAgentSessionsSnapshot(",
    )
    # Public bundles only, fetched from the same registry the desktop enrolls
    # its own key with, so a device revoked on the website stops receiving new
    # snapshots on the next refresh.
    assert '"/api/security/owner-keys"' in companions
    assert "MirrorCrypto::publicKeyId(bundle)" in companions
    # The stated key id is never trusted over the bundle bytes it must match.
    assert 'keyId != key.value(QStringLiteral("keyId")).toString()' in companions
    # A changed recipient set must re-seal rather than reuse the dedup memo.
    assert "m_lastAgentPushPayload.clear()" in companions

    crypto = QT_CRYPTO.read_text(encoding="utf-8")
    open_payload = _section(
        crypto,
        "QByteArray MirrorCrypto::openOwnerPayload(",
        "\n}\n",
    )
    # Each device opens its own wrap entry, never another device's by position.
    assert "keyIds.indexOf(me.keyId())" in open_payload
    assert ".at(myEntry)" in open_payload


def test_worker_agent_boundary_has_no_silent_missing_policy_downgrade():
    worker = (
        WORKER.read_text(encoding="utf-8")
        + "\n\n\n"
        + AGENT_ROUTES.read_text(encoding="utf-8")
    )
    agents = _section(
        worker,
        "async def agents_handler(",
        "async def agents_list_handler(",
    )
    assert '"requireAgentE2EE": True' in agents
    assert 'data.get("encryptedSessions")' in agents
    assert '"error": "owner_encryption_required"' in agents
    assert "owner_envelope_key_ids(envelope)" in agents

    prompt = _section(
        worker,
        "async def agents_prompt_handler(",
        "async def agents_ack_handler(",
    )
    assert '"requireAgentE2EE": True' in prompt
    assert "validate_owner_envelope" in prompt
    assert '"error": "owner_encryption_required"' in prompt


def test_browser_agent_surface_keeps_owner_sealed_data_separate_from_commands():
    source = DASHBOARD_SOURCE.read_text(encoding="utf-8")
    bundle = DASHBOARD_BUNDLE.read_text(encoding="utf-8")
    header = DASHBOARD_HEADER.read_text(encoding="utf-8")
    for text in (source, bundle):
        assert "/agents/new/prompt" not in text
        assert "/agents/${encodeURIComponent(agentId)}/prompt" not in text
        assert "${repoApiBase(repo)}/agents/list" not in text
        assert "Owner-device encrypted agents" in text
        assert "administrators do not receive an override" in text
        assert "${repoApiBase(repo)}/agent-sessions" in text
    assert "data-repo-agent-new-form" in header
    assert "data-agent-modal-target" in header
    assert "owner-device private keys never leave that node" in header
    assert "Prompts are encrypted at rest" in header


def test_provider_credentials_have_no_forkmesh_transfer_surface():
    """The old reversible-base64 account bundle must stay retired."""
    assert not QT_TRANSFER_SOURCE.exists()
    assert not QT_TRANSFER_HEADER.exists()

    cmake = QT_CMAKE.read_text(encoding="utf-8")
    settings = QT_SETTINGS.read_text(encoding="utf-8")
    assert not QT_HEADLESS.exists()
    assert "ClaudeAccountTransfer" not in cmake
    assert "ClaudeAccountTransfer" not in settings
    assert "forkmesh-claude-account" not in settings

    setup = _section(
        settings,
        "void MainWindow::showClaudeCodeDeviceSetup()",
        "QByteArray MainWindow::effectiveAvatar()",
    )
    for forbidden in (
        "QFileDialog",
        "QApplication::clipboard",
        "QGuiApplication::clipboard",
        "toBase64",
        "fromBase64",
        "accessToken",
        "refreshToken",
        "API key input",
    ):
        assert forbidden not in setup
    assert "run <code>claude</code>" in setup
    assert "owner-sealed task, session-state, transcript, and" in setup
    assert "logins remain isolated " in setup
    assert "to the owner device that runs the agent" in setup

    # There is no headless desktop command surface left from which credentials
    # could be exported; managed servers use the native Go node.
    main = (ROOT / "desktop" / "src" / "main.cpp").read_text(encoding="utf-8")
    assert "HeadlessConsole" not in main
    assert "claude-auth export" not in main
    assert "claude-auth import" not in main


def test_agent_workspace_serialization_excludes_live_provider_credentials():
    agents = "".join(
        (QT_AGENTS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_AGENT_SOURCES
    )
    store = QT_AGENT_STORE.read_text(encoding="utf-8")
    runner = QT_RUNNER.read_text(encoding="utf-8")

    metadata = _section(
        store,
        "QJsonObject AgentSession::toJson() const",
        "AgentSession AgentSession::fromJson(",
    )
    for forbidden in (
        '"accessToken"',
        '"refreshToken"',
        '"oauth"',
        '"apiKey"',
        '"anthropicApiKey"',
        '"openAiApiKey"',
        '"credentials"',
    ):
        assert forbidden not in metadata

    push = _section(
        agents,
        "void MainWindow::pushAgentSessionsForRepo(",
        "void MainWindow::scheduleAgentSessionsPush(",
    )
    assert '"credentialBoundary", "owner-device-only"' in push
    assert (
        '"sharedWorkspaceBoundary",\n'
        '             "owner-sealed-task-state-artifacts-only"'
    ) in push
    assert "redactProviderCredentials(" in push
    assert push.index("redactProviderCredentials(") < push.index(
        "MirrorCrypto::sealOwnerPayload("
    )
    assert '"encryptedSessions"' in push
    assert "m_networkAccess->post(request, body)" in push

    ingest = _section(
        agents,
        "void MainWindow::applyTranscriptEvent(",
        "void MainWindow::renderTranscriptForSession(",
    )
    assert "redactProviderCredentials(" in ingest
    assert ingest.index("redactProviderCredentials(") < ingest.index(
        "m_agentStore->appendEvent("
    )

    for variable in (
        "ANTHROPIC_API_KEY",
        "ANTHROPIC_AUTH_TOKEN",
        "ANTHROPIC_ADMIN_KEY",
        "CLAUDE_CODE_OAUTH_TOKEN",
        "CODEX_API_KEY",
        "OPENAI_API_KEY",
        "OPENAI_ACCESS_TOKEN",
        "OPENAI_ADMIN_KEY",
    ):
        assert f'env.remove(QStringLiteral("{variable}"))' in runner
