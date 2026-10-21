#!/usr/bin/env python3
"""A refresh on any repo tab or record sub-page stays on that page (adhoc #25).

The repo browser is one prebuilt document (dashboard/repo.html) whose JS resolves
the tab / tree / blob / record from the URL, so "stay on this page after refresh"
is a two-part contract:

* the Worker must serve the repo document for every such deep link, and
* the bundle must restore that exact view on boot instead of snapping to Code.

Issue detail (/owner/repo/issues/<N>) used to be the one record kind that never
mirrored its number into the URL, so a refresh dropped back to the issues list;
these tests pin it deep-linking the same way pulls and discussions already do.
"""

import sys
from pathlib import Path

from _dashboard_bundle import assembled_dashboard_js

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
if str(SRC) not in sys.path:
    sys.path.insert(0, str(SRC))

import static_routes  # noqa: E402

DASHBOARD_JS = assembled_dashboard_js()


# --- Server side: every repo tab / record path serves the repo document ------


def test_worker_serves_repo_document_for_every_tab_and_record_deep_link():
    for tab in static_routes.REPO_TAB_ROUTES:
        assert static_routes.looks_like_repo_route(f"/owner/repo/{tab}"), tab
    # Record deep links (a refreshed/shared issue, PR or discussion detail).
    for kind in ("issues", "pulls", "discussions"):
        assert static_routes.looks_like_repo_route(f"/owner/repo/{kind}/5"), kind
    # A pull's section (Files changed, Commits, Badge) is addressable too.
    for section in ("conversation", "commits", "files", "badge"):
        assert static_routes.looks_like_repo_route(
            f"/owner/repo/pulls/5/{section}"), section
    # Code tree/blob deep links.
    assert static_routes.looks_like_repo_route("/owner/repo/tree/src")
    assert static_routes.looks_like_repo_route("/owner/repo/blob/README.md")
    # The branch comparison page (adhoc #2148): the Agents tab links an agent's
    # branch to /owner/repo/compare/<base>...<head>, and a hard refresh or a
    # pasted link used to get the Worker's JSON {"error": "not_found"} because
    # only tabs and tree/blob paths served the repo document.
    assert "compare" in static_routes.REPO_DOCUMENT_ROUTES
    assert static_routes.looks_like_repo_route(
        "/forkmesh/forkmesh/compare/main...agent/web-39-add-123-to-the-readme")
    assert static_routes.looks_like_repo_route("/owner/repo/compare/main...feature")
    # Reserved prefixes and API paths stay off the document route.
    assert not static_routes.looks_like_repo_route("/api/repo/x/compare")
    assert not static_routes.looks_like_repo_route("/dashboard/repo/compare/a...b")


# --- The compare page is the pull review's Files changed pane ---------------


def test_compare_page_renders_the_pull_review_files_pane():
    # adhoc #2148: the comparison used to paint the bare diff renderer (no file
    # explorer, no unified/split toggle, no viewed folds). It now paints the
    # same Files changed section a pull review does, minus the review gutters
    # (there is no pull request for an inline comment to belong to).
    compare = DASHBOARD_JS[
        DASHBOARD_JS.index("async function openRepoCompare")
        : DASHBOARD_JS.index("async function handlePullComposeSubmit")
    ]
    assert "const files = parsePatchStats(patch);" in compare
    assert "compare.result = {" in compare
    assert "const viewed = loadRepoCompareViewed(repo, compare);" in compare
    assert (
        "renderRepoPullFilesSection({ patch, files }, viewed, { reviewable: false })"
        in compare
    )
    assert "renderDiffFiles(parsed, [])" not in compare

    section = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoPullFilesSection")
        : DASHBOARD_JS.index("function repoPullFilesContext")
    ]
    assert "const reviewable = options.reviewable !== false;" in section
    assert "renderRepoComparePatch(pullPatch.patch, viewed, mode, pullPatch.imageDiffs, parsed)" in section
    # The comparison's renderer draws the same file blocks with review off.
    assert (
        'function renderRepoComparePatch(patch, viewed = new Set(), mode = "unified", imageDiffs = [], parsed = null)'
        in DASHBOARD_JS
    )
    assert "renderDiffFiles(parsed || parseDiffFiles(patch), imageDiffs, viewed, mode, false)" in DASHBOARD_JS

    # The toolbar handlers (layout toggles, filter, viewed, scroll-spy) all go
    # through repoPullFilesContext(), which now serves the comparison too.
    context = DASHBOARD_JS[
        DASHBOARD_JS.index("function repoPullFilesContext")
        : DASHBOARD_JS.index("function refreshRepoPullExplorer")
    ]
    assert 'if (detail?.kind === "pulls" && section.closest("[data-repo-record-detail]")) {' in context
    assert 'compare?.result && section.closest("[data-repo-compare]")' in context
    assert "reviewable: false," in context
    assert "viewed: loadRepoCompareViewed(repo, compare)," in context
    toggle = DASHBOARD_JS[
        DASHBOARD_JS.index("function toggleRepoPullViewed")
        : DASHBOARD_JS.index("function swapLucideIcon")
    ]
    assert "const context = repoPullFilesContext();" in toggle
    assert "context.saveViewed(viewed);" in toggle
    # Viewed marks persist per compared pair of commits.
    assert (
        "`forkmesh.compare-viewed.v1:${owner}/${name}@${base}..${head}`"
        in DASHBOARD_JS
    )


