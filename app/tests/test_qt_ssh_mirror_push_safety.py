"""Safety contracts for unattended Qt-to-headless-mirror propagation."""

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
REPOS = "".join(
    (ROOT / "desktop/src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_REPOS_SOURCES
)
ACTIONS = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(
    encoding="utf-8"
)


def test_managed_push_cannot_rewind_durable_refs_and_prunes_only_after_reverse_sync():
    push = REPOS.split(
        "void MainWindow::pushToSshMirrorRemotes(int index)", 1
    )[1].split("\nvoid MainWindow::syncRepository", 1)[0]
    managed = push.split("int MainWindow::pushRefsToManagedHosts(", 1)[1].split(
        "\nvoid MainWindow::scheduleSshMirrorPushRetry", 1
    )[0]

    assert 'QStringLiteral("refs/heads/*:refs/heads/*")' in managed
    assert 'QStringLiteral("refs/tags/*:refs/tags/*")' in managed
    assert 'QStringLiteral("+refs/heads/*:refs/heads/*")' not in managed
    assert 'QStringLiteral("+refs/tags/*:refs/tags/*")' not in managed
    assert 'QStringLiteral("+refs/pr/*:refs/pr/*")' in managed
    assert 'QStringLiteral("refs/pr/*:refs/pr/*")' in managed
    assert 'QStringLiteral("--prune")' in managed
    assert 'QStringLiteral("--no-prune")' in managed
    assert "pruneRefs ?" in managed
    assert 'QStringLiteral("--no-force")' in managed


def test_automatic_ssh_mirror_push_uses_explicit_fast_forward_policy():
    push = REPOS.split(
        "void MainWindow::pushToSshMirrorRemotes(int index)", 1
    )[1].split("\nvoid MainWindow::syncRepository", 1)[0]
    command = push.split('process->start(QStringLiteral("git"),', 1)[1]

    # Durable wildcard refspecs remain unforced so Git rejects a stale main
    # instead of rewinding a gateway whose branch advanced independently.
    # Ordinary event pushes do not prune; a completed reverse-sync pass selects
    # pruning and may repair only the derived refs/pr compatibility cache.
    assert command.index('QStringLiteral("--no-force")') < command.index(
        'QStringLiteral("refs/heads/*:refs/heads/*")'
    )
    assert command.index('QStringLiteral("--no-prune")') < command.index(
        'QStringLiteral("refs/heads/*:refs/heads/*")'
    )
    assert 'QStringLiteral("--prune")' in command
    assert "pruneRefs ?" in command
    assert 'QStringLiteral("+refs/heads/' not in command
    assert 'QStringLiteral("+refs/tags/' not in command
    assert 'QStringLiteral("+refs/pr/*:refs/pr/*")' in command


def test_direct_push_fans_out_to_ssh_mirrors_immediately():
    spool = ACTIONS.split("void MainWindow::scanActionSpool()", 1)[1].split(
        "\nvoid MainWindow::syncMirrorActionsConfiguration", 1
    )[0]

    # A receive-pack that lands directly on the source's served bare mirror
    # bypasses syncPublicEncryptedRepository(), where normal SSH fanout occurs.
    # The post-receive spool consumer must therefore fan it out itself.
    assert "pushToSshMirrorRemotes(idx);" in spool
    assert "notifyMirrorUpdated(" not in spool


def test_transient_gateway_failure_queues_its_own_fast_retry():
    push = REPOS.split(
        "int MainWindow::pushToSshMirrorRemotes(int index, bool userInitiated",
        1,
    )[1].split("\nvoid MainWindow::syncRepository", 1)[0]
    failure = push.split("if (mirrorGatewayUnreachable(errors))", 1)[1]
    retry = failure.index("scheduleSshMirrorPushRetry(")
    finish = failure.index("finishPush(pushKey, false, false);")
    assert retry < finish
    assert "QStringList{gatewayHost}" in failure[:finish]


def test_successful_source_safety_sync_refans_current_refs():
    sync = REPOS.split("void MainWindow::startSyncFetch", 1)[1].split(
        "\n\nvoid MainWindow::onProfileNameChanged", 1
    )[0]
    success = sync.split("if (exitCode == 0)", 1)[1].split("} else {", 1)[0]
    fanout = (
        "pushToSshMirrorRemotes(\n"
        "                                index, /*wakeManagedIfUnchanged=*/false);"
    )
    assert fanout in success
    assert success.index(fanout) < success.index(
        "completePendingRepoAutoOpen(index);"
    )
