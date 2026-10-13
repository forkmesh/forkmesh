import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "desktop/src"
RELEASES = (SRC / "MainWindowReleases.cpp").read_text(encoding="utf-8")
HEADER = (SRC / "MainWindow.h").read_text(encoding="utf-8")
COMMANDS = (SRC / "MainWindowDesktopCommands.cpp").read_text(encoding="utf-8")

GUI_GIT = (
    "runGitCapture(",
    "runGitCaptureInput(",
    "runGitCaptureDirect(",
    "runGitCaptureBackgrounded(",
    "gitCaptureStdout(",
    "gitRefResolves(",
    "gitOutput(",
    "mirrorBranchCommit(",
    "mirrorHeadBranch(",
    "stampReleaseTarget(",
    "stampVersionOnBranch(",
    "stampVersionForRelease(",
    "worktreeForBranch(",
    "readReleaseDetail(",
    "readReleaseTags(",
    "readArtifactsPanel(",
    "pruneReleaseTagsIn(",
    "renderDiffHtml",
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
        at = tail.index("(")
        for at in range(at, len(tail)):
            depth += {"(": 1, ")": -1}.get(tail[at], 0)
            if depth == 0:
                break
        rest = tail[at + 1 :]
    out.append(rest)
    return "".join(out)


def _worker(body: str) -> str:
    start = body.index("runOffThread<")
    first = body.index("[", start)
    depth = 0
    at = body.index("{", first)
    for at in range(at, len(body)):
        depth += {"{": 1, "}": -1}.get(body[at], 0)
        if depth == 0:
            break
    return body[first : at + 1]


def _assert_no_gui_git(body: str, where: str) -> None:
    for call in GUI_GIT:
        assert call not in body, f"{where} calls {call} on the GUI thread"


def test_worker_reads_are_free_functions_without_gui_state():
    for signature in (
        "ArtifactsPanelRead readArtifactsPanel(const QString &mirrorPath)",
        "ReleaseTagsRead readReleaseTags(const QString &dir)",
        "ReleaseTagPrune pruneReleaseTagsIn(const QString &dir, const QString &keepTag)",
        "ReleaseDetailRead readReleaseDetail(const QString &dir, const QString &tag,",
    ):
        read = _body(RELEASES, signature)
        assert "m_" not in re.sub(r"\bread\.\w+|prune\.\w+", "", read), signature
        assert "this" not in read, signature
    detail = _body(RELEASES, "ReleaseDetailRead readReleaseDetail(")
    assert "renderDiffHtmlSplit(split," in detail
    assert "appearance);" in detail


def test_artifacts_panel_reads_git_on_a_worker_and_coalesces():
    load = _body(RELEASES, "void MainWindow::loadArtifactsPanel()")
    assert "runOffThread<ArtifactsLoad>(" in load
    assert "load.read = readArtifactsPanel(mirrorPath);" in load
    _assert_no_gui_git(_without_workers(load), "loadArtifactsPanel")
    assert "mirrorReleaseBlobs(" not in _without_workers(load)
    flat = _flat(load)
    assert (
        "if (m_artifactsLoading && request == m_artifactsLoadRequest) { "
        "m_artifactsReloadPending = true; return; }"
    ) in flat
    assert "if (generation != m_artifactsLoadGen) return;" in flat
    assert "if (request != artifactsMirrorRequest()) { loadArtifactsPanel(); return; }" in flat
    fill = _body(RELEASES, "void MainWindow::fillArtifactsTable(")
    _assert_no_gui_git(fill, "fillArtifactsTable")
    assert "quint64 m_artifactsLoadGen = 0;" in HEADER
    assert "bool m_artifactsReloadPending = false;" in HEADER


def test_releases_panel_reads_tags_on_a_worker_and_coalesces():
    load = _body(RELEASES, "void MainWindow::loadReleasesPanel()")
    assert "runOffThread<ReleaseTagsRead>(" in load
    assert "[dir] { return readReleaseTags(dir); }" in load
    _assert_no_gui_git(_without_workers(load), "loadReleasesPanel")
    flat = _flat(load)
    assert (
        "if (m_releasesLoading && request == m_releasesLoadRequest) { "
        "m_releasesReloadPending = true; return; }"
    ) in flat
    assert "if (generation != m_releasesLoadGen) return;" in flat
    assert "if (request != releasesLoadRequest()) { loadReleasesPanel(); return; }" in flat
    apply = _body(RELEASES, "void MainWindow::applyReleasesPanel(")
    _assert_no_gui_git(apply, "applyReleasesPanel")
    assert "QString::fromUtf8(tagsOut)" in apply


def test_release_tag_prune_deletes_on_a_worker():
    prune = _body(RELEASES, "void MainWindow::pruneReleaseTagsForRepo(")
    assert "runOffThread<ReleaseTagPrune>(" in prune
    assert "[dir, keepTag] { return pruneReleaseTagsIn(dir, keepTag); }" in prune
    _assert_no_gui_git(_without_workers(prune), "pruneReleaseTagsForRepo")
    assert "if (m_releaseTagPrunes.contains(key))" in prune
    assert "repoGitDir() == dir" in prune
    finish = _body(RELEASES, "void MainWindow::finishReleaseTagPrune(")
    _assert_no_gui_git(finish, "finishReleaseTagPrune")
    assert "if (current) {" in finish


def test_artifact_replication_resolves_the_mirror_branch_on_its_worker():
    replicate = _body(RELEASES, "void MainWindow::replicateReleaseArtifacts(int index)")
    _assert_no_gui_git(_without_workers(replicate), "replicateReleaseArtifacts")
    assert "const QString branch = mirrorHeadBranch(mirrorPath);" in _worker(replicate)


def test_release_mirror_push_resolves_the_tag_on_a_worker():
    wrapper = _body(RELEASES, "void MainWindow::pushReleaseToMirrors(const QString &tag)")
    assert "pushReleaseToMirrors(m_repoDetailIndex, tag);" in wrapper
    push = _body(RELEASES, "void MainWindow::pushReleaseToMirrors(int index, const QString &tag)")
    assert "runOffThread<QString>(" in push
    _assert_no_gui_git(_without_workers(push), "pushReleaseToMirrors")
    assert "if (m_releaseMirrorPushResolving.contains(key))" in push
    assert 'tagRef + QStringLiteral("^{commit}")' in _worker(push)
    cont = _body(RELEASES, "void MainWindow::continueReleasePushToMirrors(")
    _assert_no_gui_git(cont, "continueReleasePushToMirrors")
    assert "const int index = repoIndexFor(owner, name);" in cont
    assert "if (repo.localPath != localPath" in cont


def test_new_release_reads_before_and_tags_after_the_dialog_on_workers():
    prompt = _body(RELEASES, "void MainWindow::promptNewReleaseThen(")
    assert "runOffThread<QByteArray>(" in prompt
    _assert_no_gui_git(_without_workers(prompt), "promptNewReleaseThen")
    assert "if (m_newReleaseRunning) {" in prompt
    assert "if (request != releasesLoadRequest() || !repoHasWorkingTree()) {" in prompt
    assert "draftNewRelease(dir, branches, target, commandTag, latestTag, done);" in prompt
    assert "m_desktopCommandReleaseTag.clear();" in prompt

    draft = _body(RELEASES, "void MainWindow::draftNewRelease(")
    _assert_no_gui_git(_without_workers(draft), "draftNewRelease")
    assert "cutRelease(cut, [this, finish](bool created, const QString &detail) {" in draft
    assert draft.count("finish(false);") >= 2
    assert "finish(created);" in draft

    cut = _body(RELEASES, "void MainWindow::cutRelease(")
    assert "runOffThread<NewReleasePublish>(" in cut
    _assert_no_gui_git(_without_workers(cut), "cutRelease")
    worker = _worker(cut[cut.index("runOffThread<NewReleasePublish>(") :])
    assert "stampReleaseTarget(dir, targetRef, tag, &publish.skipped)" in worker
    assert worker.index('QStringLiteral("refs/tags/") + tag') < worker.index(
        "stampReleaseTarget(")
    assert worker.index("stampReleaseTarget(") < worker.index('"tag",')
    assert "QStringLiteral(\"fetch\"), dir," in worker
    assert '"rev-parse", "--verify", tag + "^{commit}"' in worker
    assert "m_" not in worker
    assert "const bool current = index >= 0 && index == m_repoDetailIndex &&" in cut
    assert "if (pruneTags && index >= 0)" in cut
    assert "done(true, detail.join(QLatin1Char(' ')));" in cut


def test_generated_notes_read_git_log_on_a_worker():
    draft = _body(RELEASES, "void MainWindow::draftNewRelease(")
    notes = draft[draft.index("connect(genNotesButton") : draft.index("auto *agentNotesButton")]
    assert "runOffThread<QByteArray>(" in notes
    _assert_no_gui_git(_without_workers(notes), "the Generate release notes button")
    assert "genNotesButton->setEnabled(false);" in notes
    assert "if (!notesGuard || !tagGuard)" in notes

    agent = _body(RELEASES, "void MainWindow::generateReleaseNotesWithAgent(")
    assert "runOffThread<QByteArray>(" in agent
    _assert_no_gui_git(_without_workers(agent), "generateReleaseNotesWithAgent")
    assert "if (button && !button->isEnabled())" in agent
    assert "if (!notesGuard)" in agent
    send = _body(RELEASES, "void MainWindow::sendReleaseNotesRequest(")
    _assert_no_gui_git(send, "sendReleaseNotesRequest")


def test_release_detail_reads_and_renders_on_a_worker():
    show = _body(RELEASES, "void MainWindow::showReleaseDetail(const QString &tag)")
    assert "runOffThread<ReleaseDetailRead>(" in show
    _assert_no_gui_git(_without_workers(show), "showReleaseDetail")
    flat = _flat(show)
    assert "if (m_releaseDetailLoading == request) return;" in flat
    assert "if (generation != m_releaseDetailGen) return;" in flat
    assert "if (!read.exists || repoGitDir() != dir) return;" in flat
    dialog = _body(RELEASES, "void MainWindow::openReleaseDetailDialog(")
    _assert_no_gui_git(dialog, "openReleaseDetailDialog")


def test_tag_delete_runs_on_a_worker_after_the_confirm():
    delete = _body(RELEASES, "void MainWindow::deleteTag(const QString &tag)")
    assert "runOffThread<QString>(" in delete
    _assert_no_gui_git(_without_workers(delete), "deleteTag")
    assert delete.index("QMessageBox::question(") < delete.index("runOffThread<")
    assert "if (m_releaseTagDeletes.contains(key))" in delete
    assert "if (repoGitDir() == dir)" in delete


def test_desktop_release_command_settles_when_the_draft_finishes():
    run = _body(COMMANDS, "void MainWindow::runDesktopCommand(")
    release = run[run.index("m_desktopCommandReleaseTag = command.tag;") :]
    release = release[: release.index("return;")]
    assert "promptNewReleaseThen(" in release
    assert "settleDesktopCommand(" in release
    assert "tagExists()" not in release
    assert "void promptNewReleaseThen(std::function<void(bool)> done);" in HEADER
