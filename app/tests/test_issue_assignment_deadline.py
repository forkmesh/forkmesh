import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
JS = ROOT / "public" / "dashboard" / "js"
CONTENT = (JS / "06-repo-content.js").read_text(encoding="utf-8")
ISSUES_IO = (JS / "03-issues-profile-io.js").read_text(encoding="utf-8")
HELPERS = (JS / "02-helpers.js").read_text(encoding="utf-8")
BUNDLE = (ROOT / "public" / "dashboard.js").read_text(encoding="utf-8")
TAILWIND = (ROOT / "public" / "dashboard" / "tailwind.css").read_text(encoding="utf-8")
DOC = (ROOT.parent / "docs" / "app" / "issue-assignment-deadline.md").read_text(encoding="utf-8")
HOUR = 3_600_000
T0 = 1_800_000_000_000


def _function(text, name):
    match = re.search(r"^  (?:async )?function %s\(" % re.escape(name), text, re.M)
    start = match.start()
    cursor = text.index("{", text.index(")", start))
    depth = 0
    while True:
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                return text[start:cursor + 1]
        cursor += 1


def _constants(text, prefix):
    return "\n".join(line for line in text.splitlines()
                     if line.startswith("  const %s" % prefix))


def _node(body):
    script = """
function issueLinkedPullNumbers(number, events, pulls) {
  return (pulls || []).filter((pull) => String(pull.body || "").includes(`#${number}`)).map((pull) => Number(pull.number));
}
function formatRecordDateTime(value) { return { iso: "", label: new Date(value).toISOString() }; }
function blobText(blob) { return blob.content; }
function normalizePullReviewTemplate(value) { return typeof value === "string" ? value : ""; }
%s
%s
%s
%s
""" % (
        _function(HELPERS, "escapeHtml"),
        _function(ISSUES_IO, "webIssueClaimApplied"),
        _constants(CONTENT, "ISSUE_ASSIGNMENT_") + "\n" + _constants(CONTENT, "ISSUE_AI_PROMPT_MAX"),
        "\n".join(_function(CONTENT, name) for name in (
            "issueAssignmentHours", "issueAssignmentTimes", "issueAssigneeHasPull",
            "issueDeadlineDuration", "issueAssignmentDeadline",
            "renderIssueAssignmentDeadline", "issueAssigneeDeadlineHtml",
            "repoCollabSettingsFromBlobs")),
    ) + body
    result = subprocess.run(["node", "--input-type=module"], input=script,
                            text=True, capture_output=True)
    assert result.returncode == 0, result.stderr[-2000:]
    return json.loads(result.stdout)


def test_assignment_hours_come_from_issues_json():
    out = _node("""
const read = (text) => repoCollabSettingsFromBlobs(text === null ? {} : { ".forkmesh/issues.json": { content: text } }).assignmentHours;
console.log(JSON.stringify([
  read('{"assignmentHours": 24}'), read('{"assignmentHours": 1.5}'), read('{"assignmentHours": "48"}'),
  read('{"assignmentHours": 0}'), read('{"assignmentHours": -3}'), read('{"assignmentHours": "soon"}'),
  read('{"assignmentHours": true}'), read('{"assignmentHours": 1e9}'), read('{}'), read("not json"), read(null),
]));
""")
    assert out == [24, 1.5, 48, 0, 0, 0, 0, 24 * 365, 0, 0, 0]


def test_assignment_time_is_the_event_that_last_added_the_person():
    out = _node("""
const events = [
  { type: "open", ts: %d },
  { type: "assignees", assignees: ["Alice"], ts: %d },
  { type: "assignees", assignees: ["alice", "bob"], ts: %d },
  { type: "claim", assignee: "carol", claimState: "claimed", ts: %d },
  { type: "claim", assignee: "Bob", claimState: "released", ts: %d },
  { type: "assignees", assignees: ["alice", "carol", "bob"], ts: %d },
];
console.log(JSON.stringify(Object.fromEntries(issueAssignmentTimes(events))));
""" % (T0, T0 + HOUR, T0 + 2 * HOUR, T0 + 3 * HOUR, T0 + 4 * HOUR, T0 + 5 * HOUR))
    assert out == {"alice": T0 + HOUR, "carol": T0 + 3 * HOUR, "bob": T0 + 5 * HOUR}


