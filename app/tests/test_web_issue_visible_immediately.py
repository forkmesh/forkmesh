#!/usr/bin/env python3
"""An issue filed online is a real numbered record right away (issue #609).

The relay reserves the durable issue number and answers the POST with it before
any mirror has committed the git blob, so a web submission is never a "pending"
placeholder. What used to undo that is the list: loadRepoIssues rebuilds
state.issuesView.items from the mirror's folder listing, so a submitter who
reloaded the Issues tab a second after filing watched their issue vanish until a
mirror converged.

The projection the browser already holds (state.repoOptimisticRecords) now
survives the page and is folded into the loaded list as an ordinary row, then
dropped the instant the mirror's own listing carries that number. The rules are
exercised here rather than only asserted as source text, because "shows up right
away" and "never renders twice" are behaviours, not strings.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pytest

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
REPO_CONTENT = (
    PUBLIC / "dashboard" / "js" / "06-repo-content.js"
).read_text(encoding="utf-8")
COMPOSE = (
    PUBLIC / "dashboard" / "js" / "07-repo-compose-branch.js"
).read_text(encoding="utf-8")
FILING = (PUBLIC / "chat-issue-filing.js").read_text(encoding="utf-8")
CHAT = (PUBLIC / "chat.js").read_text(encoding="utf-8")
DASHBOARD_CHAT = (PUBLIC / "dashboard-chat.js").read_text(encoding="utf-8")
DASHBOARD_JS = assembled_dashboard_js()


def _region(source, start, end):
    i = source.index(start)
    return source[i:source.index(end, i)]


# The projection store is one contiguous slice of the dashboard closure, so the
# harness runs the shipped source rather than a copy of it. Only `state`,
# `formatRecordDate` and `localStorage` cross the slice boundary.
STORE = _region(
    REPO_CONTENT,
    "  function optimisticRepoRecordKey(repo, kind, number)",
    "  function showOptimisticRepoRecord",
)

HARNESS = r'''
const storage = new Map();
globalThis.localStorage = {
  getItem: (key) => (storage.has(key) ? storage.get(key) : null),
  setItem: (key, value) => { storage.set(key, String(value)); },
};
const state = { repoOptimisticRecords: {} };
const formatRecordDate = (value) => (Number(value) > 0 ? "a date" : "unknown");

__STORE__

const repo = { owner: "Source", name: "ForkMesh" };
const out = { steps: [] };
const scenario = process.argv[2];
const event = { type: "open", id: "open-web-1", author: "AUTHORKEY" };
const record = {
  values: {
    title: "Posted from the website",
    status: "open",
    author: "AUTHORKEY",
    authorName: "jett",
    ts: 1787000000000,
    milestone: "v1",
  },
  body: "the description",
  issueEvents: [event],
  issueCreator: "AUTHORKEY",
};

if (scenario === "survives-reload") {
  rememberOptimisticRepoRecord(repo, "issues", 645, record);
  out.stored = JSON.parse(storage.get("forkmesh.repoOptimisticRecords"));
  // A fresh page: nothing in memory, everything from the browser store.
  state.repoOptimisticRecords = {};
  optimisticRecordsHydrated = false;
  // The mirror serves the issues it had before the submission.
  out.list = withOptimisticIssues(
    repo,
    [{ number: 644, title: "older", status: "open" }],
    new Set([644]),
  );
} else if (scenario === "converged") {
  rememberOptimisticRepoRecord(repo, "issues", 645, record);
  const served = [
    { number: 645, title: "Posted from the website", status: "open" },
    { number: 644, title: "older", status: "open" },
  ];
  out.list = withOptimisticIssues(repo, served, new Set([644, 645]));
  out.remaining = JSON.parse(storage.get("forkmesh.repoOptimisticRecords"));
} else if (scenario === "converged-closed") {
  // The closed history is paged in lazily, so a submission the owner already
  // closed is absent from `items` but present in the folder listing.
  rememberOptimisticRepoRecord(repo, "issues", 645, record);
  out.list = withOptimisticIssues(repo, [], new Set([645]));
  out.remaining = JSON.parse(storage.get("forkmesh.repoOptimisticRecords"));
} else if (scenario === "renumbered") {
  // The owner node found the relay's reserved slot taken and assigned the next
  // free one, so the same signed open event is served under another number.
  rememberOptimisticRepoRecord(repo, "issues", 645, record);
  out.list = withOptimisticIssues(
    repo,
    [{ number: 646, title: "Posted from the website", status: "open",
       openEventId: "open-web-1" }],
    new Set([646]),
  );
  out.remaining = JSON.parse(storage.get("forkmesh.repoOptimisticRecords"));
} else if (scenario === "expired") {
  rememberOptimisticRepoRecord(repo, "issues", 645, record);
  const stale = JSON.parse(storage.get("forkmesh.repoOptimisticRecords"));
  for (const key of Object.keys(stale)) {
    stale[key].savedAt = Date.now() - 30 * 24 * 60 * 60 * 1000;
  }
  storage.set("forkmesh.repoOptimisticRecords", JSON.stringify(stale));
  state.repoOptimisticRecords = {};
  optimisticRecordsHydrated = false;
  out.list = withOptimisticIssues(repo, [], new Set());
} else if (scenario === "other-repo") {
  rememberOptimisticRepoRecord(repo, "issues", 645, record);
  rememberOptimisticRepoRecord(repo, "pulls", 12, record);
  out.list = withOptimisticIssues(
    { owner: "someone", name: "else" }, [], new Set());
} else if (scenario === "forget") {
  rememberOptimisticRepoRecord(repo, "issues", 645, record);
  forgetOptimisticRepoRecord(repo, "issues", 645);
  out.remaining = JSON.parse(storage.get("forkmesh.repoOptimisticRecords"));
  out.list = withOptimisticIssues(repo, [], new Set());
} else if (scenario === "bounded") {
  // A CSV import files a whole batch at once, and a body can carry inline
  // images, so neither the session map nor the browser store may grow without
  // limit - and an oversized projection must cost only itself.
  for (let i = 0; i < 60; i += 1) {
    rememberOptimisticRepoRecord(repo, "issues", 800 + i, record);
  }
  rememberOptimisticRepoRecord(
    repo, "issues", 700, { ...record, body: "x".repeat(600 * 1024) });
  const stored = JSON.parse(storage.get("forkmesh.repoOptimisticRecords"));
  out.storedCount = Object.keys(stored).length;
  out.storedBytes = storage.get("forkmesh.repoOptimisticRecords").length;
  out.oversizedStored = Object.keys(stored).some((k) => k.endsWith(":700"));
  out.inMemory = Object.keys(state.repoOptimisticRecords).length;
  out.oversizedInMemory = Boolean(
    state.repoOptimisticRecords["source/forkmesh:issues:700"]);
}

process.stdout.write(JSON.stringify(out));
'''


def _run(scenario):
    completed = subprocess.run(
        ["node", "--input-type=module", "-", scenario],
        input=HARNESS.replace("__STORE__", STORE),
        text=True,
        capture_output=True,
        check=True,
    )
    return json.loads(completed.stdout)


pytestmark = pytest.mark.skipif(
    shutil.which("node") is None, reason="Node.js is unavailable")


def test_a_just_filed_issue_still_lists_after_a_reload():
    result = _run("survives-reload")
    # Persisted, not merely held in the tab that filed it.
    assert list(result["stored"]) == ["source/forkmesh:issues:645"]
    numbers = [item["number"] for item in result["list"]]
    assert numbers == [645, 644], "the new issue leads the list"
    row = result["list"][0]
    # An ordinary numbered row - no "pending"/"syncing" state, and addressable
    # (renderRepoRecordList only links rows whose number is numeric).
    assert row["state"] == "open" and row["status"] == "open"
    assert row["title"] == "Posted from the website"
    assert row["author"] == "jett"
    assert row["body"] == "the description"
    assert row["milestone"] == "v1"
    assert "pending" not in json.dumps(row).lower()
    assert "syncing" not in json.dumps(row).lower()


def test_the_projection_is_dropped_once_the_mirror_serves_the_number():
    result = _run("converged")
    assert [item["number"] for item in result["list"]] == [645, 644]
    # Exactly one row for 645: the mirror's, not the mirror's plus a copy.
    assert result["list"][0]["title"] == "Posted from the website"
    assert result["remaining"] == {}


def test_a_closed_submission_counts_as_served_even_before_its_blob_loads():
    # Open-only paging means a converged-then-closed issue is missing from
    # `items`; keying convergence on the folder listing keeps it from coming
    # back as a phantom open row.
    result = _run("converged-closed")
    assert result["list"] == []
    assert result["remaining"] == {}


def test_a_reassigned_number_retires_the_projection_by_its_signed_event():
    # Otherwise the browser would show a phantom #645 forever beside the real
    # #646 - the same submission, counted twice.
    result = _run("renumbered")
    assert [item["number"] for item in result["list"]] == [646]
    assert result["remaining"] == {}


def test_a_projection_that_never_converged_expires_instead_of_lingering():
    assert _run("expired")["list"] == []


def test_only_this_repositorys_issue_projections_join_its_list():
    # Keys are owner/repo:kind:number; a pull projection and another
    # repository's issues must not leak into this list.
    assert _run("other-repo")["list"] == []


def test_forgetting_a_record_clears_it_from_the_browser_store_too():
    result = _run("forget")
    assert result["remaining"] == {}
    assert result["list"] == []


def test_the_projection_store_is_bounded_by_count_and_size():
    result = _run("bounded")
    assert result["inMemory"] == 40, "the session map is capped too"
    assert result["storedBytes"] <= 512 * 1024
    # The oversized entry is skipped rather than evicting the 39 newest behind
    # it, and it still renders for the rest of this session.
    assert result["storedCount"] == 39
    assert result["oversizedStored"] is False
    assert result["oversizedInMemory"] is True


def test_the_issue_list_load_folds_in_and_counts_unserved_submissions():
    load = _region(
        REPO_CONTENT, "  async function loadRepoIssues(repo)",
        "  async function fetchIssuePage(")
    # Convergence is decided by the folder listing (open + closed), never by the
    # open-only blobs that were actually read.
    assert (
        "const servedNumbers = new Set([\n"
        "        ...pathByNumber.keys(), ...closedPaths.keys(),\n"
        "      ]);"
    ) in load
    assert (
        "state.issuesView.items = withOptimisticIssues(repo, items, servedNumbers);"
    ) in load
    # A mirror with no .forkmesh/issues folder yet is the normal state right
    # after the very first issue is filed online.
    assert (
        "state.issuesView.items = withOptimisticIssues(repo, [], new Set());"
    ) in load
    # The tab badge counts what the list shows, or it reads as "yours is missing".
    assert (
        "const openIssues = openPaths.size + legacyOpen + notYetServed\n"
        "        + pendingOpened - pendingClosed;"
    ) in load


def test_every_optimistic_write_goes_through_the_persisting_helper():
    # A direct assignment would be invisible after a reload, which is the exact
    # regression this issue is about.
    assert "state.repoOptimisticRecords[" not in COMPOSE
    assert COMPOSE.count('rememberOptimisticRepoRecord(repo, "issues", number, {') == 2
    assert 'rememberOptimisticRepoRecord(targetRepo, "pulls", number, {' in COMPOSE
    assert "delete state.repoOptimisticRecords" not in DASHBOARD_JS


def test_chat_surfaces_name_the_created_issue_instead_of_an_inbox_queue():
    # One wording, shared by the full chat page, the dashboard chat and the
    # World's in-world chat.
    assert (
        "export function issueFiledSummary(repositoryLabel, title, result)"
        in FILING
    )
    assert "`Issue #${number} “${title}” was created in ${repositoryLabel}.`" in FILING
    assert "issueFiledSummary," in CHAT
    assert CHAT.count("issueFiledSummary(") == 2  # both filing call sites
    assert "filing.issueFiledSummary(" in DASHBOARD_CHAT
    assert DASHBOARD_CHAT.count("issueFiledSummary(") == 3
    # No surface still describes an accepted, numbered issue as queued work.
    for source in (CHAT, DASHBOARD_CHAT):
        assert "was signed and sent to" not in source
    assert DASHBOARD_CHAT.count('"Issue sent to the maintainer inbox."') == 2
    assert DASHBOARD_CHAT.count("`Issue #${Number(filed.number)} created.`") == 2
