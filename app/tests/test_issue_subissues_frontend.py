"""Checklist-to-sub-issue behavior shared by the dashboard and desktop."""

import json
import subprocess
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


ROOT = Path(__file__).resolve().parents[1]
DASHBOARD = assembled_dashboard_js()
ISSUE_IO = (ROOT / "public/dashboard/js/03-issues-profile-io.js").read_text()
REPO_UI = (ROOT / "public/dashboard/js/06-repo-content.js").read_text()
WIRING = (ROOT / "public/dashboard/js/08-repo-detail-network.js").read_text()
ISSUE_STORE = (ROOT.parent / "desktop/src/IssueStore.cpp").read_text()
ISSUE_VIEW = (ROOT.parent / "desktop/src/MainWindowIssues.cpp").read_text()


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


def _run_task_transforms(expression):
    functions = "\n".join(_function(ISSUE_IO, name) for name in (
        "issueSubtaskCandidateAtLine",
        "setIssueTaskChecked",
        "issueTaskIndent",
        "issueTaskBlockEnd",
        "reorderIssueTaskBody",
    ))
    script = f"{functions}\nconsole.log(JSON.stringify({expression}));"
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_dashboard_finds_only_unlinked_unchecked_tasks():
    parser = _slice(
        ISSUE_IO,
        "function issueSubtaskCandidates(body)",
        "function linkIssueSubtasks(body, linkedTasks)",
    )
    assert r"(?:[-*+]\s+)?\[\s\]\s+" in parser
    assert "tasks.length < 50" in parser
    assert r"\(sub-issue\s+#\d+\)" in parser
    assert "label.slice(0, 240)" in parser


def test_dashboard_creates_children_with_bidirectional_portable_links():
    handler = _slice(
        ISSUE_IO,
        "async function handleWebIssueSubtasks(button)",
        "async function handleWebIssueMetadataSubmit(form)",
    )
    assert "issueSubtaskCandidates(parentBody)" in handler
    assert "await submitWebIssue(repo, candidate.title, childBody)" in handler
    assert "`Parent issue: #${parentNumber}`" in handler
    assert '"edit"' in handler
    assert "linkIssueSubtasks(parentBody, linked)" in handler
    assert "detail.parsed.issueOpenEventId" in handler
    assert "rememberOptimisticRepoRecord" in handler

    linker = _slice(
        ISSUE_IO,
        "function linkIssueSubtasks(body, linkedTasks)",
        "// The compact World chat composer",
    )
    assert "(sub-issue #${task.number})" in linker


def test_dashboard_exposes_and_wires_the_sub_issue_action():
    detail = _slice(
        REPO_UI,
        "function renderRepoIssueDetail(repo, number, parsed)",
        "function setRepoIssueRecordOpen(open)",
    )
    assert "issueSubtaskCandidates(body)" in detail
    assert "canManage ? subtaskCount : 0" in detail
    # The action lives in the Sub-issues panel header it fills.
    panel = _function(REPO_UI, "renderRepoIssueSubIssues")
    assert "data-repo-issue-create-subissues" in panel
    assert "Create from 1 task" in panel
    assert "Create from ${subtaskCount} tasks" in panel
    assert "handleWebIssueSubtasks(issueSubtasksButton)" in WIRING
    # The generated asset a browser receives contains the same complete flow.
    assert "data-repo-issue-create-subissues" in DASHBOARD
    assert "async function handleWebIssueSubtasks(button)" in DASHBOARD


def test_dashboard_task_body_transforms_preserve_markdown_and_nested_content():
    body = (
        "## Plan\n"
        "- [ ] First\n"
        "  wrapped detail\n"
        "  - nested note\n"
        "- [x] Second (sub-issue #8)\n"
        "- [ ] Third\n\n"
        "After the list\n"
    )
    checked, reordered, checked_candidate = _run_task_transforms(
        "["
        f"setIssueTaskChecked({json.dumps(body)}, 1, true),"
        f"reorderIssueTaskBody({json.dumps(body)}, 1, 4, true),"
        'issueSubtaskCandidateAtLine("- [x] Completed row", 0)'
        "]"
    )
    assert "- [x] First\n  wrapped detail" in checked
    assert "Second (sub-issue #8)" in checked
    assert reordered == (
        "## Plan\n"
        "- [x] Second (sub-issue #8)\n"
        "- [ ] First\n"
        "  wrapped detail\n"
        "  - nested note\n"
        "- [ ] Third\n\n"
        "After the list\n"
    )
    assert checked_candidate == {
        "line": 0,
        "prefix": "- [x] ",
        "label": "Completed row",
        "title": "Completed row",
        "checked": True,
    }


