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

ROOT = Path(__file__).resolve().parents[2]
QT_SRC = ROOT / "desktop" / "src"
RELEASES = QT_SRC / "MainWindowReleases.cpp"
REPOS = QT_SRC / "MainWindowRepos.cpp"
BACKEND = QT_SRC / "ChatBackend.h"
SERVER_NODE = QT_SRC / "ServerNode.cpp"
SERVER_NODE_H = QT_SRC / "ServerNode.h"
MAIN_WINDOW_H = QT_SRC / "MainWindow.h"
SETTINGS = QT_SRC / "MainWindowSettings.cpp"
GATEWAY = ROOT / "tools" / "mirror_gateway.py"


def _body(source: str, start: str, end: str) -> str:
    i = source.index(start)
    j = source.index(end, i)
    return source[i:j]


def test_mirror_nodes_table_has_separate_owner_column():
    source = RELEASES.read_text(encoding="utf-8")
    build = _body(
        source,
        "QWidget *MainWindow::buildMirrorNodesTab()",
        "void MainWindow::loadMirrorNodesPanel()",
    )
    load = _body(
        source,
        "void MainWindow::loadMirrorNodesPanel()",
        "void MainWindow::fetchCatalogMirrors",
    )

    assert "MirrorNodeColOwner" in source
    assert '{MirrorNodeColNode, "device-desktop", "Node", "Node"}' in build
    assert '{MirrorNodeColOwner, "person", "Owner", "Owner"}' in build
    assert '{MirrorNodeColCommit, "git-commit", "Latest commit", "Commit"}' in build
    assert ('{MirrorNodeColGitDatabase, "database", '
            '"Physical Git object database", "Git DB"}') in build
    assert "new QTableWidget(0, MirrorNodeColumnCount)" in build
    assert "m_mirrorNodesTable->setItem(row, MirrorNodeColOwner" in load
    assert 'm.value(QStringLiteral("ownerUser"))' in load


def test_mirror_nodes_show_the_latest_commit_message_and_author():
    # A hash alone doesn't say what a node is serving, so every row names the
    # commit's subject and author (adhoc #337). Peers advertise the identity
    # themselves; our own mirror is only the fallback for older peers.
    source = RELEASES.read_text(encoding="utf-8")
    gateway = GATEWAY.read_text(encoding="utf-8")
    build = _body(
        source,
        "QWidget *MainWindow::buildMirrorNodesTab()",
        "void MainWindow::requestMirrorNodesRefresh()",
    )
    load = _body(
        source,
        "void MainWindow::loadMirrorNodesPanel()",
        "void MainWindow::fetchCatalogMirrors",
    )

    assert '{MirrorNodeColCommit, "git-commit", "Latest commit", "Commit"}' in build
    assert '{MirrorNodeColMessage, "note", "Message", "Message"}' in build
    assert '{MirrorNodeColAuthor, "pencil", "Author", "Author"}' in build
    assert "MirrorNodeColMessage" in source and "MirrorNodeColAuthor" in source
    # Both row builders (live roster and catalog-backed) fill the columns.
    assert load.count("setCommitIdentityCells(row,") == 2
    assert "resolveCommitIdentity(advert->commit, advert->commitIdentity)" in load
    assert 'm.value(QStringLiteral("lastCommitMessage"))' in load
    assert 'm.value(QStringLiteral("lastCommitAuthorName"))' in load

    # The direct branches response supplies identity without a Worker/catalog
    # round trip, and the desktop copies it into the displayed host record.
    assert "%(subject)%00%(authorname)" in gateway
    assert '"message": fields[3]' in gateway
    assert '"author": fields[4]' in gateway
    assert 'QStringLiteral("lastCommitMessage")' in source
    assert 'QStringLiteral("lastCommitAuthorName")' in source


def test_mirror_nodes_use_node_account_not_chat_name():
    source = RELEASES.read_text(encoding="utf-8")
    load = _body(
        source,
        "void MainWindow::loadMirrorNodesPanel()",
        "void MainWindow::fetchCatalogMirrors",
    )

    assert "advert->ownerName.section(QLatin1Char('/'), 0, 0)" in load
    assert "name = node.nodeName.trimmed();" in load
    assert "name = accountOwner().trimmed();" in load
    assert "const QString nodeDisplay = displayNodeName(node, advert);" in load
    assert "shownNames.insert(nodeDisplay.trimmed().toLower());" in load
    assert "nodeDisplay.trimmed().toLower()" in load


def test_mirror_nodes_import_direct_and_ssh_repository_counts():
    source = RELEASES.read_text(encoding="utf-8")
    load = _body(
        source,
        "void MainWindow::loadMirrorNodesPanel()",
        "void MainWindow::fetchCatalogMirrors",
    )
    assert "localPublicRepositories" in source
    assert "localRepositoryCount" in source
    assert 'QStringLiteral("repositoryCount")' in source
    assert 'QStringLiteral("publicRepositoryCount")' in source
    assert "reposByNode.insert(key" in load


