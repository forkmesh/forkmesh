#!/usr/bin/env python3
"""Collaboration inbox reads hand back a bounded page, never the backlog.

MAX_PENDING_* bounds a queue's row count and MAX_PULL_BYTES bounds one patch;
neither bounds their product. A drain that selected, decrypted and encoded
every leased row materialized the whole queue several times over inside a
128 MB Worker isolate — the allocation behind "RuntimeError: memory access out
of bounds" / abortIsolate() on the mirror-intake route. It could not recover on
its own either: the mirror daemon rejects any inbox response over 8 MiB
(collaborationInboxLimit), so the backlog that caused the crash was also the
backlog that could never drain.
"""

import ast
import asyncio
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

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "src" / "entry.py"
ENTRY_TEXT = ENTRY.read_text(encoding="utf-8")
ENTRY_TREE = ast.parse(ENTRY_TEXT, filename=str(ENTRY))
INTAKE_GO = (
    ROOT.parent / "server" / "intake.go").read_text(encoding="utf-8")
QT_ISSUES = (
    ROOT.parent / "desktop" / "src" / "MainWindowIssues.cpp"
).read_text(encoding="utf-8")
QT_REPOS = "".join(
    (ROOT.parent / "desktop" / "src" / name).read_text(encoding="utf-8")
    for name in MAIN_WINDOW_REPOS_SOURCES
)


def _constant(name):
    for node in ENTRY_TREE.body:
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Name) and target.id == name
                for target in node.targets):
            # The real value, so raising a cap can never quietly
            # turn these bounds checks into no-ops.
            return eval(compile(
                ast.Expression(node.value), "<const>", "eval"), {})
    raise AssertionError(name + " is not defined in entry.py")


def _function_source(name):
    node = next(
        item for item in ENTRY_TREE.body
        if isinstance(item, (ast.FunctionDef, ast.AsyncFunctionDef))
        and item.name == name
    )
    return ast.get_source_segment(ENTRY_TEXT, node)


MAX_ROWS = _constant("INBOX_DRAIN_MAX_ROWS")
MAX_BYTES = _constant("INBOX_DRAIN_MAX_BYTES")


def _load_page_reader(queue):
    """Return (_leased_inbox_page, recorded queries) over a fake inbox table."""
    calls = []

    async def d1_all(_env, sql, *args):
        calls.append((sql, args))
        if "LENGTH(data)" in sql:
            limit = args[-1]
            return [{"id": row["id"], "n": len(row["data"])}
                    for row in queue][:limit]
        wanted = set(args)
        return [row for row in queue if row["id"] in wanted]

    node = next(
        item for item in ENTRY_TREE.body
        if isinstance(item, ast.AsyncFunctionDef)
        and item.name == "_leased_inbox_page"
    )
    module = ast.fix_missing_locations(
        ast.Module(body=[node], type_ignores=[]))
    namespace = {
        "d1_all": d1_all,
        "INBOX_DRAIN_MAX_ROWS": MAX_ROWS,
        "INBOX_DRAIN_MAX_BYTES": MAX_BYTES,
    }
    exec(compile(module, str(ENTRY), "exec"), namespace)
    return namespace["_leased_inbox_page"], calls


def _queue(count, size):
    return [{"id": index + 1, "data": "x" * size} for index in range(count)]


def test_page_is_capped_by_row_count_and_reports_more():
    page_reader, calls = _load_page_reader(_queue(MAX_ROWS + 5, 16))

    rows, more = asyncio.run(
        page_reader(object(), "pull_inbox", "repo-bi", "claim-bi"))

    assert len(rows) == MAX_ROWS
    assert more is True
    # The sizing pass must never carry `data`: selecting the payload and
    # trimming afterwards would already have pulled the backlog into the
    # isolate, which is the allocation this bound exists to prevent.
    sizing, payload = calls
    assert "LENGTH(data) AS n" in sizing[0]
    assert "SELECT id, data" not in sizing[0]
    assert "LIMIT ?" in sizing[0] and sizing[1][-1] == MAX_ROWS + 1
    assert "SELECT id, data" in payload[0]
    assert payload[1] == (
        "repo-bi", "claim-bi") + tuple(range(1, MAX_ROWS + 1))


def test_page_is_capped_by_payload_bytes_before_the_row_cap():
    row_size = MAX_BYTES // 4
    page_reader, calls = _load_page_reader(_queue(MAX_ROWS, row_size))

    rows, more = asyncio.run(
        page_reader(object(), "pull_inbox", "repo-bi", "claim-bi"))

    assert len(rows) == 4 < MAX_ROWS
    assert more is True
    # Only the page's own rows are ever read back at full size.
    assert calls[1][1] == ("repo-bi", "claim-bi", 1, 2, 3, 4)


def test_a_row_larger_than_the_budget_still_ships_and_never_stalls():
    page_reader, _ = _load_page_reader(
        [{"id": 1, "data": "x" * (MAX_BYTES * 3)},
         {"id": 2, "data": "small"}])

    rows, more = asyncio.run(
        page_reader(object(), "pull_inbox", "repo-bi", "claim-bi"))

    # Skipping it would park the queue behind a row that can never be read.
    assert [row["id"] for row in rows] == [1]
    assert more is True


