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
QT_SRC = ROOT / "desktop" / "src"
CHAT = tuple(QT_SRC / name for name in MAIN_WINDOW_CHAT_SOURCES)
HEADER = QT_SRC / "MainWindow.h"
SETTINGS = QT_SRC / "MainWindowSettings.cpp"
SETUP = QT_SRC / "MainWindowSetup.cpp"


def _body(source: str, start: str, end: str) -> str:
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j]


def test_top_bar_uses_one_account_avatar_for_identity_and_switching():
    header = HEADER.read_text(encoding="utf-8")
    chat = "".join(path.read_text(encoding="utf-8") for path in CHAT)

    assert "QLabel *m_userLabel = nullptr;" not in header
    assert "QPushButton *m_userMenuButton = nullptr;" not in header
    assert "QPushButton *m_avatarNavButton = nullptr;" not in header
    assert "QPushButton *m_accountSwitcherButton = nullptr;" in header

    assert "m_accountSwitcherButton->setParent(projectContextStash);" in chat
    assert "&MainWindow::showUserAccountMenu" in chat
    assert "updateUserAvatarButton();" in chat
    assert "click to switch users or" in chat


def test_top_bar_user_identity_prefers_linked_owner_over_node_name():
    setup = SETUP.read_text(encoding="utf-8")
    body = _body(
        setup,
        "QString MainWindow::topBarUserName() const",
        "bool MainWindow::accountEmailVerified",
    )

    assert body.index("m_nodeOwnerUser.trimmed().toLower()") < body.index(
        "settingsAccountName()"
    )
    assert "return accountNameFromInput(m_userName, QString());" in body


def test_user_avatar_uses_user_identicon_not_node_machine_avatar():
    settings = SETTINGS.read_text(encoding="utf-8")
    user_body = _body(
        settings,
        "QByteArray MainWindow::effectiveUserAvatar()",
        "void MainWindow::updateAvatarButton()",
    )
    node_body = _body(
        settings,
        "QByteArray MainWindow::effectiveAvatar()",
        "QByteArray MainWindow::effectiveUserAvatar()",
    )

    assert "topBarUserName()" in user_body
    assert "forkMeshAvatarPng(seed)" in user_body
    assert "forkMeshNodeAvatarPng(seed)" in node_body
