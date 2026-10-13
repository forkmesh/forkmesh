import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "desktop/src"
NAVIGATION = (SRC / "MainWindowNavigation.cpp").read_text(encoding="utf-8")
RELEASES = (SRC / "MainWindowReleases.cpp").read_text(encoding="utf-8")
REPO_SYNC = (SRC / "MainWindowRepoSync.cpp").read_text(encoding="utf-8")
HEADER = (SRC / "MainWindow.h").read_text(encoding="utf-8")

GUI_GIT = (
    "runGitCapture(",
    "runGitCaptureInput(",
    "runGitCaptureBackgrounded(",
    "gitCaptureStdout(",
    "gitRefResolves(",
    "mirrorStateHash(",
    "mirrorRoutingStateHash(",
    "branchHeadsFromGit(",
    "mirrorHasCommit(",
    "gitCommitIdentity(",
    "gatherMirrorSelfSnapshot(",
    "relayPublishRepo(",
    "mirrorArtifactCount(",
    "mirrorBranchCommit(",
    "mirrorHeadBranch(",
    "upstreamPublishesThroughMirror(",
    "waitForGit(",
    "processEvents(",
    "GitKeepAlive",
)


def _body(source: str, signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index("\n}\n", start) + 3]


def _code(text: str) -> str:
    return re.sub(r"^\s*//.*$", "", text, flags=re.M)


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
    code = _code(body)
    for call in GUI_GIT:
        assert call not in code, f"{where} calls {call} on the GUI thread"


def test_mirror_nodes_panel_reads_no_git_on_the_gui_thread():
    load = _body(RELEASES, "void MainWindow::loadMirrorNodesPanel()\n{")
    _assert_no_gui_git(load, "loadMirrorNodesPanel")
    assert "requestMirrorPanelGitFacts(gitRequest);" in load
    assert "refreshMirrorSelfSnapshot(repo, selfKey);" in load
    assert "gitRequest.wantRefs = referenceRefs.stamp != referenceStamp;" in load
    assert "gitRequest.relations.append(pair);" in load
    assert "gitRequest.counts.append(args);" in load
    assert "gitRequest.identities.append(commit);" in load
    assert "Reading this repository's mirror state" in load


def test_mirror_nodes_git_facts_are_read_by_a_static_worker():
    assert (
        "static MirrorPanelGitFacts\n    readMirrorPanelGitFacts(const MirrorPanelGitRequest &request);"
        in HEADER
    )
    assert "static bool mirrorHasCommit(const QString &mirrorPath, const QString &commit);" in HEADER
    assert "m_mirrorCommitsPresent" not in HEADER
    read = _body(RELEASES, "MainWindow::readMirrorPanelGitFacts(const MirrorPanelGitRequest &request)")
    assert "mirrorRoutingStateHash(gitDir)" in read
    assert "mirrorStateHash(gitDir)" in read
    assert "branchHeadsFromGit(gitDir)" in read
    assert "mirrorHasCommit(gitDir, sourceOid)" in read
    assert '"--is-ancestor"' in read
    assert "gitCommitIdentity(request.identityDir, commit)" in read
    assert "m_" not in re.sub(r"\b(facts|request)\.\w+", "", _code(read))

    request = _body(RELEASES, "void MainWindow::requestMirrorPanelGitFacts(")
    assert "runOffThread<MirrorPanelGitFacts>(" in request
    assert "if (m_mirrorPanelGitInFlight) {" in request
    assert "m_mirrorPanelGitPending = true;" in request
    _assert_no_gui_git(_without_workers(request), "requestMirrorPanelGitFacts")

    apply = _body(RELEASES, "void MainWindow::applyMirrorPanelGitFacts(")
    _assert_no_gui_git(apply, "applyMirrorPanelGitFacts")
    assert "facts.gitDir == m_mirrorPanelGitDir" in apply
    assert "m_mirrorRelationUnknownAt.insert(it.key(), facts.stamp);" in apply

    has = _body(REPO_SYNC, "bool MainWindow::mirrorHasCommit(")
    assert "QMutexLocker" in has


