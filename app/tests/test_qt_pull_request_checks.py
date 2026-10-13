"""Checks have to run on pull requests, and untrusted authors have to be gated.

Nothing ever queued a workflow for a pull request. `on: [push]` fires from the
served mirror's post-receive hook (and the mirror-head watcher beside it), but a
pull request's head is a branch nobody pushed there — for a submission that
arrived through the relay it is not even a branch until PullStore materializes
one — so the Checks tab was empty until somebody pressed "Run checks against
this PR", and even that queued nothing when the head was missing from the mirror
the runner checks out of.

The pass added here publishes each open pull request's head into the served
mirror and queues the workflows that opted in with `on: pull_request`. Because
that executes a contributor's own shell commands on this node, a first-time
author — nobody who has landed a merge here — has every one of their pull
requests held for the maintainer, whatever the repository's auto-approve setting
says.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
ACTIONS = (ROOT / "desktop/src/MainWindowActions.cpp").read_text(encoding="utf-8")
PULLS = (ROOT / "desktop/src/MainWindowPulls.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")
ACTION_FILE = (ROOT / "desktop/src/ActionFile.h").read_text(encoding="utf-8")
ACTION_STORE = (ROOT / "desktop/src/ActionStore.h").read_text(encoding="utf-8")
PULL_STORE = (ROOT / "desktop/src/PullStore.cpp").read_text(encoding="utf-8")
CI = (ROOT / ".forkmesh/ci.yml").read_text(encoding="utf-8")
DESKTOP_CI = (ROOT / ".forkmesh/desktop-ci.yml").read_text(encoding="utf-8")
DEPLOY = (ROOT / ".forkmesh/deploy.yml").read_text(encoding="utf-8")


def _function(source: str, signature: str) -> str:
    return source.split(signature, 1)[1].split("\n}\n", 1)[0]


def test_this_repository_runs_its_tests_on_pull_requests():
    # The whole path below is useless to this repository unless its own test
    # workflow opts in to the event.
    assert CI.startswith("name: CI tests\non: [push, pull_request]\n")
    # The Qt client suite is a second workflow (it is kept out of the deploy's
    # `needs:`), and a pull request has to reach it too.
    assert DESKTOP_CI.startswith(
        "name: Desktop client CI tests\non: [push, pull_request]\n"
    )


def test_a_deploy_never_runs_a_proposed_head():
    # Deploys stay push/manual-only, and the automatic pass matches nothing
    # else, so merely opening a pull request can never ship its head.
    assert "on: [workflow_dispatch]" in DEPLOY
    assert "pull_request" not in DEPLOY
    scan = _function(ACTIONS, "void MainWindow::applyWorkflowScan(")
    automatic = scan.split("case WorkflowTrigger::PullRequest:", 1)[1].split(
        "break;", 1
    )[0]
    assert "matchesTrigger = runsOnPullRequests;" in automatic
    assert "triggersOnPush" not in automatic
    # The button keeps its older meaning of "run this repository's checks",
    # which is why it is a separate trigger rather than the same one.
    manual = scan.split("case WorkflowTrigger::PullRequestManual:", 1)[1].split(
        "break;", 1
    )[0]
    assert "runsOnPullRequests || wf.triggersOnPush()" in manual


def test_the_base_branch_also_decides_whether_a_workflow_runs_on_pulls():
    # Workflows are read from the proposed head, so a pull request opened before
    # the repository adopted the event would never be checked — and a
    # contributor could switch their own checks off by editing the trigger.
    # The base branch's copy of the same file answers that question too.
    scan = _function(ACTIONS, "void MainWindow::applyWorkflowScan(")
    assert (
        "wf.triggersOnPullRequest() || entry.baseTriggersPullRequest" in scan
    )
    queue = ACTIONS.split(
        "void MainWindow::queueWorkflowsForCommit(int repoIndex, "
        "const QString &owner,\n                                         "
        "const QString &name, const QString &commit,",
        1,
    )[1].split("\n}\n", 1)[0]
    # The base ref is interpolated into a git revision and arrives inside a pull
    # request written by somebody else.
    assert 'QStringLiteral("^[A-Za-z0-9._/-]{1,200}$")' in queue
    assert 'QStringLiteral("refs/heads/") + pull.base.trimmed()' in queue
    # What runs is still the head's copy: that is the tree the runner checks out
    # and the state the maintainer's approval is bound to.
    assert "entry.content = content;" in queue
    assert (
        "entry.baseTriggersPullRequest =\n                        "
        "!baseContent.isEmpty() &&" in queue
    )


def test_pull_request_is_a_parsed_trigger_in_both_yaml_forms():
    assert 'on.contains(QStringLiteral("pull_request"))' in ACTION_FILE
    parser = (ROOT / "desktop/src/ActionFile.cpp").read_text(encoding="utf-8")
    # `on:` accepts a mapping whose keys are the events; reading only scalars
    # there leaves a filtered workflow triggering on nothing.
    assert 'wf.on = eventNames(root.child(QStringLiteral("on")));' in parser
    events = _function(parser, "QStringList eventNames(const Node *node)")
    assert "node->type == Node::Map" in events
    assert "it.key().trimmed()" in events


def test_the_head_is_published_into_the_mirror_the_runner_uses():
    resolve = _function(ACTIONS, "void MainWindow::resolvePullCheckHeads(")
    # The runner does `git -C <mirror> worktree add --detach <commit>`, so a head
    # that only exists in the working copy has to be pushed, under a ref: a
    # dangling object is one garbage collection from vanishing mid-run.
    assert 'QStringLiteral("refs/heads/pr/%1").arg(candidate.number)' in resolve
    assert 'QStringLiteral("+%1:%2").arg(head.commit, head.ref)' in resolve
    assert "head.published = true;" in resolve
    # Resolution order: the signed head branch, then the durable pr/<n> refs.
    assert 'QStringLiteral("refs/pr/%1/head").arg(candidate.number)' in resolve


def test_an_equivalent_reconciled_check_head_is_not_rewound_on_startup():
    resolve = _function(ACTIONS, "void MainWindow::resolvePullCheckHeads(")
    assert "const QString publishedHead = resolve(mirror, head.ref);" in resolve
    assert "publishedTree == requestedTree" in resolve
    assert "!requestedTree.isEmpty()" in resolve
    assert "mergeBase(mirror, head.commit, publishedHead)" in resolve
    assert "head.commit = publishedHead;" in resolve
    assert 'QStringLiteral("--force-with-lease=%1:%2")' in resolve
    assert resolve.index("head.commit = publishedHead;") < resolve.index(
        'QStringLiteral("+%1:%2").arg(head.commit, head.ref)'
    )


def test_a_patch_only_submission_gets_a_head_to_check_out():
    # A pull request that arrived through the relay is a signed patch, not a
    # branch: refs/pr/<n>/head does not exist until PullStore replays its
    # commits onto the base. Without this, every cross-node pull request
    # resolved to no commit and was silently skipped.
    resolve = _function(ACTIONS, "void MainWindow::resolvePullCheckHeads(")
    assert "store.ensurePullHeadRef(candidate.pull, &head.error);" in resolve
    assert "materialized < kPullCheckMaterializeLimit" in resolve
    assert "++materialized;" in resolve
    # It only runs when nothing else resolved, and it is bounded per pass —
    # a scratch worktree plus a `git am` each.
    assert "head.commit = firstResolved();" in resolve
    assert "constexpr int kPullCheckMaterializeLimit" in ACTIONS
    store = (ROOT / "desktop/src/PullStore.h").read_text(encoding="utf-8")
    assert "bool ensurePullHeadRef(const PullRequest &pr, QString *error = nullptr) const" in store
    # A submission that never applies must not spend every pass's budget and
    # starve the pulls that would resolve; an update to the pulls retries it.
    assert "candidate.mayMaterialize &&" in resolve
    apply = _function(ACTIONS, "void MainWindow::applyPullCheckHeads(")
    assert "m_pullChecksNoHead.insert(" in apply
    queue = _function(ACTIONS, "void MainWindow::queueChecksForOpenPulls(")
    assert "m_pullChecksNoHead.erase(it)" in queue


def test_publishing_the_head_does_not_also_fire_the_push_pipeline():
    apply = _function(ACTIONS, "void MainWindow::applyPullCheckHeads(")
    assert "m_explicitActionPushes.insert(explicitKey);" in apply
    assert "if (head.published) {" in apply
    # enqueuePushEvent consumes that claim and returns before queueing anything.
    push = _function(ACTIONS, "void MainWindow::enqueuePushEvent(")
    assert "if (m_explicitActionPushes.remove(explicitKey)) {" in push


def test_the_head_probes_never_block_the_gui_thread():
    resolve = _function(ACTIONS, "void MainWindow::resolvePullCheckHeads(")
    assert "runOffThread<QList<PullCheckHead>>(" in resolve
    worker = resolve.split("runOffThread<QList<PullCheckHead>>(", 1)[1]
    # runGitCapture pumps the event loop on the GUI thread; this pass runs on
    # every pull reload, so it uses a plain QProcess on a worker instead.
    assert "runGitCapture" not in worker
    assert "QProcess process;" in worker


def test_every_pull_reload_and_the_relay_sweep_queue_checks():
    loaded = _function(PULLS, "void MainWindow::applyLoadedPulls(")
    assert "queueChecksForOpenPulls(" in loaded
    # ...and a repository nobody has opened still gets its pull requests checked.
    assert "sweepPullChecksFor(repo);" in PULLS
    sweep = _function(PULLS, "void MainWindow::sweepPullChecksFor(")
    assert "if (!store.canWrite())" in sweep  # only this node's own repositories
    assert "m_pullChecksSweepAtMs.insert(key, nowMs);" in sweep
    assert "queueChecksForOpenPulls(writable, std::move(pulls));" in sweep
    # A submission that just arrived from another node is not left for a timer.
    inbox = _function(PULLS, "void MainWindow::applyPullsInboxPayload(")
    assert "sweepPullChecksFor(repo);" in inbox


def test_running_pull_checks_are_visible_without_opening_the_pull():
    # The PR list reads the run's pull number, not a commit range, so it can
    # afford to render a live signal for every row. Only genuinely executing
    # checks get the spinner; queued and held checks remain distinct states.
    assert "bool pullHasRunningChecks(" in PULLS
    helper = _function(PULLS, "bool pullHasRunningChecks(")
    assert "run.pullNumber == pull.number" in helper
    assert "run.status == ActionStatus::Running" in helper
    listing = _function(PULLS, "void MainWindow::refreshPullList()")
    assert 'checksRunning ? QStringLiteral("sync")' in listing
    assert 'QStringLiteral(" · checks running")' in listing


def test_open_pull_checks_tab_pulses_while_a_check_is_running():
    counts = _function(PULLS, "void MainWindow::updatePullSubTabCounts(")
    assert "setPullChecksAttention(m_pullTabChecks, false);" in counts
    assert "pullHasRunningChecks(pr, m_actionRuns, owner, name)" in counts
    internal = (ROOT / "desktop/src/MainWindowInternal.h").read_text(
        encoding="utf-8")
    assert "void setAttentionLight(bool active)" in internal
    assert "m_attentionTimer->setInterval(650);" in internal
    assert "m_attentionLightOn = !m_attentionLightOn;" in internal


def test_repository_owner_can_merge_despite_unmet_review_or_check_gates():
    action_state = _function(PULLS, "void MainWindow::updatePullActionState()")
    assert "PullMergePolicy::reviewGateBlocks(" in action_state
    assert "independentReviewReady, repositoryOwner" in action_state
    assert "const bool canMerge = mergeable && mergeClean && !conflictPending &&" in action_state
    merge = _function(PULLS, "void MainWindow::mergeCurrentPull()")
    assert "ownerReviewOverride" in merge
    assert "store.mergePull(m_currentPullNumber, &error, requirePeerApproval," in merge


def test_a_first_time_author_is_never_auto_approved():
    resolve = _function(ACTIONS, "bool MainWindow::resolveActionApproval(")
    assert (
        "if (alreadyApproved || !repo.actionsAutoApprove || run.firstTimePullAuthor)"
        in resolve
    )
    # The startup/settings sweep must not quietly release them either.
    sweep = _function(ACTIONS, "int MainWindow::autoApproveAwaitingRuns(")
    assert "if (run.firstTimePullAuthor) {" in sweep
    assert sweep.index("if (run.firstTimePullAuthor) {") < sweep.index(
        "resolveActionApproval(repo, run)"
    )


def test_the_gate_survives_a_restart():
    # Approval is bound to the repository state at that commit, so it never
    # carries to the author's next pull request; the flag has to be persisted
    # or a relaunch would auto-approve what was being held.
    assert "bool firstTimePullAuthor = false;" in ACTION_STORE
    store = (ROOT / "desktop/src/ActionStore.cpp").read_text(encoding="utf-8")
    assert 'o["firstTimePullAuthor"] = firstTimePullAuthor;' in store
    assert (
        'r.firstTimePullAuthor = o.value("firstTimePullAuthor").toBool();' in store
    )


def test_trust_comes_from_a_merge_not_from_a_claimed_name():
    established = _function(
        PULL_STORE,
        "bool isEstablished(const PullRequest &pr, const QList<PullRequest> &history,",
    )
    # A pull request's authorName is chosen by whoever submitted it.
    assert "const bool locallyAuthored = pr.author.trimmed().isEmpty();" in established
    assert "if (locallyAuthored &&" in established
    # Only a merged pull request by the same identity makes an author known,
    # and their own pull request never counts.
    assert 'other.status != QLatin1String("merged")' in established
    assert "other.number == pr.number" in established


def test_the_maintainer_can_release_a_held_run_from_the_pull_request():
    assert "m_pullApproveChecksButton = new QPushButton(\"Approve and run\");" in PULLS
    approve = _function(PULLS, "void MainWindow::approveChecksForCurrentPull()")
    assert "run->status != ActionStatus::AwaitingApproval" in approve
    assert "approveActionRun(id, &error)" in approve
    # The same bound approval the Actions page grants, not a separate one.
    bound = _function(ACTIONS, "bool MainWindow::approveActionRun(")
    assert "bindActionRepositoryState(&verified, mirror, &snapshotError)" in bound
    assert "ActionStore::approve(pending.repoKey()" in bound
    # The verification pumps the event loop, so the run is re-entered by id
    # before the list is mutated (git-pump use-after-free family).
    assert "ActionRun *live = findRun(runId);" in bound
    # And the tab explains what it is waiting for.
    render = _function(PULLS, "void MainWindow::renderPullChecks(PullRequest pr)")
    assert "run->firstTimePullAuthor" in render
    assert "has no merged work in this repository yet" in render


def test_a_skipped_check_is_not_reported_as_a_failure():
    # The conversation card said "3 passed, 3 failed" for a pull request whose
    # three other runs had been skipped, because every terminal non-success
    # status was counted as a failure. Skipped and cancelled runs never reached
    # a verdict, so they carry their own neutral counts — the vocabulary the web
    # pull view already uses.
    summary = _function(PULLS, "void MainWindow::renderPullChecksSummary(PullRequest pr)")
    assert "int passed = 0, failed = 0, skipped = 0, cancelled = 0," in summary
    assert "else if (run->status == ActionStatus::Skipped)\n            ++skipped;" in summary
    assert "else if (run->status == ActionStatus::Cancelled)\n            ++cancelled;" in summary
    # A rejected run is a maintainer's verdict, so it stays a failure, and the
    # two inconclusive states are no longer part of that branch.
    failure_branch = summary.split("++failed;", 1)[0].rsplit("else if", 1)[1]
    assert "ActionStatus::Failed" in failure_branch
    assert "ActionStatus::Rejected" in failure_branch
    assert "ActionStatus::Skipped" not in failure_branch
    assert "ActionStatus::Cancelled" not in failure_branch
    # Both get their own chip, in the neutral grey rather than the failure red,
    # and nothing falls out of the total that decides whether the line renders.
    assert "<span style='color:#8b949e'>\\xE2\\x8A\\x98 %1 skipped</span>" in summary
    assert "<span style='color:#8b949e'>\\xE2\\x8A\\x98 %1 cancelled</span>" in summary
    assert "%1 failed" in summary and summary.count("#f85149") == 1
    assert "passed + failed + skipped + cancelled + running + pending" in summary
