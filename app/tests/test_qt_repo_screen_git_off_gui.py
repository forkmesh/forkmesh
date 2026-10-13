import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "desktop/src"
REPO_DETAIL = (SRC / "MainWindowRepoDetail.cpp").read_text(encoding="utf-8")
SETUP = (SRC / "MainWindowSetup.cpp").read_text(encoding="utf-8")
HEADER = (SRC / "MainWindow.h").read_text(encoding="utf-8")
WATCHDOG = (SRC / "StallWatchdog.cpp").read_text(encoding="utf-8")

GUI_GIT = (
    "runGitCapture(",
    "runGitCaptureDirect(",
    "gitRefResolves(",
    "gitOutput(",
    "mirrorBranchCommit(",
    "mirrorHeadBranch(",
    "upstreamPublishesThroughMirror(",
    "waitForGit(",
    "processEvents(",
)


def _body(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index("\n}\n", start) + 3]


def _flat(text: str) -> str:
    return re.sub(r"\s+", " ", text)


def _without_workers(body: str) -> str:
    out = []
    rest = body
    while "runOffThread<" in rest:
        head, tail = rest.split("runOffThread<", 1)
        out.append(head)
        depth = 0
        at = tail.index("{")
        for at in range(at, len(tail)):
            depth += {"{": 1, "}": -1}.get(tail[at], 0)
            if depth == 0:
                break
        rest = tail[at + 1 :]
    out.append(rest)
    return "".join(out)


def _assert_no_gui_git(body: str, where: str) -> None:
    for call in GUI_GIT:
        assert call not in body, f"{where} calls {call} on the GUI thread"


def test_repo_overview_reads_git_on_a_worker():
    load = _body(REPO_DETAIL, "void MainWindow::loadRepoOverview(const QString &path)")
    assert "runOffThread<OverviewLoad>(" in load
    assert "[read] { return readRepoOverview(read); }" in load
    _assert_no_gui_git(_without_workers(load), "loadRepoOverview")
    assert "GitKeepAlive" not in load

    placeholders = _body(REPO_DETAIL, "void MainWindow::showOverviewLoadingPlaceholders()")
    assert "processEvents(" not in placeholders


def test_repo_overview_worker_is_static_and_apply_is_git_free():
    assert "static OverviewLoad readRepoOverview(const OverviewRead &request);" in HEADER
    read = _body(REPO_DETAIL, "MainWindow::OverviewLoad MainWindow::readRepoOverview(")
    assert "runGitCapture(dir, {\"rev-parse\", ref}" in read
    assert "load.unchanged = true" in read
    assert "m_" not in re.sub(r"\bload\.\w+|request\.\w+", "", read)

    apply = _body(REPO_DETAIL, "void MainWindow::applyRepoOverview(")
    assert "generation != m_overviewLoadGen" in apply
    assert "request != overviewRequestFor(m_overviewPath)" in apply
    _assert_no_gui_git(apply, "applyRepoOverview")


def test_repo_overview_coalesces_a_repeat_request():
    load = _body(REPO_DETAIL, "void MainWindow::loadRepoOverview(const QString &path)")
    flat = _flat(load)
    assert (
        "if (m_overviewLoading && request == m_overviewLoadRequest) { "
        "m_overviewReloadPending = true; return; }"
    ) in flat
    assert "quint64 m_overviewLoadGen = 0;" in HEADER
    assert "bool m_overviewReloadPending = false;" in HEADER


def test_commit_history_and_unpushed_markers_load_on_a_worker():
    load = _body(REPO_DETAIL, "void MainWindow::loadCommits()")
    assert "runOffThread<CommitsLoad>(" in load
    assert "loadGen != m_commitsLoadGen" in load
    _assert_no_gui_git(_without_workers(load), "loadCommits")
    assert "GitKeepAlive" not in load

    read = _body(REPO_DETAIL, "MainWindow::CommitsLoad MainWindow::readCommits(")
    assert "load.mirrorTip + \"..\" + ref" in read
    assert "upstreamPublishesThroughMirror(dir, mirrorPath, relayHost)" in read

    apply = _body(REPO_DETAIL, "void MainWindow::applyCommits(")
    _assert_no_gui_git(apply, "applyCommits")
    assert "m_commitsLoadedMirrorTip = load.mirrorTip;" in apply

    assert "unpushedCommitHashes" not in HEADER + REPO_DETAIL
    assert "static CommitsLoad readCommits(" in HEADER


def test_commit_list_freshness_probe_never_waits_on_the_gui():
    for name in (
        "MainWindowRepoDetail.cpp",
        "MainWindowBranches.cpp",
        "MainWindowPulls.cpp",
        "MainWindow.h",
    ):
        assert "commitsListIsCurrent" not in (SRC / name).read_text(encoding="utf-8")
    refresh = _body(REPO_DETAIL, "void MainWindow::refreshCommitsUnlessCurrent()")
    assert "runOffThread<CommitMarkerProbe>(" in refresh
    _assert_no_gui_git(_without_workers(refresh), "refreshCommitsUnlessCurrent")
    markers = _body(REPO_DETAIL, "void MainWindow::refreshCommitMarkersIfStale()")
    _assert_no_gui_git(_without_workers(markers), "refreshCommitMarkersIfStale")


def test_load_more_restores_scroll_after_the_worker_answers():
    more = _body(REPO_DETAIL, "void MainWindow::loadMoreCommits()")
    assert "m_commitsLoadMoreScroll = scrollVal;" in more
    assert "setValue(" not in more
    load = _body(REPO_DETAIL, "void MainWindow::loadCommits()")
    assert "setValue(m_commitsLoadMoreScroll)" in load


def test_rebuild_source_scan_and_head_reads_run_on_workers():
    build = _flat(_body(SETUP, "void MainWindow::buildAndRelaunch("))
    assert (
        "runOffThread<QString>( [clientDir] { QString head; "
        "gitOutput(clientDir, {\"rev-parse\", \"--short\", \"HEAD\"}, &head); "
        "return head; }"
    ) in build
    assert (
        "runOffThread<QStringList>( [clientDir, started] { "
        "QStringList scan{sourceChangedSince(clientDir, started), QString()};"
    ) in build
    assert build.count("sourceChangedSince(") == 1
    assert build.count("gitOutput(") == 2


def test_event_delivery_probe_is_not_a_stall_origin():
    glue = _body(WATCHDOG, "bool isGlueFrame(const QString &symbol)")
    assert '"QtEventDeliveryProbe"' in glue
    assert '"QtRuntimeDiagnostics::noteDelivery"' in glue
