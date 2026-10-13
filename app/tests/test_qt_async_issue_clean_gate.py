"""Keep automatic issue intake's git status probes off the Qt GUI thread."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PULLS = (ROOT / "desktop/src/MainWindowPulls.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "desktop/src/MainWindow.h").read_text(encoding="utf-8")


def _section(source: str, start: str, end: str) -> str:
    first = source.index(start)
    return source[first : source.index(end, first)]


def test_tracked_clean_probe_only_runs_in_the_background_gate():
    launch = _section(
        PULLS,
        "void MainWindow::launchAutomaticIssueSync(",
        "QUrlQuery MainWindow::signedInboxQuery(",
    )

    assert PULLS.count("worktreeTrackedClean(") == 1
    assert "runOffThread<bool>(" in launch
    assert "return worktreeTrackedClean(writablePath);" in launch
    assert "ActionTelemetry::Execution::Worker" in launch


def test_all_automatic_issue_paths_use_the_coalesced_gate():
    poll = _section(
        PULLS,
        "void MainWindow::pollOwnedInboxes()",
        "void MainWindow::queueAutomaticIssueSync(",
    )
    relay = PULLS[PULLS.index("void MainWindow::performRelaySync()") :]

    assert "queueAutomaticIssueSync(repo);" in poll
    assert "worktreeTrackedClean(" not in poll
    assert "queueAutomaticIssueSync(repo, entry.value(\"issues\").toArray()," in relay
    assert "queueAutomaticIssueSync(uncovered, {}, /*applyPayload=*/false," in relay
    assert "worktreeTrackedClean(" not in relay


def test_pending_request_gets_a_fresh_probe_and_callbacks_revalidate_context():
    launch = _section(
        PULLS,
        "void MainWindow::launchAutomaticIssueSync(",
        "QUrlQuery MainWindow::signedInboxQuery(",
    )

    assert "m_automaticIssueSyncChecksInFlight.contains(key)" in launch
    assert "m_automaticIssueSyncPending.insert(key" in launch
    assert "m_automaticIssueSyncPending.erase(pendingIt)" in launch
    assert "launchAutomaticIssueSync(std::move(next))" in launch
    assert "repoIndexFor(request.repo.owner, request.repo.name)" in launch
    assert "request.writablePath" in launch
    assert "request.account" in launch
    assert "request.relayOrigin" in launch
    assert "RepositoryRecord liveRepo" in launch
    assert "applyIssuesInboxPayload(liveRepo" in launch
    assert "drainIssuesInboxFor(liveRepo" in launch
    assert "QSet<QString> m_automaticIssueSyncChecksInFlight" in HEADER
    assert "QHash<QString, AutomaticIssueSyncRequest> m_automaticIssueSyncPending" in HEADER


def test_dirty_or_stale_probe_never_applies_or_acks_issue_rows():
    callback = _section(
        PULLS,
        "[this, key, request = std::move(request)](bool clean) mutable {",
        "m_automaticIssueSyncChecksInFlight.remove(key);",
    )

    assert "if (clean && stillCurrent)" in callback
    guarded = callback.split("if (clean && stillCurrent)", 1)[1]
    assert "applyIssuesInboxPayload(" in guarded
    assert "drainIssuesInboxFor(" in guarded


def test_consolidated_payload_keeps_the_origin_that_returned_it():
    relay = PULLS[PULLS.index("void MainWindow::performRelaySync()") :]

    assert "const QString syncRelayOrigin = normalizedRelayOrigin(url);" in relay
    assert "[this, reply, account, syncRelayOrigin]" in relay
    assert "/*applyPayload=*/true, account," in relay
    assert "account, syncRelayOrigin);" in relay
