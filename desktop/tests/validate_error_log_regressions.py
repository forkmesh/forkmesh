#!/usr/bin/env python3
"""Guard fixes for desktop failures that previously reached the shared log."""

import re
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

ROOT = Path(__file__).resolve().parents[1]
AGENTS = "".join(
    (ROOT / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_AGENT_SOURCES
)
ACTION = (ROOT / "src/MainWindowActions.cpp").read_text(encoding="utf-8")
PULLS = (ROOT / "src/MainWindowPulls.cpp").read_text(encoding="utf-8")
REPOS = "".join(
    (ROOT / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_REPOS_SOURCES
)
WINDOW = (ROOT / "src/MainWindow.cpp").read_text(encoding="utf-8")
PERMISSIONS = (ROOT / "src/MainWindowCommandPermissions.cpp").read_text(
    encoding="utf-8")
DIAGNOSTICS = (ROOT / "src/MainWindowDiagnostics.cpp").read_text(
    encoding="utf-8")


def between(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def main() -> None:
    push_hook = between(
        ACTION,
        "void MainWindow::ensurePushHook(",
        "void MainWindow::ensureCommitSignalHook(",
    )
    private_guard = push_hook.index(
        "PrivateMirrorStore::isOpaqueId(repo.privateReplicaId)")
    push_url = push_hook.index('QStringLiteral("--push")')
    assert private_guard < push_url
    assert "ensureCommitSignalHook(repo)" in push_hook[:push_url]

    pull_cleanup = between(
        PULLS,
        "void MainWindow::deletePullAndBranchAsync(",
        "QUrl MainWindow::pullsApiUrl",
    )
    delete_pull = pull_cleanup.index("store.deletePull(")
    branch_probe = pull_cleanup.index("repoHasLocalBranch(dir, head)")
    assert branch_probe > delete_pull
    assert "forkmesh::kLocalOperationNoticeKind" in pull_cleanup

    adopt_mirror = between(
        REPOS,
        "void MainWindow::adoptMaterializedMirror(",
        "void MainWindow::saveRepositories()",
    )
    assert adopt_mirror.count("queueGitRepositoryIdentityStamp(") == 2
    assert "stampGitRepositoryIdentity(" not in adopt_mirror
    assert adopt_mirror.count("repositoryIdentityKey(repo).toLower()") == 2

    source_converge = between(
        REPOS,
        "void MainWindow::convergeSourceRepoFromMesh(",
        "void MainWindow::propagateRepoUpdate(",
    )
    assert "if (mirrorRoutes.isEmpty())" in source_converge
    fallback = source_converge.index("addMirrorRoute(meshUrl)")
    exact_routes = source_converge.index("loadSavedHosts(")
    assert fallback > exact_routes

    mirror_failure = between(
        REPOS,
        'QStringLiteral("cannot lock ref")',
        "const bool transient = isTransientSyncError(errors);",
    )
    assert "forkmesh::upstream::refsNamedAsBadObjects(errors)" in mirror_failure
    repair = mirror_failure.index("QThread::create(")
    drop_ref = mirror_failure.index("forkmesh::upstream::dropUnreadableRefs(")
    assert repair < drop_ref < mirror_failure.index("resyncFailedRepo();")
    assert "refRaceAttempt < kUnreadableRefRepairs" in mirror_failure
    assert "m_mirrorSkippedRefs[failedKey]" in mirror_failure
    assert "stableRefspecs.append(QLatin1Char('^') + ref.key());" in REPOS

    # The QWidget base destructor deletes whatever children are left — the
    # long-lived dialogs among them — after every MainWindow member has been
    # destroyed, and emits destroyed() for each one while this window's
    # connections are still live. A handler bound to the window then cleared
    # containers whose heap blocks had just been freed, aborting the process
    # inside free() under the "MainWindow teardown" crash context. Severing
    # those paths has to be the last thing the destructor body does: anything
    # added after it would run with no protection at all.
    teardown = between(WINDOW, "MainWindow::~MainWindow()", "void MainWindow::destroyTransientOverlays(")
    sever = teardown.index("QObject::disconnect(descendant, nullptr, this, nullptr)")
    assert teardown.index("findChildren<QObject *>()") < sever
    assert teardown.index("m_diffViews.clear()") < sever
    tail = teardown[sever:]
    assert tail.count("connect(") == tail.count("disconnect(")

    # startCliTranscript rebuilds m_agentSessions (showAgentSession, then
    # reloadAgents) partway through a launch. The queue pump handed it a
    # reference into that list, so the no-worktree branch choice read a freed
    # session and faulted in QString::operator= (SIGSEGV 2026-09-18 16:09:07).
    # The pump passes a copy like the other launchers, and nothing past the
    # rebuild reads the session parameter.
    queue_pump = between(
        AGENTS,
        "void MainWindow::processAgentQueue()",
        "void MainWindow::onClaudeOpenDiff(",
    )
    assert "startCliTranscript(prepared," in queue_pump
    assert "startCliTranscript(*session" not in queue_pump
    cli_start = between(
        AGENTS,
        "void MainWindow::startCliTranscript(",
        "forkmesh::agentguard::Config MainWindow::cliTranscriptGuardrails(",
    )
    rebuilt = cli_start[cli_start.index("showAgentSession(sid);"):]
    code = "\n".join(line.split("//")[0] for line in rebuilt.splitlines())
    assert re.search(r"\bsession\s*(\.|->)", code) is None, code
    assert re.search(r"\bcustomPrompt\b", code) is None, code

    resolver = between(
        PERMISSIONS,
        "void MainWindow::installAgentCommandApprovals()",
        "void MainWindow::showAgentCommandPermissions(",
    )
    assert "sessionWorkdir(" not in resolver, resolver
    spins = min(
        resolver.index(call)
        for call in ("disabledAgentToolFamilies()", "loadAgentAccess(",
                     "classifyCommand(")
    )
    after_spin = "\n".join(
        line.split("//")[0] for line in resolver[spins:].splitlines())
    assert re.search(r"\bsession\s*(\.|->)", after_spin) is None, after_spin
    assert re.search(r"findAgentSession\(", after_spin) is None, after_spin

    footer_diagnostics = between(
        DIAGNOSTICS,
        "void MainWindow::updateFooterDiagnostics()",
        "void MainWindow::checkFileDescriptorPressure()",
    )
    assert "showHighMemoryProcessPanel(false)" in footer_diagnostics


if __name__ == "__main__":
    main()
