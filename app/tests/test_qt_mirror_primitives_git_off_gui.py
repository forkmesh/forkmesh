import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "desktop/src"
REPOS = (SRC / "MainWindowRepos.cpp").read_text(encoding="utf-8")
FETCH = (SRC / "MainWindowRepoFetch.cpp").read_text(encoding="utf-8")
ACTIONS = (SRC / "MainWindowActions.cpp").read_text(encoding="utf-8")
SETTINGS = (SRC / "MainWindowRepoSettings.cpp").read_text(encoding="utf-8")
REPO_DETAIL = (SRC / "MainWindowRepoDetail.cpp").read_text(encoding="utf-8")
HEADER = (SRC / "MainWindow.h").read_text(encoding="utf-8")

GUI_GIT = (
    "runGitCapture(",
    "runGitCaptureInput(",
    "gitCaptureStdout(",
    "cachedGitConfigLocal(",
    "cachedRepoRemotesRaw(",
    "cachedWorktreeListPorcelain(",
    "gitRepositoryIdentity(",
    "gitOriginUrl(",
    "stampGitRepositoryIdentity(",
    "validateGitSourceIdentity(",
    "validateExistingMirrorIdentity(",
    "mirrorOpenIssueCount(",
    "mirrorIssueCount(",
    "readRepoRemotes(",
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


def test_mirror_sync_association_is_checked_on_a_worker():
    check = _body(REPOS, "bool MainWindow::validateMirrorSyncAssociation(int index, bool quiet)")
    assert "runOffThread<QString>(" in check
    worker = check[check.index("runOffThread<QString>(") :]
    assert "validateGitSourceIdentity(repo, source)" in worker
    assert "validateExistingMirrorIdentity(repo, source)" in worker
    assert "stampGitRepositoryIdentity(it.key(), it.value())" in worker
    _assert_no_gui_git(_without_workers(check), "validateMirrorSyncAssociation")

    flat = _flat(check)
    assert "if (m_mirrorSyncAssociationPassed.remove(request)) return true;" in flat
    assert "running.value() = running.value() && quiet;" in flat
    assert (
        "return mirrorSyncAssociationRequest( current, repositorySource(current)) == "
        "request;"
    ) in flat
    assert flat.index("m_mirrorSyncAssociationPassed.insert(request);") < flat.index(
        "syncRepository(at, quietSync);"
    ) < flat.index("m_mirrorSyncAssociationPassed.remove(request); });")

    apply = _body(REPOS, "bool MainWindow::applyMirrorSyncAssociation(int index, const QString &reason)")
    _assert_no_gui_git(apply, "applyMirrorSyncAssociation")
    assert "Mirror sync quarantined for record" in apply

    sync = _body(FETCH, "void MainWindow::syncRepository(int index, bool quiet)")
    assert "if (!validateMirrorSyncAssociation(index, quiet))" in sync
    assert "bool validateMirrorSyncAssociation(int index, bool quiet);" in HEADER


def test_sync_repository_test_hook_waits_for_the_association_check():
    start = HEADER.index("void testSyncRepository(int index, bool quiet = true)")
    hook = _flat(HEADER[start : HEADER.index("\n    }\n", start)])
    assert "syncRepository(index, quiet);" in hook
    assert "while (!m_mirrorSyncAssociationChecks.isEmpty() && wait.elapsed() < 5000)" in hook


def test_identity_stamps_are_queued_onto_workers():
    stamp = _body(REPOS, "void MainWindow::stampRepositorySyncIdentity(const RepositoryRecord &repo)")
    assert "queueGitRepositoryIdentityStamp(local, repositoryIdentityKey(repo));" in stamp
    _assert_no_gui_git(stamp, "stampRepositorySyncIdentity")

    queue = _body(REPOS, "void MainWindow::queueGitRepositoryIdentityStamp(")
    assert "runOffThread<bool>(" in queue
    assert "stampGitRepositoryIdentity(dir, identity);" in queue
    _assert_no_gui_git(_without_workers(queue), "queueGitRepositoryIdentityStamp")
    assert "if (m_pendingIdentityStamps.value(dir) == identity)" in queue

    adopt = REPOS[
        REPOS.index("void MainWindow::adoptMaterializedMirror(") : REPOS.index(
            "void MainWindow::saveRepositories()"
        )
    ]
    assert adopt.count("queueGitRepositoryIdentityStamp(") == 2
    _assert_no_gui_git(adopt, "adoptMaterializedMirror")


def test_post_fetch_identity_stamp_runs_in_the_tidy_worker():
    fetch = _body(FETCH, "void MainWindow::startSyncFetch(")
    worker = fetch[fetch.index("QThread *worker = QThread::create(") :]
    worker = worker[: worker.index("connect(worker, &QThread::finished")]
    assert "stampGitRepositoryIdentity(mirrorPath, identity);" in worker
    assert fetch.count("stampGitRepositoryIdentity(") == 1


def test_push_url_check_runs_on_a_worker():
    hook = ACTIONS[
        ACTIONS.index("void MainWindow::ensurePushHook(") : ACTIONS.index(
            "void MainWindow::ensureCommitSignalHook("
        )
    ]
    assert "self->runOffThread<QString>(" in hook
    worker = hook[hook.index("self->runOffThread<QString>(") :]
    assert 'QStringLiteral("get-url")' in worker
    assert 'QStringLiteral("set-url")' in worker
    _assert_no_gui_git(_without_workers(hook), "ensurePushHook")

    flat = _flat(hook)
    assert "if (m_pushUrlVerifying.contains(localPath)) {" in flat
    assert "m_pushUrlRecheck.insert(localPath, repo);" in flat
    assert "self->m_pushUrlVerifying.remove(localPath);" in flat
    assert "self->ensurePushHook( self->m_pushUrlRecheck.take(localPath));" in flat
    assert "mutable QHash<QString, QString> m_pushUrlVerifying;" in HEADER


def test_repo_issue_badge_reads_the_mirror_count_on_a_worker():
    badge = _body(REPO_DETAIL, "void MainWindow::updateRepoIssueCount()")
    assert "runOffThread<int>(" in badge
    worker = badge[badge.index("runOffThread<int>(") :]
    assert "mirrorIssueCount(mirrorPath, tip.branch)" in worker
    _assert_no_gui_git(_without_workers(badge), "updateRepoIssueCount")
    assert "gen != m_repoIssueCountGen" in badge


def test_remotes_table_reads_git_on_a_worker():
    reload = _body(SETTINGS, "void MainWindow::reloadRepoRemotesTable()")
    assert "runOffThread<QList<RepoRemoteRow>>(" in reload
    assert "return readRepoRemotes(gitDir);" in reload
    _assert_no_gui_git(_without_workers(reload), "reloadRepoRemotesTable")
    assert "if (generation != m_repoRemotesLoadGen || !m_repoRemotesTable)" in reload
    assert "quint64 m_repoRemotesLoadGen = 0;" in HEADER


def test_import_reads_origins_on_a_worker():
    add = _body(REPOS, "void MainWindow::promptAddRepository()")
    assert "runOffThread<QStringList>(" in add
    worker = add[add.index("runOffThread<QStringList>(") :]
    assert 'QStringLiteral("get-url")' in worker
    _assert_no_gui_git(_without_workers(add), "promptAddRepository")
    flat = _flat(add)
    assert "if (repoIndexFor(repo.owner, repo.name) >= 0) continue;" in flat
    assert "const QString originUrl = origins.value(i);" in flat


def test_remotes_table_hides_stale_rows_and_writes_on_a_worker():
    reload = _flat(_body(SETTINGS, "void MainWindow::reloadRepoRemotesTable()"))
    sync_part = reload[: reload.index("runOffThread<")]
    assert "m_repoRemotesTableDir.clear();" in sync_part
    assert "m_repoRemotesTable->setRowCount(0);" in sync_part
    assert "m_repoRemotesTable->setEnabled(false);" in sync_part
    assert "m_repoRemotesTableDir = gitDir; m_repoRemotesTable->setEnabled(true);" in reload

    write = _body(SETTINGS, "void MainWindow::runRepoRemoteWrite(")
    assert "runGitCapture(gitDir, step.first, nullptr, &error)" in write
    _assert_no_gui_git(_without_workers(write), "runRepoRemoteWrite")
    assert "m_repoRemoteWriteInFlight = false;" in write

    for signature in (
        "void MainWindow::promptAddRepoRemote()",
        "void MainWindow::promptEditRepoRemote()",
        "void MainWindow::deleteSelectedRepoRemote()",
    ):
        body = _body(SETTINGS, signature)
        _assert_no_gui_git(body, signature)
        assert "runRepoRemoteWrite(" in body
        assert "if (m_repoRemoteWriteInFlight) {" in body
        assert "repoRemoteGitDir(m_repositories.at(m_repoDetailIndex)) != gitDir" in body
    for signature in (
        "void MainWindow::promptEditRepoRemote()",
        "void MainWindow::deleteSelectedRepoRemote()",
    ):
        body = _body(SETTINGS, signature)
        assert body.index("if (!repoRemoteRowsAreCurrent(gitDir))") < body.index(
            "m_repoRemotesTable->item(row, 0)->text()"
        )


def test_association_apply_finds_a_moved_record_and_reports_a_removed_one():
    check = _flat(_body(REPOS, "bool MainWindow::validateMirrorSyncAssociation(int index, bool quiet)"))
    assert "int at = matches(index) ? index : -1;" in check
    assert "for (int i = 0; at < 0 && i < m_repositories.size(); ++i) if (matches(i)) at = i;" in check
    assert "const int moved = repoIndexFor(owner, name);" in check
    assert "syncRepository(moved, quietSync);" in check
    assert "} else if (!quietSync) { flashMessage(" in check
    assert "if (!applyMirrorSyncAssociation(at, reason)) return;" in check
    assert "syncRepository(at, quietSync);" in check