def test_dashboard_wires_checking_dragging_keyboard_and_row_conversion():
    assert 'document.addEventListener("change"' in WIRING
    assert 'handleWebIssueTaskToggle(toggle)' in WIRING
    assert 'document.addEventListener("dragstart", handleIssueTaskDragStart)' in WIRING
    assert 'document.addEventListener("drop", handleIssueTaskDrop)' in WIRING
    assert 'handleIssueTaskReorderKey(event)' in WIRING
    assert 'data-repo-issue-task-subissue' in REPO_UI
    assert 'issueSubtaskCandidateAtLine(parentBody, requestedLine)' in ISSUE_IO


def test_dashboard_check_animation_has_reduced_motion_and_touch_fallbacks():
    shell = (ROOT / "public/dashboard/shell.html").read_text()
    assert "fm-task-constellation-out" in shell
    assert "fm-task-constellation-in" in shell
    assert "fm-task-check-pop" in shell
    assert '@media (hover: none)' in shell
    reduced = _slice(
        shell,
        "@media (prefers-reduced-motion: reduce)",
        "@media (max-width: 1023px)",
    )
    assert "[data-repo-issue-task-list]" in reduced


def test_issue_references_in_portable_relationship_text_are_links():
    inline = _slice(
        REPO_UI,
        "function renderMarkdownInline(escapedText, context, linkRecordReferences = true)",
        "function htmlTagAttr(tag, name)",
    )
    assert r"#([1-9]\d*)\b" in inline
    assert 'repoRecordUrl(repo, "issues", Number(number))' in inline
    assert 'recordReference(whole, prefix, label, "pulls", number)' in inline
    assert "forkmesh-ref" in (
        ROOT.parent / "desktop/src/MainWindowShared.cpp"
    ).read_text()


def test_desktop_uses_the_same_link_text_and_offers_one_batch_action():
    assert "bool IssueStore::createSubIssues" in ISSUE_STORE
    assert 'opened.body = QStringLiteral("Parent issue: #%1")' in ISSUE_STORE
    assert 'QStringLiteral(" (sub-issue #%1)")' in ISSUE_STORE
    assert "issueSubIssuesButton" in ISSUE_VIEW
    assert "store.createSubIssues(num, &created, &error)" in ISSUE_VIEW


def test_sub_issue_parent_links_render_as_live_dependency_lists():
    parser = _function(REPO_UI, "issueDependencyNumbers")
    assert "parent\\s+issues?" in parser
    assert "depends?\\s+on" in parser
    assert "#([1-9]\\d*)" in parser

    # A sub-issue names its parents beside the title rather than in a list
    # under its description.
    parents = _function(REPO_UI, "renderRepoIssueParents")
    assert "data-repo-issue-parents" in parents
    assert "Sub-issue of" in parents
    assert "parent.title" in parents
    assert "issueRelationStatusPill(parent.status)" in parents
    detail = _function(REPO_UI, "renderRepoIssueDetail")
    assert "renderRepoIssueParents(repo, parsed.issueDependencies)" in detail
    assert detail.index("data-repo-issue-title-heading") < detail.index("renderRepoIssueParents(repo, parsed.issueDependencies)")

    description = _function(REPO_UI, "issueDescriptionWithoutDependencies")
    assert "parent\\s+issues?" in description
    assert "depends?\\s+on" in description

    summaries = _function(REPO_UI, "loadRepoIssueSummaries")
    assert "fetchRepoIssueBlob(repo, number)" in summaries
    assert "parseIssueJson(blobText(blob), number)" in summaries
    loader = _function(REPO_UI, "loadRepoIssueDependencies")
    assert "loadRepoIssueSummaries(repo, numbers, known)" in loader

    assert "IssueStore::dependencyNumbers(" in ISSUE_VIEW
    assert 'QStringLiteral("issueParentRef")' in ISSUE_VIEW
    assert "Sub-issue of" in ISSUE_VIEW