def test_an_empty_queue_reads_once_and_reports_nothing_waiting():
    page_reader, calls = _load_page_reader([])

    rows, more = asyncio.run(
        page_reader(object(), "pull_inbox", "repo-bi", "claim-bi"))

    assert rows == [] and more is False
    assert len(calls) == 1


def test_page_scopes_to_the_lease_and_to_unmaterialized_rows():
    page_reader, calls = _load_page_reader(_queue(2, 8))

    asyncio.run(page_reader(
        object(), "discussion_inbox", "repo-bi", "claim-bi", mirror_only=True))
    scoped = [sql for sql, _ in calls]
    assert all("claimed_by_bi=?" in sql and "mirrored_at=0" in sql
               for sql in scoped)

    # The desktop's own discussion read is claim-free; it must stay that way.
    page_reader, calls = _load_page_reader(_queue(2, 8))
    asyncio.run(page_reader(object(), "discussion_inbox", "repo-bi"))
    assert all("claimed_by_bi" not in sql for sql, _ in calls)
    assert calls[0][1][0] == "repo-bi"


def test_page_reader_refuses_a_table_it_does_not_own():
    page_reader, calls = _load_page_reader(_queue(2, 8))

    assert asyncio.run(
        page_reader(object(), "issue_inbox; DROP", "repo-bi")) == ([], False)
    assert calls == []


def test_every_handler_drains_through_the_bounded_page_reader():
    for handler, table in (
        ("issues_handler", "issue_inbox"),
        ("pulls_handler", "pull_inbox"),
        ("discussions_handler", "discussion_inbox"),
    ):
        source = _function_source(handler)
        assert "_leased_inbox_page(" in source
        # No path may go back to reading the queue in one unbounded pass.
        assert "SELECT id, data FROM " + table not in source
        assert "SELECT id,data FROM " + table not in source
        # A page is only honest if the reader is told there is more to fetch.
        assert '"more": more' in source


def test_the_mirror_daemon_follows_more_without_spinning():
    assert '`json:"more"`' in INTAKE_GO
    assert "collaborationDrainMaxPages" in INTAKE_GO
    # Rows the mirror cannot acknowledge (owner-retained, undecodable) come
    # back byte-identical, so no-progress has to end the loop.
    assert "if err != nil || !more || !acked {" in INTAKE_GO


def test_the_desktop_issue_refresh_acknowledges_then_follows_more():
    drain = QT_ISSUES.split(
        "void MainWindow::drainIssuesInboxFor", 1)[1].split(
        "void MainWindow::syncIssuesFromMeshOnOpen", 1)[0]
    assert 'response.value("more").toBool()' in drain
    assert "constexpr int kMaxDrainPages = 64" in drain
    assert "if (applied && more && page + 1 < kMaxDrainPages)" in drain
    # The recursion lives in applyIssuesInboxPayload's completion callback,
    # which fires only after DELETE finishes; issuing it straight after GET
    # would replay the same leased page.
    assert drain.index("applyIssuesInboxPayload(") < drain.index(
        "drainIssuesInboxFor(repo, interactive, /*force=*/true")
    apply_body = QT_ISSUES.split(
        "void MainWindow::applyIssuesInboxPayload", 1)[1].split(
        "QWidget *MainWindow::buildChatSection", 1)[0]
    assert "ackReply->error() == QNetworkReply::NoError" in apply_body
    assert "onApplied(ok);" in apply_body
    assert "duplicateMaterializations" in apply_body
    assert "event.id == wanted.eventId" in apply_body
    assert "event.sig == wanted.signature" in apply_body


def test_issue_button_does_not_reload_the_pre_sync_snapshot():
    click = QT_ISSUES.split(
        "connect(m_issueSyncButton, &QPushButton::clicked", 1)[1].split(
        "connect(issueBurnupButton", 1)[0]
    assert "syncIssuesFromMeshOnOpen(index);" in click
    assert "reloadIssuesInBackground();" not in click


def test_issue_refresh_finishes_inbox_before_fetching_every_exact_mirror():
    sync = QT_ISSUES.split(
        "void MainWindow::syncIssuesFromMeshOnOpen", 1)[1].split(
        "void MainWindow::applyIssuesInboxPayload", 1)[0]
    assert "drainIssuesInboxFor(" in sync
    assert "[this, owner, name]" in sync
    assert sync.index("drainIssuesInboxFor(") < sync.index(
        "convergeSourceRepoFromMesh(liveIndex")

    converge = QT_REPOS.split(
        "void MainWindow::convergeSourceRepoFromMesh", 1)[1].split(
        "void MainWindow::propagateRepoUpdate", 1)[0]
    assert "for (const RepoRemoteRow &remote : readRepoRemotes(workTree))" in converge
    assert "remote.pushUrl.trimmed()" in converge
    assert "convergeSourceCheckoutFromRoutes(" in converge
    assert "m_sourceConvergePending" in converge