def test_deadline_text_fraction_and_warning_threshold():
    out = _node("""
const at = %d, h = %d;
const pick = (d) => d && { label: d.label, warn: d.warn, overdue: d.overdue, fraction: Math.round(d.fraction * 100) };
console.log(JSON.stringify([
  pick(issueAssignmentDeadline(at, 24, at + 6 * h)),
  pick(issueAssignmentDeadline(at, 24, at + 19 * h)),
  pick(issueAssignmentDeadline(at, 24, at + 26 * h + 10 * 60000)),
  pick(issueAssignmentDeadline(at, 24, at + 23 * h + 15 * 60000)),
  pick(issueAssignmentDeadline(at, 168, at)),
  issueAssignmentDeadline(0, 24, at),
  issueAssignmentDeadline(at, 0, at),
]));
""" % (T0, HOUR))
    assert out[0] == {"label": "18 h left", "warn": False, "overdue": False, "fraction": 25}
    assert out[1] == {"label": "5 h left", "warn": True, "overdue": False, "fraction": 79}
    assert out[2] == {"label": "overdue by 2 h", "warn": True, "overdue": True, "fraction": 100}
    assert out[3]["label"] == "45 min left"
    assert out[4]["label"] == "7 d left"
    assert out[5] is None and out[6] is None


def test_bar_is_hidden_when_closed_unset_or_the_assignee_has_a_pull_request():
    out = _node("""
const at = %d, h = %d;
const events = [{ type: "assignees", assignees: ["alice", "bob"], ts: at }];
const base = { number: 7, closed: false, events, pulls: [], settings: { assignmentHours: 24 } };
const html = (issue, who) => issueAssigneeDeadlineHtml(issue, who, at + 20 * h);
const pulls = [
  { number: 11, author: "Alice", state: "open", body: "Fixes #7" },
  { number: 12, author: "bob", state: "closed", body: "Fixes #7" },
  { number: 13, author: "bob", state: "open", body: "Unrelated #8" },
];
console.log(JSON.stringify({
  open: html(base, "alice"),
  closed: html({ ...base, closed: true }, "alice"),
  unset: html({ ...base, settings: {} }, "alice"),
  unassigned: html(base, "dave"),
  alicePull: html({ ...base, pulls }, "alice"),
  bobClosedPull: html({ ...base, pulls }, "bob"),
  merged: html({ ...base, pulls: [{ number: 14, author: "bob (unverified)", state: "merged", body: "Closes #7" }] }, "bob"),
}));
""" % (T0, HOUR))
    assert 'data-repo-issue-deadline-state="soon"' in out["open"]
    assert "4 h left" in out["open"]
    assert 'aria-valuenow="83"' in out["open"]
    assert "bg-attention" in out["open"] and "text-attention" in out["open"]
    assert out["closed"] == "" and out["unset"] == "" and out["unassigned"] == ""
    assert out["alicePull"] == ""
    assert "4 h left" in out["bobClosedPull"]
    assert out["merged"] == ""


def test_on_track_bar_uses_primary_and_classes_exist():
    out = _node("""
console.log(JSON.stringify(renderIssueAssignmentDeadline(issueAssignmentDeadline(%d, 24, %d))));
""" % (T0, T0 + 2 * HOUR))
    assert 'data-repo-issue-deadline-state="on-track"' in out
    assert "bg-primary" in out and "text-muted-foreground" in out
    assert "22 h left" in out
    for name in ("bg-primary", "bg-attention", "text-attention", "h-1\\.5", "w-20"):
        assert "." + name + "{" in TAILWIND, name


def test_sidebar_wiring_bundle_and_docs():
    detail = _function(CONTENT, "renderRepoIssueDetail")
    assert "issueAssigneeDeadlineHtml(deadlineIssue, assignee)" in detail
    assert 'data-repo-issue-deadline="${escapeHtml(assignee)}"' in detail
    assert ": row;" in detail
    development = _function(CONTENT, "renderRepoRecordDevelopment")
    assert "renderRepoIssueDeadlines()" in development
    assert "issueLinkedPullNumbers(number, events, pulls)" in _function(CONTENT, "issueAssigneeHasPull")
    assert "function issueAssigneeDeadlineHtml(" in BUNDLE
    assert '{"assignmentHours": 24}' in DOC and ".forkmesh/issues.json" in DOC