# --- Client side: the bundle restores that view on boot ----------------------


def test_issue_detail_is_a_recognized_record_deep_link():
    # On boot renderRepoDetail treats issues like pulls/discussions: a numeric
    # /owner/repo/issues/<N> segment is parsed as a record route so the detail
    # (not the list) is what re-opens. A pull may carry its section after the
    # number (/pulls/<N>/files), so the number is read from that segment alone.
    assert (
        '["pulls", "discussions", "issues"].includes(routeKind) && /^\\d+$/.test(recordNumber)'
        in DASHBOARD_JS
    )
    assert "normalizeRepoRecordSection(routeParts[4])" in DASHBOARD_JS


def test_feature_panel_loader_opens_the_deep_linked_issue_detail():
    panels = DASHBOARD_JS[
        DASHBOARD_JS.index("function loadRepoFeaturePanels")
        : DASHBOARD_JS.index("function updateRepoLiveCounts")
    ]
    # The issues branch opens the record detail directly when a deep link
    # carried its number, and only otherwise falls back to the list.
    assert 'recordRoute && recordRoute.kind === "issues" && recordRoute.number' in panels
    assert 'loadRepoRecordDetail(repo, "issues", recordRoute.number)' in panels
    assert "loadRepoIssues(repo);" in panels


def test_opening_an_issue_mirrors_its_number_into_the_url():
    assert (
        '["pulls", "discussions", "issues"].includes(kind) && /^\\d+$/.test(number)'
        in DASHBOARD_JS
    )


def test_issue_back_reloads_the_list_when_a_deep_link_never_fetched_it():
    # Stepping Back from a deep-linked issue detail (the list was never loaded)
    # must fetch it rather than flash an empty "No issues".
    assert (
        "if (state.issuesView.items.length) renderRepoIssues();\n"
        "          else loadRepoIssues(state.selectedRepo);"
        in DASHBOARD_JS
    )


def test_popstate_restores_issue_detail_and_list():
    # Browser Back/Forward between an issue detail and its list is handled the
    # same as pulls/discussions; for a pull it also steps between the record's
    # sections, which is a tab switch rather than another mirror read.
    assert (
        '["pulls", "discussions", "issues"].includes(kind)) {\n'
        '            const recordNumber = parts[3] ? safeDecodeURIComponent(parts[3]) : "";\n'
        "            if (/^\\d+$/.test(recordNumber)) {"
        in DASHBOARD_JS
    )
    assert "else loadRepoRecordDetail(repo, kind, recordNumber);" in DASHBOARD_JS
    assert "if (sameRecord) setRepoRecordSection(section);" in DASHBOARD_JS


def test_boot_only_loads_code_tree_when_code_is_visible():
    # A feature-tab deep link must not warm the hidden Code tree/README and
    # compete with the mirror request the visitor is actually waiting for.
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoDetail")
        : DASHBOARD_JS.index("function findRepository")
    ]
    assert 'if (initialTab === "code") {' in render
    assert 'state.loadedRepoTabs.code = true;' in render
    assert 'void openRepoCodeRoute(repo, routeKind, routePath, { explicitRef: Boolean(routeRef) });' in render
    assert '{ background: initialTab !== "code" }' not in render
    tree = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepositoryTree")
        : DASHBOARD_JS.index("async function loadRepositoryBlob")
    ]
    assert 'if (!background) setRepoTab("code");' in tree
    # Both URL rewrites (empty tree and populated tree) are background-gated.
    assert tree.count('navigateHistory(repoCodeLinkUrl(repo, "tree", path))') == 2
    assert tree.count('if (!background) navigateHistory(repoCodeLinkUrl(repo, "tree", path));') == 2
