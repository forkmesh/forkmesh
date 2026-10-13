#!/usr/bin/env python3
"""Projects tab with gantt view (issue #384).

Projects live in .forkmesh/projects/<n>/project-<n>.json (signed-event JSON
like issues), link issue numbers and a milestone, and carry start/end dates.
These contract tests pin the website half: the Projects tab sits next to
Issues, loads lazily over the live-mirror tree/blobs tunnel, and renders a
gantt view (default) plus a list view.
"""

import ast
import hashlib
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js


SRC = Path(__file__).resolve().parents[1] / "src"
DASHBOARD_JS = assembled_dashboard_js()


def test_projects_tab_registered_next_to_issues():
    # tabMeta entry + the ordered tab row place Projects right after Issues.
    assert 'projects: { label: "Projects", icon: "chart-gantt", count: "" }' in DASHBOARD_JS
    assert '"issues", "projects", "pulls"' in DASHBOARD_JS
    assert 'data-dashboard-repo-tab-panel="projects"' in DASHBOARD_JS
    assert "data-repo-projects" in DASHBOARD_JS


def test_projects_route_is_a_recognized_tab():
    # Deep links like /owner/repo/projects must parse as a tab route on both
    # the client router and the worker's static-route classifier.
    assert '"issues", "projects", "pulls", "discussions", "mirrors"]' in DASHBOARD_JS
    routes = (SRC / "static_routes.py").read_text(encoding="utf-8")
    assert '"projects",' in routes


def test_projects_load_from_forkmesh_projects_folder():
    # Same batched tree+blobs flow as issues, against .forkmesh/projects/.
    assert '".forkmesh/projects"' in DASHBOARD_JS
    assert ".forkmesh/projects/${Number(number)}/project-${Number(number)}.json" in DASHBOARD_JS
    assert 'if (tab === "projects") loadRepoProjects(state.selectedRepo);' \
        in DASHBOARD_JS.replace("else if", "if")


def test_projects_pin_records_to_the_folder_listing_commit():
    # A tree and its blobs can be routed to separate mirrors. Pin the follow-up
    # project and linked-issue reads to the tree's immutable revision so a
    # healthy mirror that is briefly one push behind cannot look unavailable.
    source = _function_source("loadRepoProjects")
    assert "const projectCommit = immutableGitCommit(tree.commit);" in source
    assert "const projectReadOptions = projectCommit ? { ref: projectCommit } : {};" in source
    assert "projectJsonPath(Number(entry.name))), projectReadOptions);" in source
    assert "issueJsonCandidatePaths(number)),\n          projectReadOptions);" in source


def test_projects_gantt_is_default_view_with_list_fallback():
    assert 'projectsView: { filter: "open", mode: "gantt", items: [] }' in DASHBOARD_JS
    assert "renderRepoProjectsGantt" in DASHBOARD_JS
    assert "renderRepoProjectsList" in DASHBOARD_JS
    assert 'data-dashboard-project-view="${mode}"' in DASHBOARD_JS
    assert "data-repo-projects-gantt" in DASHBOARD_JS


# --- gantt label gutter (issue #639) ----------------------------------------
# The gantt rows are built at runtime, so a `grid-cols-[<gutter>_...]` class
# never reaches the Tailwind scanner and the shipped stylesheet carries no rule
# for it: every row collapses to a single column and the labels get painted
# across the bars and the vertical grid lines. The two-column layout therefore
# comes from an inline grid-template-columns, and whatever static classes the
# view does use have to exist in the compiled stylesheet.

TAILWIND_CSS = (Path(__file__).resolve().parents[1]
                / "public" / "dashboard" / "tailwind.css").read_text(encoding="utf-8")


def _function_source(name):
    """Source text of a top-level function in the assembled dashboard bundle."""
    start = DASHBOARD_JS.index("function %s(" % name)
    depth = 0
    for index in range(DASHBOARD_JS.index("{", start), len(DASHBOARD_JS)):
        if DASHBOARD_JS[index] == "{":
            depth += 1
        elif DASHBOARD_JS[index] == "}":
            depth -= 1
            if depth == 0:
                return DASHBOARD_JS[start:index + 1]
    raise AssertionError("unbalanced braces in " + name)


def _without_comments(source):
    return "\n".join(
        line for line in source.splitlines() if not line.lstrip().startswith("//"))