def test_synced_nodes_show_the_same_canonical_repository_size():
    source = RELEASES.read_text(encoding="utf-8")
    load = _body(
        source,
        "void MainWindow::loadMirrorNodesPanel()",
        "void MainWindow::fetchCatalogMirrors",
    )
    assert "const qint64 refSizeBytes" in load
    assert load.count("usesCanonicalSize") >= 6
    assert load.count("Canonical repository content size") == 2


def test_source_of_truth_is_not_marked_as_rejecting_its_own_pin():
    source = RELEASES.read_text(encoding="utf-8")
    load = _body(
        source,
        "void MainWindow::loadMirrorNodesPanel()",
        "void MainWindow::fetchCatalogMirrors",
    )
    assert "(node.self && m_repoPinMismatch)" not in load
    assert 'QLatin1String("rejected")' in load


def test_desktop_does_not_publish_mirror_catalog_metadata():
    source = "".join(
        (REPOS.parent / name).read_text(encoding="utf-8")
        for name in MAIN_WINDOW_REPOS_SOURCES
    )
    relay = SERVER_NODE.read_text(encoding="utf-8")
    assert "void MainWindow::publishRepositoryNow" not in source
    assert "writeMirrors(hello" not in relay
    assert "setMirroredRepos(" not in relay


def test_mirror_nodes_can_request_live_peer_refresh():
    releases = RELEASES.read_text(encoding="utf-8")
    server = SERVER_NODE.read_text(encoding="utf-8")
    window_h = MAIN_WINDOW_H.read_text(encoding="utf-8")

    build = _body(
        releases,
        "QWidget *MainWindow::buildMirrorNodesTab()",
        "void MainWindow::requestMirrorNodesRefresh()",
    )
    request = _body(
        releases,
        "void MainWindow::requestMirrorNodesRefresh()",
        "void MainWindow::loadMirrorNodesPanel()",
    )

    assert "setAccessibleName(QStringLiteral(\"Refresh nodes\"))" in build
    assert "refreshNodesButton->setFixedSize(32, 30)" in build
    assert "&MainWindow::requestMirrorNodesRefresh" in build
    assert "m_backend->requestMirrorRefresh" not in request
    assert "m_backend->advertiseMirrorsNow" not in request
    assert "m_catalogMirrorsFetchedMs = 0;" in request
    assert "refreshing active Hosts directly" in request

    # Socket events repaint promptly, opening the tab reads, and Refresh
    # forces a read. There is no repair timer, and a rebuild of a hidden panel
    # (roster frames rebuild it on any page) never reaches the network.
    assert "panelRefresh" not in releases
    assert "No repair timer" in build
    assert "if (m_mirrorNodesTable->isVisible() &&" in releases

    assert 'makeMessage("mirror-refresh")' not in server
    assert 'type == "mirror-refresh"' not in server
    assert "void requestMirrorNodesRefresh();" in window_h
    assert "void onMirrorRefreshRequested" not in window_h


def test_mirror_nodes_header_labels_cover_every_column():
    source = RELEASES.read_text(encoding="utf-8")
    enum = _body(source, "enum MirrorNodeColumn {", "};")
    columns = [
        line.strip().rstrip(",").split(" ")[0]
        for line in enum.splitlines()[1:]
        if line.strip().startswith("MirrorNodeCol")
        and not line.strip().startswith("MirrorNodeColumnCount")
    ]
    assert columns[0].startswith("MirrorNodeColNode")
    labels = _body(
        source,
        "static constexpr MirrorHeaderIcon kMirrorHeaderIcons[] = {",
        "static_assert(sizeof(kMirrorHeaderIcons)",
    )
    mappings = [
        line for line in labels.splitlines()
        if line.strip().startswith("{MirrorNodeCol")
    ]
    assert len(mappings) == len(columns), (
        f"{len(columns)} columns but {len(mappings)} header mappings"
    )
    assert '{MirrorNodeColNode, "device-desktop", "Node", "Node"}' in labels
    assert '{MirrorNodeColOwner, "person", "Owner", "Owner"}' in labels
    assert "MirrorNodeColSync" not in columns
    assert "MirrorNodeColStatus" not in columns
    assert "MirrorNodeColTunnel" not in columns
    assert "MirrorNodeColRoute" in columns
    assert 'QStringLiteral("Sync now")' not in source
    assert mappings[-1].strip() == (
        '{MirrorNodeColReachability, "link", "Reachability", "Reach"},'
    )

    # Every header shows its caption, not just the icon, and the header type is
    # shrunk so all the columns still fit the panel.
    build = _body(
        source,
        "QWidget *MainWindow::buildMirrorNodesTab()",
        "void MainWindow::loadMirrorNodesPanel()",
    )
    assert "headerItem->setText(QLatin1String(h.text));" in build
    assert 'QHeaderView::section { font-size: 10px;' in build