def test_mirror_nodes_window_hooks_wait_for_the_worker():
    assert "loadMirrorNodesPanel();\n        testSettleMirrorNodesPanel();" in HEADER
    settle = _body(RELEASES, "void MainWindow::testSettleMirrorNodesPanel()")
    assert "m_mirrorPanelGitInFlight" in settle
    assert "m_mirrorSelfSnapshotsInFlight" in settle
    tests_only = RELEASES[RELEASES.index("#ifdef FORKMESH_WINDOW_TESTS") :]
    assert "void MainWindow::testSettleMirrorNodesPanel()" in tests_only
    for hook in ("void MainWindow::testSetMirrorSectionCatalog(",
                 "void MainWindow::testSetSshMirrorMetadata("):
        assert "testSettleMirrorNodesPanel();" in _body(RELEASES, hook)


def test_commit_event_hash_runs_on_a_worker():
    drain = _body(REPO_SYNC, "void MainWindow::drainNodeCommitEvent(")
    assert "runOffThread<QString>(" in drain
    assert "return mirrorStateHash(mirrorPath);" in drain
    assert "if (m_mirrorEventHashChecks.contains(wanted))" in drain
    assert "mirrorStateHash(" not in _code(_without_workers(drain))


def test_roster_behind_check_reads_git_on_a_worker():
    roster = _body(REPO_SYNC, "void MainWindow::syncMirrorsBehindRoster()")
    assert "runOffThread<QList<RosterBehindResult>>(" in roster
    assert "[inputs] { return readMirrorsBehindRoster(inputs); }" in roster
    assert "m_rosterBehindReadPending = true;" in roster
    assert "m_repositories.at(i).mirrorPath != result.mirrorPath" in roster
    _assert_no_gui_git(_without_workers(roster), "syncMirrorsBehindRoster")

    assert "static QList<RosterBehindResult>\n    readMirrorsBehindRoster(" in HEADER
    read = _body(REPO_SYNC, "MainWindow::readMirrorsBehindRoster(const QList<RosterBehindInput> &inputs)")
    assert "mirrorStateHash(repo.mirrorPath)" in read
    assert "mirrorHasCommit(repo.mirrorPath, m.commit)" in read
    assert "m_" not in _code(read)


def test_push_reads_its_target_on_a_worker():
    assert "static bool relayPublishRepo(const RepositoryRecord &repo," in HEADER
    assert "static PushTarget readPushTarget(const RepositoryRecord &repo," in HEADER

    push = _body(NAVIGATION, "void MainWindow::pushCurrentRepoUpstream()")
    assert "runOffThread<PushTarget>(" in push
    assert "[repo, relayHost] { return readPushTarget(repo, relayHost); }" in push
    assert "m_pushingRepos.insert(index);" in push
    _assert_no_gui_git(_without_workers(push), "pushCurrentRepoUpstream")

    resume = _body(NAVIGATION, "void MainWindow::continueRepoPush(")
    assert "m_repositories.at(index).localPath != repo.localPath" in resume
    assert "runOffThread<MarkSafeCommit>(" in resume
    assert "runOffThread<PushScan>(" in resume
    _assert_no_gui_git(_without_workers(resume), "continueRepoPush")
    assert "index == m_repoDetailIndex" in resume

    read = _body(NAVIGATION, "MainWindow::PushTarget MainWindow::readPushTarget(")
    assert "relayPublishRepo(repo, relayHost, nullptr, nullptr)" in read
    assert "m_" not in _code(read)
    relay = _body(NAVIGATION, "bool MainWindow::relayPublishRepo(")
    assert "configuredRelayHost()" not in relay
    assert "m_" not in _code(relay)


