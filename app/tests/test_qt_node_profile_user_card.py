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
THEME = QT_SRC / "Theme.h"


def _body(source: str, start: str, end: str) -> str:
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j]


def test_node_profile_features_user_card_before_node_details():
    header = HEADER.read_text(encoding="utf-8")
    chat = "".join(path.read_text(encoding="utf-8") for path in CHAT)
    build = _body(
        chat,
        "QWidget *MainWindow::buildNodeProfilePanel()",
        "void MainWindow::hideNodeProfile()",
    )

    assert "QListWidget *m_profileUserNodesList = nullptr;" in header
    # The user card no longer repeats the node's own avatar/name (shown in the
    # left-column header); those widgets are gone.
    assert "m_profileUserAvatar" not in header
    assert "m_profileUserName" not in header
    assert 'm_profileAccountSection->setObjectName("profileUserCard");' in build
    assert 'm_profileAccountLabel = makeProfileSection("NODES");' in build
    assert "accountLayout->addWidget(m_profileAccountLabel);" in build
    # adhoc #177: the "USER PROFILE" card lists every node linked to the user
    # account (not just this node), so it is shown in the right column. It is
    # not a redundant identity repeat — it surfaces the user's *other* nodes.
    assert "rightColumn->addWidget(m_profileAccountSection);" in build
    assert "layout->addWidget(m_profileAccountSection);" not in build


def test_node_profile_user_card_lists_owned_nodes_with_icons():
    chat = "".join(path.read_text(encoding="utf-8") for path in CHAT)
    render = _body(
        chat,
        "void MainWindow::renderProfileAccountStatus()",
        "void MainWindow::applyProfileAccountRecord(",
    )

    assert "m_profileUserNodesList->clear();" in render
    assert "nodeMachineFavicon(nodeName, 32)" in render
    assert "roundedRectPixmap(nodeMachineFavicon(nodeName, 32), 32, 8)" in render
    assert 'QStringLiteral("  (this machine)")' in render
    assert "m_profileUserNodesList->setFixedHeight" in render


def test_node_profile_user_card_has_dark_and_light_styles():
    theme = THEME.read_text(encoding="utf-8")

    assert theme.count("#profileUserCard") >= 2
    assert theme.count("#profileNodeList::item") >= 2
