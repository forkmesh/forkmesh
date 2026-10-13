#!/usr/bin/env python3
"""A close or reopen made on the website survives the next mirror read (#715).

The signed status event reaches the mirror only after the owner node drains
its inbox and the mirror syncs that commit. The change used to live only in
page memory, so reopening the issue from the list, reloading the list or
refreshing the page read the old status back and the issue showed as open
again. #711 was closed three times in 21 seconds that way. These tests run the
shipped overlay helpers in node against served records that predate the drain,
that carry the event, and that carry a newer status change from elsewhere.
"""

import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ISSUE_IO = (ROOT / "public/dashboard/js/03-issues-profile-io.js").read_text()
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()

HELPERS = (
    "optimisticRepoRecordKey",
    "pendingIssueStatuses",
    "persistPendingIssueStatuses",
    "rememberPendingIssueStatus",
    "pendingIssueStatusFor",
    "withPendingIssueStatus",
    "applyPendingIssueStatuses",
    "pendingReopenedIssueNumbers",
)


def _slice(source, start, end):
    begin = source.index(start)
    finish = source.index(end, begin)
    return source[begin:finish]


def _function(source, name):
    start = source.index(f"function {name}(")
    cursor = source.index("{", start)
    depth = 0
    while cursor < len(source):
        if source[cursor] == "{":
            depth += 1
        elif source[cursor] == "}":
            depth -= 1
            if depth == 0:
                return source[start:cursor + 1]
        cursor += 1
    raise AssertionError(f"unterminated JavaScript function: {name}")


def _constant(source, name):
    start = source.index(f"const {name} =")
    return source[start:source.index(";", start) + 1]


def _run(body):
    script = "\n".join([
        "const storage = new Map();",
        "globalThis.localStorage = {",
        "  getItem: (key) => (storage.has(key) ? storage.get(key) : null),",
        "  setItem: (key, value) => storage.set(key, String(value)),",
        "};",
        "let state = {};",
        _constant(REPO_UI, "OPTIMISTIC_RECORD_TTL_MS"),
        _constant(REPO_UI, "PENDING_ISSUE_STATUS_STORAGE"),
        _constant(REPO_UI, "PENDING_ISSUE_STATUS_LIMIT"),
        "let pendingIssueStatusHydrated = false;",
        *(_function(REPO_UI, name) for name in HELPERS),
        "const repo = { owner: 'mainnode', name: 'ForkMesh' };",
        "const close = { type: 'status', id: 'status-web-2000-a', status: 'closed', ts: Date.now() };",
        "const out = {};",
        body,
        "console.log(JSON.stringify(out));",
    ])
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    return json.loads(result.stdout)


def test_detail_served_before_the_drain_stays_closed():
    out = _run("""
      rememberPendingIssueStatus(repo, 711, close);
      const served = { values: { status: 'open', title: 't' }, issueEvents: [{ type: 'open', id: 'o', ts: 1 }] };
      const parsed = withPendingIssueStatus(repo, 711, served);
      out.status = parsed.values.status;
      out.events = parsed.issueEvents.map((event) => event.id);
      out.title = parsed.values.title;
      // Another issue is untouched.
      out.other = withPendingIssueStatus(repo, 712, { values: { status: 'open' }, issueEvents: [] }).values.status;
    """)
    assert out["status"] == "closed"
    assert out["events"] == ["o", "status-web-2000-a"]
    assert out["title"] == "t"
    assert out["other"] == "open"


def test_pending_close_survives_a_page_reload():
    out = _run("""
      rememberPendingIssueStatus(repo, 711, close);
      // A fresh page: nothing in memory, only what the browser stored.
      state = {};
      pendingIssueStatusHydrated = false;
      out.status = withPendingIssueStatus(repo, 711, { values: { status: 'open' }, issueEvents: [] }).values.status;
    """)
    assert out["status"] == "closed"


def test_mirror_carrying_the_event_retires_it():
    out = _run("""
      rememberPendingIssueStatus(repo, 711, close);
      const served = { values: { status: 'closed' }, issueEvents: [close] };
      out.status = withPendingIssueStatus(repo, 711, served).values.status;
      out.count = served.issueEvents.length;
      out.left = Object.keys(pendingIssueStatuses()).length;
      out.stored = JSON.parse(localStorage.getItem(PENDING_ISSUE_STATUS_STORAGE));
    """)
    assert out["status"] == "closed"
    assert out["count"] == 1
    assert out["left"] == 0
    assert out["stored"] == {}


def test_a_newer_status_change_from_another_client_wins():
    out = _run("""
      rememberPendingIssueStatus(repo, 711, close);
      const reopen = { type: 'status', id: 'qt', status: 'open', ts: close.ts + 5000 };
      const served = { values: { status: 'open' }, issueEvents: [reopen] };
      out.status = withPendingIssueStatus(repo, 711, served).values.status;
      out.left = Object.keys(pendingIssueStatuses()).length;
    """)
    assert out["status"] == "open"
    assert out["left"] == 0


def test_list_rows_move_and_report_the_served_status():
    out = _run("""
      rememberPendingIssueStatus(repo, 711, close);
      rememberPendingIssueStatus(repo, 600, { type: 'status', id: 'status-web-2001-b', status: 'open', ts: Date.now() });
      const rows = [
        { number: 711, status: 'open', state: 'open', events: [] },
        { number: 600, status: 'closed', state: 'closed', events: [] },
        { number: 5, status: 'open', state: 'open', events: [] },
      ];
      const moved = applyPendingIssueStatuses(repo, rows);
      out.rows = rows.map((row) => [row.number, row.status, row.state]);
      out.moved = [...moved.entries()];
      out.reopened = pendingReopenedIssueNumbers(repo, new Map([[600, 'p'], [601, 'q']]));
    """)
    assert out["rows"] == [[711, "closed", "closed"], [600, "open", "open"], [5, "open", "open"]]
    assert sorted(out["moved"]) == [[600, "closed"], [711, "open"]]
    assert out["reopened"] == [600]


def test_expired_entries_are_dropped():
    out = _run("""
      rememberPendingIssueStatus(repo, 711, { ...close, ts: Date.now() - OPTIMISTIC_RECORD_TTL_MS - 1 });
      out.status = withPendingIssueStatus(repo, 711, { values: { status: 'open' }, issueEvents: [] }).values.status;
    """)
    assert out["status"] == "open"


def test_status_events_are_remembered_and_every_mirror_read_applies_them():
    submit = _slice(
        ISSUE_IO,
        "async function submitWebIssueEvent(repo, number, type, fields = {})",
        "async function setWebIssueSubscription",
    )
    # Recorded only after the relay accepted the event.
    assert submit.index("await fetchRepoMutation(repo, \"issues\", payload);") < submit.index(
        'if (type === "status") rememberPendingIssueStatus(repo, issueNumber, event);')
    detail = _function(REPO_UI, "loadRepoRecordDetail")
    assert "withPendingIssueStatus(repo, number, issueDetailParsed(blobText(blob), number))" in detail
    assert "withPendingIssueStatus(repo, number, parsed)" in _function(REPO_UI, "showOptimisticRepoRecord")
    listing = _function(REPO_UI, "loadRepoIssues")
    assert "applyPendingIssueStatuses(repo, state.issuesView.items)" in listing
    assert "pendingReopenedIssueNumbers(repo, closedPaths)" in listing
