"""Contracts for the fast native-only Vultr mirror bootstrap."""

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
SETUP = (ROOT / "desktop/src/MainWindowSetup.cpp").read_text(encoding="utf-8")
DEPLOY = (ROOT / "app/deploy.sh").read_text(encoding="utf-8")
INSTALLER = (ROOT / "www/public/install.sh").read_text(
    encoding="utf-8"
)
RELEASE_WORKFLOW = (ROOT / ".forkmesh/release.yml").read_text(encoding="utf-8")


def test_vultr_uses_one_native_upload_instead_of_the_desktop_installer():
    driver = CHAT.split("void MainWindow::startVultrHostInstall(", 1)[1][:9000]
    builder = CHAT.split(
        "bool MainWindow::buildVultrMirrorNodeInstallCommand(", 1
    )[1].split("\nbool MainWindow::buildHostInstallCommand(", 1)[0]
    assert "kMaxInstallAttempts = 1" in driver
    assert "buildVultrMirrorNodeInstallCommand" in CHAT
    assert "__FORKMESH_GO_MIRROR_V1__" in CHAT
    assert 'QStandardPaths::findExecutable(QStringLiteral("forkmesh-mirror-node"))' in builder
    assert "forkmesh-mirror-node.service" in builder
    assert "systemctl enable --now forkmesh-mirror-node.service" in builder
    assert "Go mirror-node installed and running (no Qt/GTK packages)" in builder
    assert "install.sh" not in builder
    assert "apt-get install" in builder
    assert "python3-cryptography" in builder
    assert "python3 -c 'import cryptography'" in builder
    assert "qt6" not in builder.lower()
    assert "gtk" not in builder.lower().replace("no qt/gtk packages", "")


def test_native_payload_is_checksummed_linked_and_health_checked():
    builder = CHAT.split(
        "bool MainWindow::buildVultrMirrorNodeInstallCommand(", 1
    )[1].split("\nbool MainWindow::buildHostInstallCommand(", 1)[0]
    for contract in (
        "QCryptographicHash::Sha256",
        "sha256sum",
        "--init --config /etc/forkmesh/mirror-node.json",
        "--register-link-code",
        "FORKMESH LINK CODE:",
        "http://127.0.0.1:8791/healthz",
        "https://"):
        assert contract in builder


def test_native_bootstrap_reports_timestamped_remote_milestones():
    builder = CHAT.split(
        "bool MainWindow::buildVultrMirrorNodeInstallCommand(", 1
    )[1].split("\nbool MainWindow::buildHostInstallCommand(", 1)[0]
    assert "fm_log() {" in builder
    assert "+%Y-%m-%dT%H:%M:%SZ" in builder
    for milestone in (
        "Native mirror package received and SHA-256 verified.",
        "Installing required Debian packages and the host HTTPS server.",
        "Fetching the ForkMesh router identity.",
        "provisioning unproxied DNS and host-owned HTTPS.",
        "Independent HTTPS configured",
        "Starting the ForkMesh mirror service.",
    ):
        assert milestone in builder


def test_native_bootstrap_is_tunnel_free_and_uploads_replica_dependencies():
    builder = CHAT.split(
        "bool MainWindow::buildVultrMirrorNodeInstallCommand(", 1
    )[1].split("\nbool MainWindow::buildHostInstallCommand(", 1)[0]
    for contract in (
        "direct_https_bootstrap.py",
        "mirror_replica.py",
        "public_snapshots.py",
        "portable_sessions.py",
        "node_operator_policy.py",
        "secret_gate.py",
        "[ \\\"$count\\\" = 9 ]",
        "--disable-cloudflared",
        "--mirror-url %6",
        "public-https-origin",
        "reload-or-restart caddy.service",
        "ufw allow 443/tcp",
    ):
        assert contract in builder
    assert "cloudflare_tunnel_bootstrap.py" not in builder
    assert "cloudflared_install.py" not in builder
    assert "cfargotunnel.com" not in builder


def test_native_command_percent_tokens_survive_qstring_rendering():
    builder = CHAT.split(
        "bool MainWindow::buildVultrMirrorNodeInstallCommand(", 1
    )[1].split("\nbool MainWindow::buildHostInstallCommand(", 1)[0]
    assert "printf '%s' \\\"$router_json\\\"" in builder
    assert "printf '%s' \\\"$router_key\\\"" in builder
    assert "link_number % 1000000" in builder
    assert "link_code=\\\"${link_code#1}\\\"" in builder
    assert "FORKMESH LINK CODE: %s" in builder
    assert "%%s" not in builder
    assert "%%06d" not in builder


def test_update_flows_refresh_the_go_companion():
    # "Click Update to retry" must be able to heal a desktop that is missing
    # forkmesh-mirror-node: the source rebuild builds the companion with the
    # local Go toolchain and installs it beside the client, and the prebuilt
    # auto-update fetches the published companion asset by SHA-256.
    assert "buildMirrorNodeCompanion" in SETUP
    assert "./cmd/forkmesh-mirror-node" in SETUP
    assert "installMirrorNodeCompanionFile" in SETUP
    assert "installMirrorCompanionThenRelaunch" in SETUP
    # The companion asset shares the client asset's os/arch, so the auto-update
    # client slot must select by name and never install the companion as the
    # client binary.
    assert 'name.startsWith(QStringLiteral("forkmesh-mirror-node"))' in SETUP


def test_companion_lookup_includes_the_installed_bin_directory():
    # GUI launches often lack ~/.local/bin in PATH, so both companion lookups
    # must also check the installer-owned directory beside the installed client.
    builder = CHAT.split(
        "bool MainWindow::buildVultrMirrorNodeInstallCommand(", 1
    )[1].split("\nbool MainWindow::buildHostInstallCommand(", 1)[0]
    assert "runningClientExecutable()" in builder
    assert "QCoreApplication::applicationDirPath()" in builder
    assert "QDir(binDir).filePath(QStringLiteral(\"forkmesh-mirror-node\"))" in SETUP


def test_desktop_no_longer_contains_the_legacy_mirror_server():
    assert not (ROOT / "desktop/src/MainWindowMirrorNode.cpp").exists()
    assert not (ROOT / "desktop/src/RepoHost.cpp").exists()
    assert not (ROOT / "desktop/src/HeadlessConsole.cpp").exists()


def test_signed_release_publishes_and_installs_the_go_companion():
    assert "forkmesh-mirror-node-${os}-${arch}" in DEPLOY
    assert "CGO_ENABLED=0 go build -trimpath" in DEPLOY
    assert 'publish_args+=("app/$mirror_asset")' in DEPLOY
    assert 'mirror_name="forkmesh-mirror-node-${ASSET_OS}-${ASSET_ARCH}"' in INSTALLER
    assert "_install_mirror_node_binary" in INSTALLER
    # The Actions release build publishes the companion under the exact asset
    # name install.sh resolves, and fails closed instead of cutting a
    # client-only release when the Go toolchain is missing.
    assert 'mirror_asset="forkmesh-mirror-node-${os}-${arch}"' in RELEASE_WORKFLOW
    assert "CGO_ENABLED=0 go build -trimpath" in RELEASE_WORKFLOW
    assert "./cmd/forkmesh-mirror-node" in RELEASE_WORKFLOW
    assert 'publish_assets=("$mirror_asset")' in RELEASE_WORKFLOW
    assert 'publish_assets+=("$staged")' in RELEASE_WORKFLOW
    assert (
        "ERROR: Go is required to build the native mirror-node release asset."
        in RELEASE_WORKFLOW
    )
