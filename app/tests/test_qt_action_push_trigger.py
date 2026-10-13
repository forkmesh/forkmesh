"""Push-triggered Actions must fire for commits that land through the app.

Workflows with `on: [push]` — "CI tests" and "Deploy Cloudflare Worker" — used
to be queued only from the served bare mirror's post-receive hook. That hook
runs for a real `git push` into the mirror and nothing else, but every commit
this app makes lands in the working copy and reaches the served mirror through
syncRepository's `git fetch`, which runs no hooks at all. Merges made in-app
therefore queued no runs and the Worker stopped being deployed.

The fix watches the served mirror's own default branch, so the trigger no longer
depends on which transport moved it.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ACTIONS = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(
    encoding="utf-8"
)
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")
DEPLOY = (ROOT / ".forkmesh/deploy.yml").read_text(encoding="utf-8")


def _watcher() -> str:
    return ACTIONS.split(
        "void MainWindow::scanServedMirrorHeads(", 1
    )[1].split("\n}\n", 1)[0]


def _spool_sweep() -> str:
    return ACTIONS.split("void MainWindow::scanActionSpool()", 1)[1].split(
        "\nvoid MainWindow::scanServedMirrorHeads", 1
    )[0]


def test_deploy_workflow_is_never_reached_by_push():
    assert DEPLOY.split("\n", 3)[1] == "on: [workflow_dispatch]"
    assert "name: Deploy Cloudflare Worker" in DEPLOY
    steps = DEPLOY.split("    steps:\n", 1)[1]
    first = steps.split("      - name:", 2)[1]
    assert first.startswith(" Refuse Cloudflare deploys from this repository")
    assert "exit 1" in first


def test_control_deploy_runs_the_local_orchestrator():
    assert "on: [workflow_dispatch]" in DEPLOY
    control = (ROOT / "desktop/src/MainWindowControlNode.cpp").read_text(
        encoding="utf-8"
    )
    run = control.split("void MainWindow::runSiteDeploy(", 1)[1].split(
        "void MainWindow::setSiteDeployRunning", 1
    )[0]
    assert "findSiteDeployScript(" in run
    # The argument list is built once, so the Force control can add deploy.sh's
    # --force flag without a second start() call growing its own rules.
    assert (
        "forkmesh::control::siteDeployArguments(script, deployTarget, force)"
        in run
    )
    assert "process->start(bash, arguments);" in run
    assert 'm_siteDeployForceButton->setChecked(false);' in run
    assert "QProcess::readyReadStandardOutput" in run
    assert "m_siteDeployCancelButton->setEnabled(true)" in run
    assert "runSelectedWorkflowManually();" not in run


def test_spool_sweep_watches_the_served_mirror_head():
    sweep = _spool_sweep()
    assert "scanServedMirrorHeads(pushedCommits);" in sweep
    # The watcher runs before the queue is drained, so a commit it discovers
    # starts in the same sweep rather than waiting for the next tick.
    assert sweep.index("scanServedMirrorHeads(pushedCommits);") < sweep.index(
        "processActionQueue();"
    )
    assert "void scanServedMirrorHeads(const QHash<int, QString> &" in HEADER


def test_a_real_post_receive_event_is_not_queued_twice():
    # The hook path records what it reported so the watcher can tell "already
    # queued in this sweep" from "new", instead of doubling every run whenever
    # someone does push into the served mirror by hand.
    sweep = _spool_sweep()
    assert "pushedCommits.insert(pushedIndex, commit.toLower());" in sweep
    watcher = _watcher()
    assert "m_servedMirrorHeadPushedCommits[it.key()].insert(it.value())" in watcher
    assert ".value(result.index)" in watcher
    assert ".contains(result.commit)" in watcher


def test_served_head_git_probes_never_block_the_gui_thread():
    watcher = _watcher()
    assert "QFutureWatcher<QList<ServedMirrorHeadResult>>" in watcher
    assert "QtConcurrent::run" in watcher
    worker = watcher.split("QtConcurrent::run", 1)[1]
    assert "gitHeadRef(probe.mirrorPath)" in worker
    assert "gitCommitAt(probe.mirrorPath, result.ref)" in worker
    ui_prefix = watcher.split("QtConcurrent::run", 1)[0]
    assert "gitHeadRef(repo.mirrorPath)" not in ui_prefix
    assert "gitCommitAt(repo.mirrorPath" not in ui_prefix


def test_served_head_scans_are_single_flight_and_coalesced():
    watcher = _watcher()
    assert "m_servedMirrorHeadScanInFlight" in watcher
    assert "m_servedMirrorHeadScanPending = true" in watcher
    assert "generation == m_servedMirrorHeadScanGeneration" in watcher
    assert "scanServedMirrorHeads({})" in ACTIONS


def test_served_head_checkpoint_sync_is_off_the_gui_and_precedes_enqueue():
    watcher = _watcher()
    persist_callback = watcher.split("connect(persistWatcher", 1)[1].split(
        "persistWatcher->setFuture", 1
    )[0]
    persist_worker = watcher.split(
        "persistWatcher->setFuture(QtConcurrent::run", 1
    )[1]
    assert "enqueuePushEvent" in persist_callback
    assert "settings.sync()" not in persist_callback
    assert "QSettings settings" in persist_worker
    assert "settings.sync()" in persist_worker
    before_persist = watcher.split("persistWatcher->setFuture", 1)[0]
    assert "settings.sync()" not in before_persist


def test_only_the_working_copy_holder_queues_workflows():
    watcher = _watcher()
    # A pure mirror replicates whatever the source serves. Queueing there too
    # would fan a single merge out into a duplicate run on every mirror node.
    assert "repo.localPath.trimmed().isEmpty()" in watcher
    assert "repo.previewOnly" in watcher
    assert "!repo.actionsEnabled" in watcher
    # Gateway-managed Actions mirrors keep their own trigger elsewhere.
    assert "repo.externallyManagedActions" in watcher


def test_first_sight_of_a_branch_does_not_replay_its_history():
    watcher = _watcher()
    persist_worker = watcher.split(
        "persistWatcher->setFuture(QtConcurrent::run", 1
    )[1]
    assert "settings.setValue(item.settingsKey, item.result.commit)" in persist_worker
    persist_callback = watcher.split("connect(persistWatcher", 1)[1].split(
        "persistWatcher->setFuture", 1
    )[0]
    first_run = persist_callback.index("item.previousCommit.isEmpty()")
    queue = persist_callback.index("enqueuePushEvent(repo.owner")
    # QFutureWatcher invokes this callback only after its worker has persisted
    # the tip, and a first sight never queues the branch's existing history.
    assert first_run < queue
    # ...and never treat the branch's whole history as one enormous push.
    assert "item.previousCommit == result.commit" in persist_callback


def test_watcher_survives_a_mirror_that_is_being_re_sealed():
    watcher = _watcher()
    # An encrypted repository is served from a temporary materialization that
    # every seal replaces, so both the path and the ref lookup can be missing
    # for a moment. Neither may queue a run against an empty commit.
    assert 'QDir(repo.mirrorPath).exists()' in watcher
    assert "result.ref.isEmpty()" in watcher
    assert "result.commit.isEmpty()" in watcher
    # The remembered tip is keyed by repository, not by that unstable path.
    assert "QString servedActionHeadKey(const RepositoryRecord &repo)" in ACTIONS
    assert "actions/servedHeads/" in ACTIONS


def test_head_ref_lookup_rejects_a_detached_or_bogus_head():
    lookup = ACTIONS.split("QString gitHeadRef(const QString &repository)", 1)[
        1
    ].split("\n}\n", 1)[0]
    assert 'QStringLiteral("symbolic-ref")' in lookup
    assert 'QStringLiteral("--quiet")' in lookup
    # Reuses the same refs/heads/... validation the external-source watcher uses.
    assert "kExternalActionRef.match(ref).hasMatch()" in lookup


def test_stale_runner_completion_cannot_fail_or_unpin_a_running_run():
    completion = ACTIONS.split(
        "void MainWindow::onRunFinished(int runId, bool ok)", 1
    )[1].split("\nvoid MainWindow::onReleaseMetadataLanded", 1)[0]
    terminal_guard = completion.index(
        "stored->status != ActionStatus::Success"
    )
    unpin = completion.index("releaseActionMirrorPin(runId);")
    notification = completion.index("notifyActionEvent(title, body")
    assert terminal_guard < unpin < notification
    assert "const ActionRun run = *stored;" in completion
    assert "const bool succeeded = run.status == ActionStatus::Success;" in completion
    assert "notifyActionEvent(title, body, !succeeded" in completion
