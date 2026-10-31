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

ROOT = Path(__file__).resolve().parents[2]
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")
CHAT = "".join(
    (ROOT / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
REPOS = "".join(
    (ROOT / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_REPOS_SOURCES
)
SETUP = (ROOT / "desktop/src/MainWindowSetup.cpp").read_text(encoding="utf-8")


def test_avatar_opens_account_and_instance_management():
    assert 'QStringLiteral("accountSwitcherButton")' in CHAT
    assert "&MainWindow::showUserAccountMenu" in CHAT
    assert 'QStringLiteral("Manage account")' in REPOS
    assert 'QStringLiteral("Manage relay instances")' in REPOS
    assert 'QStringLiteral("Manage mirror servers")' in REPOS
    assert "showNetworkTab(kNetworkRelaysTab)" in REPOS
    assert "showNetworkTab(kNetworkHostsTab)" in REPOS


def test_recent_users_are_unlimited_and_switch_through_real_login():
    assert "QStringList MainWindow::recentUserAccounts() const" in REPOS
    assert "while (accounts.size() > 8)" not in REPOS
    assert "saveRecentUserIdentities(identities);" in REPOS
    assert 'QStringLiteral("Switch to @%1 on %2…")' in REPOS
    assert "restoreCachedAccountSession(account, targetServer)" in REPOS
    assert "if (!restored && !runLoginFlow(account))" in REPOS
    assert "cacheActiveAccountSession(previousServer)" in REPOS
    assert "rememberUserAccount(m_accountName);" in SETUP


def test_recent_users_remember_and_restore_their_instance():
    assert "kRecentUserInstancesSetting" in REPOS
    assert "saveRecentUserInstance(account, target, instanceId)" in REPOS
    assert "rememberUserAccount(m_accountName, targetServer)" in REPOS
    assert "targetServer = recentUserInstance(account)" in REPOS
    assert "switchToServer(targetServerIndex)" in REPOS
    assert 'QStringLiteral("Instance or relay")' in REPOS
    assert 'QStringLiteral("Sign in on another instance…")' in REPOS


def test_remote_instance_login_keeps_owner_signing_fail_closed():
    assert "AccountCapability::allowsPasswordOnlyFallback(err)" in SETUP
    assert "activateAccountSession(webResp, requestedAccount, false," in SETUP
    assert "Owner-signed actions \"\n                             \"stay off on this desktop" in REPOS
    assert 'payload.value(QStringLiteral("isAdmin")).toBool(false)' in SETUP
    assert "m_profileLinkedNodes.clear()" in SETUP
    assert "void showUserAccountMenu();" in HEADER


def test_cross_instance_login_is_bound_to_selected_origin_and_account():
    assert "postAccountSyncForInstance(" in SETUP
    assert 'QJsonObject loginRequest{{"nodeName", requestedAccount}' in SETUP
    assert "accountLoginResponseAccepted(requestedAccount, resp)" in SETUP
    assert "accountLoginResponseAccepted(requestedAccount, webResp)" in SETUP
    assert "loginResponseMatchesRequestedAccount(" in SETUP
    assert "accountApiUrlForInstance(instanceUrl, leaf)" in SETUP
    setup_server_load = SETUP[
        SETUP.index("const QString savedServerUrl") : SETUP.index(
            "m_serverUrlEdit->setText", SETUP.index("const QString savedServerUrl")
        )
    ]
    assert 'endsWith(QStringLiteral(".workers.dev"))' not in setup_server_load
    startup_server_load = CHAT[
        CHAT.index("if (m_servers.isEmpty())") : CHAT.index(
            "m_activeServer = QSettings().value", CHAT.index("if (m_servers.isEmpty())")
        )
    ]
    assert 'endsWith(QStringLiteral(".workers.dev"))' not in startup_server_load


def test_every_desktop_of_the_account_keeps_owner_signing_after_a_switch():
    assert "accountDesktopKeysForSession(session)" in REPOS
    assert 'QStringLiteral("desktops")' in REPOS
    assert "session, localKey, primaryKey, accountDesktopKeys)" in REPOS
    assert "hasOwnerSigningCapability()\n            ? " in REPOS
    assert "owner-signed actions remain disabled" not in REPOS
