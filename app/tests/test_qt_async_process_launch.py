"""Periodic SSH and detached Git launches must never spawn on the GUI thread."""

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
BRANCHES = (ROOT / "desktop/src/MainWindowBranches.cpp").read_text(
    encoding="utf-8"
)
CHAT = "".join(
    (ROOT / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_CHAT_SOURCES
)
INTERNAL = (ROOT / "desktop/src/MainWindowInternal.h").read_text(
    encoding="utf-8"
)


def _host_probe() -> str:
    return CHAT.split("void MainWindow::probeSavedHost(", 1)[1].split(
        "\nvoid MainWindow::installAgentClisForHost", 1
    )[0]


def _detached_git_start() -> str:
    # The first occurrence is the namespace forward declaration.
    return BRANCHES.split("void startDetachedGit(DetachedGitRequest request)", 2)[
        2
    ].split("\n}\n\n} // namespace", 1)[0]


def _worker_process() -> str:
    return INTERNAL.split("inline WorkerProcessResult runWorkerProcess(", 1)[
        1
    ].split("\ninline bool waitForGit", 1)[0]


def test_process_lifetime_is_worker_only_and_bounded():
    worker = _worker_process()
    assert "QProcess process;" in worker
    assert "process.start(request.program, request.arguments)" in worker
    assert "process.waitForFinished" in worker
    assert "process.kill()" in worker
    assert "cancelled->load(std::memory_order_relaxed)" in worker
    assert "elapsed.hasExpired(request.timeoutMs)" in worker
    assert "process.readAllStandardOutput()" in worker
    assert "process.readAllStandardError()" in worker
    assert "maxOutputBytes" in INTERNAL


def test_saved_host_probe_launches_ssh_off_the_gui_and_keeps_contracts():
    probe = _host_probe()
    assert "QFutureWatcher<WorkerProcessResult>" in probe
    assert "QtConcurrent::run" in probe
    assert "runWorkerProcess(processRequest, cancelled)" in probe
    assert "processRequest.timeoutMs = 35000" in probe
    assert "processRequest.maxOutputBytes = 4096" in probe
    assert "processRequest.channelMode = QProcess::MergedChannels" in probe
    assert "std::atomic_bool" in probe
    assert "m_hostProbesInFlight.contains(key)" in probe
    assert "m_hostProbesInFlight.remove(key)" in probe
    assert "result.timedOut" in probe
    assert "result.started" in probe
    assert "new QProcess" not in probe
    assert "process->start" not in probe


def test_detached_git_keeps_fifo_limit_cancellation_and_gui_callback_guards():
    launch = _detached_git_start()
    assert "QFutureWatcher<WorkerProcessResult>" in launch
    assert "QtConcurrent::run" in launch
    assert "runWorkerProcess(processRequest, cancelled, source)" in launch
    assert "processRequest.timeoutMs = kDetachedGitTimeoutMs" in launch
    assert "QCoreApplication::instance()" in launch
    assert "QPointer<MainWindow>" in BRANCHES
    assert "if (window && onDone)" in launch
    assert "--g_detachedGitInFlight" in launch
    assert "pumpDetachedGitQueue()" in launch
    assert "kMaxDetachedGitInFlight = 12" in BRANCHES
    assert "kDetachedGitTimeoutMs = 60000" in BRANCHES
    assert "new QProcess" not in launch
    assert "git->start" not in launch


def test_existing_consumers_still_reject_stale_results():
    assert "statusGen != m_worktreeStatusGen" in BRANCHES
    repo_detail = (ROOT / "desktop/src/MainWindowRepoDetail.cpp").read_text(
        encoding="utf-8"
    )
    assert "gen != m_commitLoadGen" in repo_detail
    source_control = (
        ROOT / "desktop/src/MainWindowSourceControl.cpp"
    ).read_text(encoding="utf-8")
    assert "generation != m_scmStatusGeneration" in source_control
    assert "m_repoDetailIndex != forIndex" in source_control