def test_source_converge_reads_remotes_on_its_worker():
    converge = REPO_SYNC[
        REPO_SYNC.index("void MainWindow::convergeSourceRepoFromMesh(int index, bool force)")
        : REPO_SYNC.index("void MainWindow::propagateRepoUpdate(int index)")
    ]
    gui, worker = converge.split("QThread *worker = QThread::create(", 1)
    assert "readRepoRemotes(" not in _code(gui)
    assert "for (const RepoRemoteRow &remote : readRepoRemotes(workTree))" in worker
    assert "baseRoutes = mirrorRoutes, managedRouteIdentities" in worker
    assert "appendMirrorRoute(mirrorRoutes, rawUrl, environment," in worker
    assert worker.index("addMirrorRoute(meshUrl)") < worker.index(
        "const QString before = mirrorRefsDigest(mirrorPath);"
    )


def test_private_sync_fingerprint_is_read_on_a_worker():
    sync = _body(
        REPO_SYNC, "void MainWindow::syncPrivateRepository(int index, bool quiet, bool idleChecked)"
    )
    assert "runOffThread<QString>(" in sync
    assert "[localSource] { return privateSyncRefsFingerprint(localSource); }" in sync
    assert "if (m_privateSyncIdleChecks.contains(localSource))" in sync
    assert "current.owner != owner || current.name != name" in sync
    assert "privateSyncRefsFingerprint(" not in _code(_without_workers(sync))
    idle = _body(REPO_SYNC, "bool MainWindow::privateSyncIdle(")
    assert "privateSyncRefsFingerprint(" not in idle

    seal = REPO_SYNC[REPO_SYNC.index("void MainWindow::syncPrivateRepositoryWithRecipients(") :]
    seal = seal[: seal.index("\n}\n")]
    assert "privateSyncLocalSource(current) == result->refsSource" in seal
    assert "? result->refsFingerprint" in seal


def test_mirror_panel_stamps_the_real_git_directory():
    load = _body(RELEASES, "void MainWindow::loadMirrorNodesPanel()\n{")
    assert "const QString referenceStamp = mirrorPanelRefsStamp(referenceGitDir);" in load
    stamp = _body(RELEASES, "static QString mirrorPanelRefsStamp(const QString &gitDir)")
    assert "gitCommonDirPath(gitDir)" in stamp
    assert "resolveGitDirPath(gitDir)" in stamp


def test_commit_event_for_a_moved_repository_is_requeued():
    drain = _body(REPO_SYNC, "void MainWindow::drainNodeCommitEvent(")
    apply = drain[drain.index("[this, wanted, state](QString hash) {") :]
    assert "m_pendingMirrorEventStates.insert(wanted, QString());" in apply
    assert "drainNodeCommitEvent(wanted);" in apply


def test_release_follow_ups_do_not_depend_on_the_open_repository():
    assert "void pruneReleaseTagsForRepo(int repoIndex, const QString &dir," in HEADER
    assert "void announceReleaseOnFediverse(int repoIndex, const QString &tag," in HEADER
    assert "void postReleaseToMastodon(int repoIndex, const QString &tag," in HEADER
    assert "announceReleaseOnFediverse(index, tag, releaseTitle, notes);" in RELEASES
    assert "postReleaseToMastodon(index, tag, releaseTitle, notes);" in RELEASES
    assert "pruneReleaseTagsForRepo(index, dir, tag);" in RELEASES
    assert "            if (current)\n                loadBranchesAndTags();" in RELEASES
    for fn in ("void MainWindow::announceReleaseOnFediverse(",
               "void MainWindow::postReleaseToMastodon("):
        assert "m_repoDetailIndex" not in _body(RELEASES, fn)


def test_releases_table_clears_when_another_repository_loads():
    load = _body(RELEASES, "void MainWindow::loadReleasesPanel()")
    assert "if (request != m_releasesShownRequest) {" in load
    assert "m_releasesTable->setRowCount(0);" in load
    assert "m_releasesShownRequest = request;" in load