def test_bulleted_depends_on_lines_backfill_relationships():
    # Existing planning issues write "- Depends on: #481, #484" in a list.
    script = "\n".join(_function(REPO_UI, name) for name in (
        "issueDependencyNumbers",
        "issueDescriptionWithoutDependencies",
    )) + """
const body = "## Delivery\\n- Depends on: #481, #484\\n* **Parent issue:** #7\\n- Depends on: None\\n";
console.log(JSON.stringify([
  issueDependencyNumbers(body),
  issueDescriptionWithoutDependencies(body),
]));
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    numbers, description = json.loads(result.stdout)
    assert numbers == [481, 484, 7]
    assert description == "## Delivery\n\n- Depends on: None"


def test_parents_list_sub_issues_with_full_status_and_an_add_action():
    finder = _function(REPO_UI, "issueSubIssueNumbers")
    assert "issueDependencyNumbers(issue.body)" in finder and "dependsOn.includes(parent)" in finder
    assert r"\(sub-issue\s+#([1-9]\d*)\)" in finder

    renderer = _function(REPO_UI, "renderRepoIssueSubIssues")
    assert "data-repo-issue-subissues" in renderer
    assert "data-repo-issue-add-subissue" in renderer
    for field in ("issue.status", "issue.progress", "issue.priority", "issue.assignees"):
        assert field in renderer
    assert "closed</span>" in renderer

    detail = _function(REPO_UI, "renderRepoIssueDetail")
    assert detail.index("${openingCard}") < detail.index("${subIssues}")
    assert "openIssueCompose(state.selectedRepo, {" in WIRING
    compose = (ROOT / "public/dashboard/js/07-repo-compose-branch.js").read_text()
    assert "`Parent issue: #${parent}\\n\\n`" in compose

    assert "IssueStore::subIssueNumbers(m_currentIssues, issue.number)" in ISSUE_VIEW
    assert 'QStringLiteral("Parent issue: #%1\\n\\n")' in ISSUE_VIEW
    assert "QList<int> IssueStore::subIssueNumbers" in ISSUE_STORE


def test_mentions_join_the_issue_history():
    mentions = _function(REPO_UI, "issueMentionsOf")
    assert "issueReferenceNumbers(body).includes(target)" in mentions
    assert "_mention: true" in mentions
    timeline = _function(REPO_UI, "renderIssueTimeline")
    assert "mentioned this in" in timeline
    assert "options.mentions" in timeline
    assert "IssueStore::mentionsOf(m_currentIssues, issue.number)" in ISSUE_VIEW
    assert "mentioned this in" in ISSUE_VIEW

    script = "\n".join(_function(REPO_UI, name) for name in (
        "issueDependencyNumbers",
        "issueReferenceNumbers",
        "issueSubIssueNumbers",
        "issueMentionsOf",
    )) + """
const issues = [
  { number: 20, title: "Parent", author: "ann", body: "- [ ] Row (sub-issue #24)",
    events: [{ id: "o20", type: "open", ts: 1, authorName: "ann", body: "- [ ] Row (sub-issue #24)" }] },
  { number: 21, title: "Child", author: "bob", body: "Parent issue: #20",
    events: [{ id: "o21", type: "open", ts: 2, authorName: "bob", body: "Parent issue: #20" }] },
  { number: 23, title: "Talker", author: "dee", body: "`#20` in code",
    events: [
      { id: "o23", type: "open", ts: 3, authorName: "dee", body: "`#20` in code" },
      { id: "c1", type: "comment", ts: 5, authorName: "eve", body: "See #20" },
      { id: "c2", type: "comment", ts: 6, authorName: "fay", body: "Also #20" },
      { id: "d1", type: "delete", target: "c2", ts: 7 },
    ] },
];
console.log(JSON.stringify([
  issueReferenceNumbers("#5 foo#6 &#7; `#8`\\n```\\n#9\\n```\\n#10"),
  issueSubIssueNumbers(issues, 20, issues[0].body),
  issueMentionsOf(issues, 20).map((m) => [m.sourceNumber, m.authorName, m.ts]),
]));
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True, text=True, capture_output=True,
    )
    references, children, found = json.loads(result.stdout)
    assert references == [5, 10]
    assert children == [21, 24]
    assert found == [[21, "bob", 2], [23, "eve", 5]]


def _run_issue_blob_probe(batch):
    functions = "\n".join((
        _function(REPO_UI, "issueJsonPath"),
        _function(REPO_UI, "issueJsonCandidatePaths"),
        "async " + _function(REPO_UI, "fetchRepoIssueBlob"),
    ))
    script = f"""{functions}
const single = [];
const batch = {json.dumps(batch)};
const fetchRepoBlobs = async (repo, paths) => {{
  if (batch === "error") throw new Error("mirror_unavailable");
  return Object.fromEntries(paths.map((path) => [path, batch[path] || null]));
}};
const repoLiveUrl = (repo, action, params) => params.path;
const fetchRepoJson = async (path) => {{
  single.push(path);
  if (batch === "error" && path.includes("/closed/")) return {{ text: "single" }};
  throw new Error("not_found");
}};
let blob = null;
let error = "";
try {{ blob = await fetchRepoIssueBlob({{}}, 499); }} catch (e) {{ error = e.message; }}
console.log(JSON.stringify({{ blob, error, single }}));
"""
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_closed_issue_is_read_without_a_404_probe():
    closed = ".forkmesh/issues/closed/499/issue-499.json"
    found = _run_issue_blob_probe({closed: {"text": "batch"}})
    assert found == {"blob": {"text": "batch"}, "error": "", "single": []}

    fallback = _run_issue_blob_probe("error")
    assert fallback["blob"] == {"text": "single"}
    assert fallback["single"] == [
        ".forkmesh/issues/open/499/issue-499.json", closed,
        ".forkmesh/issues/499/issue-499.json"]

    missing = _run_issue_blob_probe({})
    assert missing == {"blob": None, "error": "not_found", "single": []}