def _static_class_tokens(source):
    """Class tokens written literally, with every ${...} interpolation dropped."""
    tokens = set()
    index = 0
    while True:
        start = source.find('class="', index)
        if start < 0:
            return tokens
        cursor = start + len('class="')
        depth = 0
        literal = []
        while cursor < len(source):
            if source.startswith("${", cursor):
                depth += 1
                cursor += 2
                continue
            if source[cursor] == "}" and depth:
                depth -= 1
                cursor += 1
                continue
            if source[cursor] == '"' and not depth:
                break
            if not depth:
                literal.append(source[cursor])
            cursor += 1
        tokens.update("".join(literal).split())
        index = cursor + 1


def _stylesheet_defines(token):
    # Tailwind escapes the specials in an arbitrary-value class name, writing a
    # comma as the CSS escape "\2c "; variants land as a selector suffix.
    selector = "." + "".join(
        "\\2c " if char == "," else "\\" + char if char in "[]().:/%!#" else char
        for char in token)
    return selector in TAILWIND_CSS


def test_projects_gantt_labels_own_a_dedicated_gutter():
    source = _without_comments(_function_source("renderRepoProjectsGantt"))
    # No runtime-built Tailwind column class: the tracks are inline, off one
    # custom property shared by the rows, the month axis and the grid lines.
    assert "grid-cols-[" not in source
    assert '"grid-template-columns:var(--fm-gantt-gutter) minmax(0,1fr)"' in source
    assert "--fm-gantt-gutter:${GUTTER}" in source
    assert source.count('style="${COLUMNS}"') == 2
    # The grid-line overlay opens at the timeline column (gutter + gap-3 + the
    # rows' px-1), so no tick line is ever drawn through a label.
    assert '"left:calc(var(--fm-gantt-gutter) + 1rem);right:0.25rem"' in source
    assert 'style="${TRACK}"' in source


def test_projects_gantt_labels_truncate_with_a_full_title_tooltip():
    source = _function_source("renderRepoProjectsGantt")
    assert '<span class="min-w-0 truncate">${escapeHtml(row.label)}</span>' in source
    # Both halves of a row - the truncated label and the bar - carry the title.
    assert source.count('title="${escapeHtml(barTitle(row))}"') == 2


def test_projects_gantt_classes_are_in_the_compiled_stylesheet():
    source = _function_source("renderRepoProjectsGantt")
    missing = sorted(
        token for token in _static_class_tokens(source)
        if not _stylesheet_defines(token))
    assert not missing, "gantt classes absent from tailwind.css: %s" % missing


def test_projects_link_issues_and_milestones_for_progress():
    # Progress = closed share of linked issues, falling back to the linked
    # milestone's issues; linked issue files are fetched in one batched read.
    assert "function projectProgress(project, issues)" in DASHBOARD_JS
    assert "issue.milestone === project.milestone" in DASHBOARD_JS
    assert "project.issues.includes(issue.number)" in DASHBOARD_JS


def test_issue_json_exposes_gantt_dates():
    # parseIssueJson surfaces startDate/endDate so issue bars can render.
    assert "startDate: Number(issue.startDate || 0) || 0" in DASHBOARD_JS
    assert "endDate: Number(issue.endDate || 0) || 0" in DASHBOARD_JS


# --- signed "dates" event canonicalization ----------------------------------
# The desktop client signs start/end date changes as a "dates" event; the
# worker's issue_event_content must byte-match IssueStore::contentForSigning.


def _load_issue_event_content():
    source = (SRC / "events.py").read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(SRC / "events.py"))
    selected = [
        node for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "issue_event_content"
    ]
    module = ast.fix_missing_locations(ast.Module(body=selected, type_ignores=[]))
    namespace = {}
    exec(compile(module, str(SRC / "events.py"), "exec"), namespace)
    return namespace["issue_event_content"]


def test_dates_event_content_matches_client_contentforsigning():
    issue_event_content = _load_issue_event_content()
    ev = {"type": "dates", "startDate": 1783206983436, "endDate": 1785206983436}
    assert issue_event_content(ev) == "1783206983436\x001785206983436"
    # Unset halves stay "0" so a start-only or end-only edit still verifies.
    assert issue_event_content({"type": "dates"}) == "0\x000"
    assert issue_event_content({"type": "dates", "startDate": "junk"}) == "0\x000"
    content_hash = hashlib.sha256(b"1783206983436\x001785206983436").hexdigest()
    assert len(content_hash) == 64
