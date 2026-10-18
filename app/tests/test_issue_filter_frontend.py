#!/usr/bin/env python3
"""Issues tab defaults to the Open view (issue #270).

The repo Issues tab used to list every issue regardless of state. These contract
tests pin the Open/Closed/All filter: Open is the default, closed issues are
opt-in, and the tab badge counts open issues.
"""

import json
import subprocess
from pathlib import Path

from _dashboard_shell import assembled_dashboard
from _dashboard_bundle import assembled_dashboard_js


PUBLIC = Path(__file__).resolve().parents[1] / "public"
WWW_PUBLIC = PUBLIC.parents[1] / "www" / "public"
# The dashboard shell is split into HTML partials and prebuilt before deploy;
# assert against the assembled document a browser actually receives.
DASHBOARD = assembled_dashboard()
# dashboard.js is likewise built from ordered public/dashboard/js/*.js fragments
# before deploy (see src/dashboard_bundle.py).
DASHBOARD_JS = assembled_dashboard_js()
STYLES = (WWW_PUBLIC / "styles.css").read_text(encoding="utf-8")
INDEX = (WWW_PUBLIC / "index.html").read_text(encoding="utf-8")


def test_dashboard_has_open_closed_all_filter_defaulting_to_open():
    assert 'class="issue-filter"' in DASHBOARD
    for state in ("open", "closed", "all"):
        assert f'data-state="{state}"' in DASHBOARD
    # The Open button is the active default.
    open_btn = DASHBOARD[
        DASHBOARD.index('id="issue-filter-open"') : DASHBOARD.index('id="issue-filter-closed"')
    ]
    assert "is-active" in open_btn
    assert 'aria-pressed="true"' in open_btn


def test_styles_define_issue_filter_control():
    assert ".issue-filter" in STYLES
    assert ".issue-filter button.is-active" in STYLES
    assert ".panel-header-aside" in STYLES


# --- Live dashboard (the page /owner/repo actually resolves to) -------------
# The repo browser users land on is dashboard.html + dashboard.js, so the
# Open-by-default behaviour has to live there to be visible on the website.


def test_dashboard_js_defaults_issue_filter_to_open():
    # The shared issues view starts on the Open filter.
    assert 'filter: "open"' in DASHBOARD_JS
    # The Issues panel renders GitHub's Open/Closed toggles built from these
    # states, with Open pre-selected. "All" is still a state, reached with
    # state:all in the search box (and linked from the Closed view).
    assert 'const statusTabs = [' in DASHBOARD_JS
    for marker in (
        '["open", "circle-dot", "Open", openCount]',
        '["closed", "check", "Closed", closedCount]',
    ):
        assert marker in DASHBOARD_JS
    assert '["all", "list", "All", openCount + closedCount]' not in DASHBOARD_JS
    assert 'data-repo-issue-query-set="state:all"' in DASHBOARD_JS
    assert 'data-dashboard-issue-filter="${stateName}"' in DASHBOARD_JS
    assert 'stateName === issuesView.filter ? "true" : "false"' in DASHBOARD_JS


def test_dashboard_js_filters_issue_list_by_state():
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoIssues")
        : DASHBOARD_JS.index("function setIssueFilter")
    ]
    assert 'if (issuesView.filter === "all") return true;' in render
    # "Open" is "not closed", matching the desktop advert / served counts so the
    # tab and list don't undercount non-"open" statuses like "reopened" (adhoc #96).
    assert 'if (issuesView.filter === "open") return issue.status !== "closed";' in render
    assert 'return issue.status === "closed";' in render


def test_dashboard_js_sorts_issues_by_most_recent_activity():
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoIssues")
        : DASHBOARD_JS.index("function setIssueFilter")
    ]
    assert "issueSortItems(filtered);" in render
    sorter = DASHBOARD_JS[
        DASHBOARD_JS.index("function issueSortItems")
        : DASHBOARD_JS.index("const ISSUE_VIEWS = [")
    ]
    assert 'sort = issueFilterState().sort' in sorter
    assert 'stamp(b, "updatedAtMs")' in sorter
    assert "stableNewest(a, b)" in sorter
    for option in ("newest", "oldest", "priority", "comments"):
        assert f'sort === "{option}"' in sorter


def test_dashboard_js_loads_issues_from_git_tree_and_counts_open():
    load = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoIssues")
        : DASHBOARD_JS.index("async function loadRepoCollection")
    ]
    # Published issues come from the repo's .forkmesh/issues/ git folder, not the inbox.
    assert 'repoLiveUrl(repo, "tree", { path: ".forkmesh/issues" })' in load
    assert "issueJsonPath(Number(entry.name))" in load
    # Issues are split into open/ and closed/ status folders (adhoc #14); the
    # loader lists both subdirs and reads each record from wherever it lives,
    # still accepting pre-split numbered folders at the root.
    assert '.forkmesh/issues/${statusDir}' in load
    assert "issueJsonPath(Number(entry.name), statusDir)" in load
    assert "parseIssueJson(blobText(blob), number)" in load
    # A new repository starts in Open via renderRepoDetail. Reloading this same
    # tab must not overwrite the visitor's status/search/metadata selections.
    assert 'state.issuesView.filter = "open";' not in load
    assert 'state.issuesView.query = "";' not in load
    assert '["closed", "all"].includes(state.issuesView.filter)' in load
    assert "await loadClosedIssues({ renderLoading: false });" in load
    # The badge counts open issues (status != "closed", matching the desktop
    # advert; adhoc #96) across the mirror list plus any persisted offline-owner
    # submissions folded in (merged; see issue #379). A creator-deleted issue is
    # excluded; an unauthorized deletion attempt still counts (adhoc #16).
    assert 'issue.status !== "closed" && !issue.deleted).length' in load
    assert 'setRepoTabCount("issues", openIssues);' in load
    assert "renderRepoIssues();" in load


def test_dashboard_js_open_load_pages_only_open_and_defers_closed():
    # Issue #427: the initial Issues load must fetch only the OPEN (and
    # unknown-status legacy) titles so they render fast, deferring the closed
    # history to a lazy loader. The open page is built from the open + legacy
    # folder maps, never the closed one.
    load = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoIssues")
        : DASHBOARD_JS.index("async function fetchIssuePage")
    ]
    # Three folder maps, split by status subdir; closed folders are recorded for
    # the lazy loader, not paged in now.
    for marker in ("const openPaths = new Map();",
                   "const closedPaths = new Map();",
                   "const legacyPaths = new Map();"):
        assert marker in load
    # The open page is the legacy + open folders only.
    assert "const pathByNumber = new Map([...legacyPaths, ...openPaths]);" in load
    # Every open issue is fetched in bounded batches so the collection paginator
    # can reach the complete history, not only the first 50 records.
    assert "fetchIssuePages(" in load
    assert "repo, pathByNumber, numbered, issueReadOptions" in load
    assert "numbered.slice(0, 50)" not in load
    # The closed set is stashed on the view and marked not-yet-loaded (unless the
    # closed/ subdir is empty, in which case there's nothing to lazy-load).
    assert "state.issuesView.closedPaths = closedPaths;" in load
    assert "state.issuesView.closedLoaded = closedPaths.size === 0;" in load


def test_dashboard_js_pins_issue_reads_to_one_git_snapshot():
    # The visitor gateway can rotate consecutive calls across mirrors that are
    # briefly on different commits. Pin status trees, open blobs, their retry,
    # and the lazy closed load to the root tree's immutable commit so a stale
    # listing cannot turn a legitimately deleted issue into an unreadable row.
    load = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoIssues")
        : DASHBOARD_JS.index("async function loadRepoCollection")
    ]
    assert "const issueCommit = immutableGitCommit(tree.commit);" in load
    assert "const issueReadOptions = issueCommit ? { ref: issueCommit } : {};" in load
    assert "...issueReadOptions" in load
    assert "state.issuesView.ref = issueCommit;" in load

    page = DASHBOARD_JS[
        DASHBOARD_JS.index("async function fetchIssuePage")
        : DASHBOARD_JS.index("async function loadClosedIssues")
    ]
    assert "async function fetchIssuePage(repo, pathByNumber, dirs, options = {})" in page
    assert "async function fetchIssuePages(repo, pathByNumber, numbers, options = {})" in page
    assert page.count("fetchRepoBlobs(") == 2
    assert page.count(", options)") >= 2

    closed = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadClosedIssues")
        : DASHBOARD_JS.index("async function loadRepoCollection")
    ]
    assert "view.ref ? { ref: view.ref } : {}" in closed


def test_dashboard_js_issue_counts_come_from_folder_listing_not_the_page():
    # Issue #427: the open/closed counts were computed from the (50-capped,
    # open+closed mixed) page of loaded blobs, so a repo with more issues than
    # fit on the page - or with many closed issues - showed wrong counts. They
    # now come from the full folder listing (openPaths/closedPaths sizes), which
    # matches the folder-based tally the Mirror nodes tab reports. The one thing
    # added on top is this browser's own accepted submissions, which no listing
    # can carry yet (issue #609) - a badge that undercounts the rows on screen
    # reads as "your issue didn't land". It is still not the loaded page:
    # notYetServed counts only numbers the folder listing lacks, and every
    # fetched blob came from that listing.
    load = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoIssues")
        : DASHBOARD_JS.index("async function fetchIssuePage")
    ]
    assert (
        "const openIssues = openPaths.size + legacyOpen + notYetServed\n"
        "        + pendingOpened - pendingClosed;"
    ) in load
    assert "(issue) => !servedNumbers.has(Number(issue.number))" in load
    assert "const closedIssues = closedPaths.size + legacyClosed + pendingClosed;" in load
    # The one other correction: a close or reopen signed from this browser
    # that the listing predates moves its row across the split (issue #715).
    assert 'setRepoCollectionCounts("issues", openIssues, closedIssues);' in load


def test_dashboard_js_lazy_loads_closed_issues_on_filter_switch():
    # The Closed/All filter pages in the closed history on its first open, via a
    # dedicated loader that reuses the shared page fetcher and appends to the
    # already-loaded open items.
    setter = DASHBOARD_JS[
        DASHBOARD_JS.index("function setIssueFilter")
        : DASHBOARD_JS.index("async function loadRepoIssues")
    ]
    assert '(filter === "closed" || filter === "all")' in setter
    assert "!state.issuesView.closedLoaded && !state.issuesView.closedLoading" in setter
    assert "loadClosedIssues();" in setter
    closed = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadClosedIssues")
        : DASHBOARD_JS.index("async function loadRepoCollection")
    ]
    assert "async function loadClosedIssues({ renderLoading = true } = {})" in closed
    assert "if (view.closedLoaded || view.closedLoading) return;" in closed
    assert "if (renderLoading) renderRepoIssues();" in closed
    assert "fetchIssuePages(" in closed
    assert "numbers.slice(0, 50)" not in closed
    assert "view.closedLoaded = true;" in closed


def test_dashboard_js_shared_issue_page_fetcher_batches_and_retries():
    # Both the open and closed loaders go through fetchIssuePage: ONE batched
    # /blobs read, a single retry of the misses, and individual failover reads.
    page = DASHBOARD_JS[
        DASHBOARD_JS.index("async function fetchIssuePage")
        : DASHBOARD_JS.index("async function loadClosedIssues")
    ]
    assert "fetchRepoBlobs(" in page
    assert "missing = stillMissing;" in page
    assert "fetchMissingRepoBlobs(" in page
    assert "placeholderIssue" not in page
    # Complete histories are split into bounded batches below the gateway's
    # request cap instead of truncating the issue list at 50 records.
    assert "async function fetchIssuePages" in page
    assert "numbers.slice(offset, offset + 50)" in page


def test_dashboard_js_shows_per_tab_counts():
    # Every repo tab carries a count badge, filled from the bundled root-tree
    # tallies plus the open-issue and mirror counts.
    assert 'data-dashboard-repo-tab-count="${tab}"' in DASHBOARD_JS
    assert "function applyServedCounts(counts)" in DASHBOARD_JS
    assert 'setRepoTabCount("issues", openIssues);' in DASHBOARD_JS
    assert "Number.isFinite(Number(counts.openPulls))" in DASHBOARD_JS
    assert 'setRepoTabCount("pulls", openPulls);' in DASHBOARD_JS
    assert 'setRepoTabCount("discussions", Number(counts.discussions));' in DASHBOARD_JS
    assert "const mirrorCount = mirrors.length;" in DASHBOARD_JS
    assert 'setRepoTabCount("mirrors", mirrorCount);' in DASHBOARD_JS


def test_dashboard_js_issue_header_counts_open_not_total():
    # Issue #397: every issue count the web badges must be the OPEN count, not
    # open+closed. The served tree tallies now carry an open/closed split, and
    # applyServedCounts badges the Issues tab from the open count (falling back
    # to the bundled total only for older hosts), filling the panel's
    # "N Open / N Closed" header from the served split.
    served = DASHBOARD_JS[
        DASHBOARD_JS.index("function applyServedCounts(counts)")
        : DASHBOARD_JS.index("function issueMatchesQuery")
    ]
    assert "Number.isFinite(Number(counts.openIssues))" in served
    assert "? Number(counts.openIssues)" in served
    assert ": Number(counts.issues);" in served
    assert 'setRepoTabCount("issues", openIssues);' in served
    assert 'const closedIssues = Number(counts.closedIssues);' in served
    assert 'setRepoCollectionCounts("issues", openIssues, closedIssues);' in served


def test_dashboard_js_refreshes_exact_counts_before_lazy_tabs_are_viewed():
    tree = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepositoryTree")
        : DASHBOARD_JS.index("async function loadRepositoryBlob")
    ]
    assert "if (!path && data.counts)" in tree
    assert "applyServedCounts(data.counts);" in tree

    served = DASHBOARD_JS[
        DASHBOARD_JS.index("async function refreshServedCounts(repo)")
        : DASHBOARD_JS.index("function issueMatchesQuery")
    ]
    assert 'repoLiveUrl(repo, "tree", { path: "" })' in served
    assert "updateRepoLiveCounts(repo, data.counts);" in served
    assert "applyServedCounts(data.counts);" in served

    # A feature route fetches only its own record folder. It must separately
    # request the live root counters so every header (including an unopened PR
    # tab while viewing Issues) replaces its stale catalog seed.
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoDetail")
        : DASHBOARD_JS.index("function findRepository")
    ]
    assert 'if (initialTab !== "code" || ((routeKind === "tree" || routeKind === "blob") && routePath)) {' in render
    assert "refreshServedCounts(repo);" in render

    activation = DASHBOARD_JS[
        DASHBOARD_JS.index("function activateRepoTab(tab)")
        : DASHBOARD_JS.index("function repoPathParts(path)")
    ]
    assert 'if (tab === "issues" || tab === "pulls") {' in activation
    assert "void refreshServedCounts(state.selectedRepo);" in activation
    # Returning to Issues must re-read its records instead of repainting the
    # array captured during the first visit.
    assert "loadRepoIssues(state.selectedRepo);" in activation

    live_signal = DASHBOARD_JS[
        DASHBOARD_JS.index("function onLiveMirrorSignal(event)")
        : DASHBOARD_JS.index("function renderRepoRelease(")
    ]
    assert "void refreshServedCounts(repo);" in live_signal
    assert live_signal.index("void refreshServedCounts(repo);") < live_signal.index(
        "refreshOpenRepoMirrors();")

    repos = (
        Path(__file__).resolve().parents[2]
        / "desktop"
        / "src"
        / "MainWindowRepoDetail.cpp"
    ).read_text(encoding="utf-8")
    desktop_refresh = repos[
        repos.index("void MainWindow::refreshRepoTabCounts()")
        : repos.index("void MainWindow::loadRepoFileTree()")
    ]
    assert "reloadIssuesInBackground();" in desktop_refresh
    assert "reloadPullsInBackground();" in desktop_refresh


def test_dashboard_js_exposes_open_closed_all_pull_filters_and_counts_them():
    filters = DASHBOARD_JS[
        DASHBOARD_JS.index("function pullFilterState()")
        : DASHBOARD_JS.index("function renderRepoCollection(kind)")
    ]
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoCollection(kind)")
        : DASHBOARD_JS.index("function discussionsUnavailableHtml")
    ]
    assert "function pullMatchesFilter(pull, filter = pullFilterState())" in filters
    assert 'return filter === "open" ? pullIsOpen(pull) : !pullIsOpen(pull);' in filters
    assert "loadedItems.filter((item) => pullMatchesFilter(item, pullFilter)" in render
    assert "pullEmptyLabel(pullFilter)" in render
    assert "state.pullsView.query" in render

    header = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderPullListHeader")
        : DASHBOARD_JS.index("function syncPullCollectionChrome")
    ]
    assert 'data-dashboard-pull-filter="${name}"' in header
    assert '["open", "git-pull-request", "Open", openCount]' in header
    assert '["closed", "check", "Closed", closedCount]' in header
    parse = DASHBOARD_JS[
        DASHBOARD_JS.index("function parsePullQuery")
        : DASHBOARD_JS.index("function applyPullQueryText")
    ]
    assert 'if (key === "state" && ["open", "closed", "all"].includes(lower)) status = lower;' in parse.replace("else if", "if")

    click_handler = DASHBOARD_JS[
        DASHBOARD_JS.index(
            'const pullFilterButton = event.target.closest("[data-dashboard-pull-filter]")')
        : DASHBOARD_JS.index(
            'const repoCollectionPageButton = event.target.closest("[data-repo-collection-page]")')
    ]
    assert 'setPullFilter(pullFilterButton.dataset.dashboardPullFilter || "open");' in click_handler

    load = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoRecordsFromMirror")
        : DASHBOARD_JS.index("function renderRepoCollectionPagination")
    ]
    assert 'config.dir === ".forkmesh/pulls"' in load
    assert "? listedDirs" in load
    assert ": listedDirs.slice(0, 50);" in load
    assert "paths.slice(offset, offset + 50)" in load

    served = DASHBOARD_JS[
        DASHBOARD_JS.index("function applyServedCounts(counts)")
        : DASHBOARD_JS.index("async function refreshServedCounts(repo)")
    ]
    assert "Number.isFinite(Number(counts.openPulls))" in served
    assert 'setRepoTabCount("pulls", openPulls);' in served
    assert 'setRepoCollectionCounts("pulls", openPulls, Number(counts.closedPulls));' in served

    load_collection = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoCollection")
        : DASHBOARD_JS.index("async function loadRepoCommits")
    ]
    assert "const openPulls = items.filter(pullIsOpen).length;" in load_collection


def test_advertised_catalog_issue_count_is_open_only():
    # adhoc #29: the catalog's issueCount seeds the Issues tab badge on the FIRST
    # repo render (before the live-served open/closed split from #397 arrives),
    # and catalog.py documents it as the OPEN count. The desktop advert must
    # therefore count open issues, not every .forkmesh/issues/<n> directory
    # (closed issues keep their directory on disk, so a bare dir count showed all
    # issues). mirrorIssueCount now delegates to a status-aware open counter.
    internal = (
        Path(__file__).resolve().parents[2]
        / "desktop"
        / "src"
        / "MainWindowInternal4.cpp"
    ).read_text(encoding="utf-8")
    assert "int mirrorOpenIssueCount(const QString &mirrorPath" in internal
    open_counter = internal[
        internal.index("int mirrorOpenIssueCount")
        : internal.index("int mirrorIssueCount")
    ]
    # Reads each record's status; anything not "closed" counts as open,
    # matching the web's parseIssueJson default.
    assert 'issue-%1.json' in open_counter
    assert 'value(QStringLiteral("status"))' in open_counter
    assert 'if (status != QLatin1String("closed"))' in open_counter
    # The advertised issueCount is the open count, not the raw directory tally.
    issue_count = internal[
        internal.index("int mirrorIssueCount")
        : internal.index("int mirrorIssueCount") + 260
    ]
    assert "return mirrorOpenIssueCount(mirrorPath, branch);" in issue_count
    assert 'mirrorNumberedDirCount(mirrorPath, branch, QStringLiteral(".forkmesh/issues"))' not in issue_count


def test_dashboard_js_retries_and_reports_unreadable_issues_without_fake_rows():
    # adhoc: the Mirror nodes tab counted 11 issues while the Issues tab showed
    # only 7 - loadRepoIssues silently dropped any folder whose issue-N.json blob
    # came back null from the batched read. It must now retry the misses once and,
    # for anything still unreadable, surface one collection warning without
    # fabricating titles, authors, or timestamps.
    load = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoIssues")
        : DASHBOARD_JS.index("async function loadRepoCollection")
    ]
    # Null blobs are collected as misses, not dropped.
    assert "else missing.push(number);" in load
    # The misses are re-fetched in a batch and then individually through fresh
    # mirror routing before giving up.
    assert "missing.map((number) => pathByNumber.get(number))" in load
    assert "missing = stillMissing;" in load
    assert "fetchMissingRepoBlobs(" in load
    assert "placeholderIssue" not in load
    # The unreadable-record gap is recorded for the UI.
    assert "state.issuesView.missing = missing;" in load
    assert "issuesView.truncated" not in load


def test_dashboard_js_issue_panel_warns_only_for_unreadable_issues():
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoIssues")
        : DASHBOARD_JS.index("function setIssueFilter")
    ]
    # A warning bar with a Retry button renders when issue records are unreadable.
    # Older issues are reachable through the list pagination and should not show
    # the obsolete 50-item truncation warning.
    assert "couldn't be read from the live mirror" in render
    assert "beyond the first 50" not in render
    assert "data-repo-issues-reload" in render
    # The Retry button reloads the Issues tab from the live mirror.
    reload = DASHBOARD_JS[
        DASHBOARD_JS.index("[data-repo-issues-reload]")
        : DASHBOARD_JS.index("[data-repo-issues-reload]") + 400
    ]
    assert "loadRepoIssues(state.selectedRepo);" in reload


def test_dashboard_issues_refreshes_from_mirrors_on_load_and_button_click():
    toolbar = DASHBOARD_JS[
        DASHBOARD_JS.index('function renderRepoCollectionPanel(kind, repo')
        :
    ]
    assert "data-repo-issues-refresh" in toolbar
    assert "data-repo-issues-refresh-icon" in toolbar
    assert "data-repo-issues-refresh-label" in toolbar

    loader = DASHBOARD_JS[
        DASHBOARD_JS.index("function setRepoIssuesRefreshing")
        : DASHBOARD_JS.index("async function fetchIssuePage")
    ]
    assert "setRepoIssuesRefreshing(true);" in loader
    assert "setRepoIssuesRefreshing(false);" in loader
    assert 'classList.toggle("animate-spin"' in loader
    assert 'loadingHtml("Loading issues from the live mirror...")' in loader
    assert 'repoLiveUrl(repo, "tree", { path: ".forkmesh/issues" })' in loader

    initial = DASHBOARD_JS[
        DASHBOARD_JS.index("function loadRepoFeaturePanels")
        : DASHBOARD_JS.index("function renderRepoCollectionFilter")
    ]
    assert 'else if (active === "issues")' in initial
    assert "loadRepoIssues(repo);" in initial

    wiring = DASHBOARD_JS[
        DASHBOARD_JS.index('"[data-repo-issues-refresh], [data-repo-issues-reload]"')
        : DASHBOARD_JS.index('"[data-repo-issues-refresh], [data-repo-issues-reload]"') + 360
    ]
    assert "loadRepoIssues(state.selectedRepo);" in wiring


def test_homepage_links_to_active_nodes():
    # The homepage surfaces a link to the live network/active-nodes page.
    assert 'href="/network"' in INDEX
    assert "View active nodes" in INDEX


def test_dashboard_js_batches_record_reads_and_lazy_loads_tabs():
    # Record lists (issues/pulls/discussions) must fetch their record files
    # through ONE batched /blobs request - fetching each record as its own
    # /blob call fired 50+ parallel requests per page view and tripped the
    # relay's per-repo rate limit - and must only load on the tab's FIRST
    # view, not eagerly on every repo open.
    assert "async function fetchRepoBlobs(repo, paths, options = {})" in DASHBOARD_JS
    assert "/blobs?" in DASHBOARD_JS
    assert 'query.append("path", path)' in DASHBOARD_JS
    # Both list loaders go through the batch, never a per-record /blob loop.
    load = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoIssues")
        : DASHBOARD_JS.index("async function loadRepoCollection")
    ]
    assert "fetchRepoBlobs(" in load
    assert 'repoLiveUrl(repo, "blob"' not in load
    records = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoRecordsFromMirror")
        : DASHBOARD_JS.index("function renderRepoCollectionPagination")
    ]
    assert "fetchRepoBlobs(" in records
    assert 'repoLiveUrl(repo, "blob"' not in records
    # Issues and commits are lazy like pulls/discussions: repo open loads the
    # visible code view plus mirror summary; hidden tab data waits for the first
    # tab view.
    panels = DASHBOARD_JS[
        DASHBOARD_JS.index("function loadRepoFeaturePanels")
        : DASHBOARD_JS.index("function updateRepoLiveCounts")
    ]
    assert "state.loadedRepoTabs = state.loadedRepoTabs || {};" in panels
    repo_render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoDetail"):
        DASHBOARD_JS.index("function findRepository")
    ]
    assert "state.loadedRepoTabs = {};" in repo_render
    assert '["branches", "commits", "issues", "projects", "pulls", "discussions", "releases", "insights", "sizemap", "agents"].includes(tab)' in DASHBOARD_JS
    # The tab badge still fills immediately from the root tree's served counts,
    # badging the OPEN issue count (issue #397) rather than the open+closed total.
    assert 'setRepoTabCount("issues", openIssues);' in DASHBOARD_JS


# --- Free-text issue search (adjacent to the Open/Closed/All filter) --------
# The Issues panel already had a GitHub-style search box (data-repo-filter-
# query="issues") sitting there decoratively with no listener behind it.
# These tests pin it now actually filtering the already-loaded issue list.


def test_issues_view_state_carries_a_search_query():
    state = DASHBOARD_JS[
        DASHBOARD_JS.index("issuesView: {")
        : DASHBOARD_JS.index("projectsView:")
    ]
    for marker in (
        'filter: "open"',
        'items: []',
        'query: ""',
        'author: "", label: "", milestone: "", assignee: ""',
        'type: "", sort: "recently_updated"',
    ):
        assert marker in state


def test_issue_search_matches_number_title_body_author_and_meta():
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function issueMatchesQuery")
        : DASHBOARD_JS.index("function renderRepoIssues")
    ]
    for marker in (
        'if (!q) return true;',
        '"#" + issue.number, String(issue.number), issue.title, issue.body,',
        'issue.author, issue.meta,',
        '.join(" ").toLowerCase();',
        'return haystack.includes(q);',
    ):
        assert marker in render


def test_render_repo_issues_applies_the_search_filter_after_the_status_filter():
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoIssues")
        : DASHBOARD_JS.index("function setIssueFilter")
    ]
    # The three pinned status-filter lines stay exactly as before...
    assert 'if (issuesView.filter === "all") return true;' in render
    assert 'if (issuesView.filter === "open") return issue.status !== "closed";' in render
    assert 'return issue.status === "closed";' in render
    # ...with the search predicate layered on afterward, not replacing them.
    # The predicate runs over localFilterQueries: this tab's own search box is
    # the only one that narrows it, so text left in the header search box can
    # no longer empty the list (issue #673).
    assert '}).filter((issue) => issueMatchesFilters(issue, issueFilterState()))' in render
    assert '.filter((issue) => localFilterQueries(issuesView.query)' in render
    assert 'pageFilterQueries(' not in render
    assert '.every((query) => issueMatchesQuery(issue, query)));' in render


def test_issue_metadata_dropdowns_are_real_filters_with_live_options():
    toolbar = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoCollectionFilter")
        : DASHBOARD_JS.index("function renderRepoDetail")
    ]
    assert 'data-repo-issue-filter="${issueFilter}" ' in toolbar
    assert "label, issueFilterOptions(field), field, true)" in DASHBOARD_JS
    issue_list = DASHBOARD_JS[
        DASHBOARD_JS.index("const listFilters = [")
        : DASHBOARD_JS.index("const filterBar =", DASHBOARD_JS.index("const listFilters = ["))
    ]
    for marker in (
        '["Author", "author"]',
        '["Labels", "label"]',
        '["Milestones", "milestone"]',
        '["Assignees", "assignee"]',
        '["Types", "type"]',
        '"sort"]',
    ):
        assert marker in issue_list

    options = DASHBOARD_JS[
        DASHBOARD_JS.index("function issueFilterValues")
        : DASHBOARD_JS.index("function syncIssueFilterControls")
    ]
    for marker in (
        'if (field === "author") add(issue.author);',
        'else if (field === "label") (issue.labels || []).forEach(add);',
        'else if (field === "milestone") add(issue.milestone);',
        'else if (field === "assignee") (issue.assignees || []).forEach(add);',
        '{ value: ISSUE_FILTER_NONE, label: "No milestone" }',
        '{ value: ISSUE_FILTER_ME, label: "Assigned to me" }',
        '{ value: ISSUE_FILTER_ASSIGNED, label: "Anyone assigned" }',
        '{ value: ISSUE_FILTER_NONE, label: "Unassigned" }',
    ):
        assert marker in options


def test_issue_metadata_filter_change_is_delegated_and_rerenders():
    listener = DASHBOARD_JS[
        DASHBOARD_JS.index('document.addEventListener("change", (event) => {')
        : DASHBOARD_JS.index('document.addEventListener("dragstart"')
    ]
    assert 'event.target.closest?.("[data-repo-issue-filter]")' in listener
    assert 'issueFilter.dataset.repoIssueFilter || ""' in listener
    assert "issueFilter.value" in listener
    assert "setIssueCollectionFilter(" in listener

    setter = DASHBOARD_JS[
        DASHBOARD_JS.index("function setIssueCollectionFilter")
        : DASHBOARD_JS.index("function renderRepoIssues")
    ]
    assert '["author", "label", "milestone", "assignee", "mentions", "involves", "type", "sort"].includes(field)' in setter
    assert 'issueFilterState()[field] = String(value || "");' in setter
    assert "renderRepoIssues();" in setter


def _run_issue_filter_javascript(body):
    """Exercise the shipped matcher/options implementation under Node."""
    transformed = DASHBOARD_JS.replace(
        "  void bootDashboard();",
        """  globalThis.__issueFilterExports = {
    state,
    issueFilterOptions,
    issueMatchesFilters,
    issueSortItems,
    pullIsOpen,
    pullMatchesFilter,
    parseIssueQuery,
    serializeIssueQuery,
    activeIssueView,
    issueLabelHue,
    issuePickerCandidates,
    ISSUE_FILTER_NONE,
    ISSUE_FILTER_ME,
  };""",
    )
    assert "__issueFilterExports" in transformed
    script = "const SOURCE = " + json.dumps(transformed) + ";\n" + r"""
const assert = require("assert");
const vm = require("vm");
global.location = {
  origin: "https://forkmesh.test",
  hostname: "forkmesh.test",
  pathname: "/acme/widgets/issues",
  search: "",
};
global.localStorage = { getItem() { return null; }, setItem() {}, removeItem() {} };
const emptyClassList = { add() {}, remove() {}, toggle() {}, contains() { return false; } };
global.document = {
  body: { classList: emptyClassList },
  documentElement: { dataset: {}, style: {} },
  cookie: "",
  querySelector() { return null; },
  querySelectorAll() { return []; },
  addEventListener() {},
};
global.window = {
  matchMedia() { return { addEventListener() {}, addListener() {} }; },
  addEventListener() {},
  history: { pushState() {} },
  lucide: { createIcons() {} },
  setTimeout() {},
  setInterval() {},
  clearInterval() {},
};
global.navigator = {};
vm.runInThisContext(SOURCE);
const {
  state,
  issueFilterOptions,
  issueMatchesFilters,
  issueSortItems,
  pullIsOpen,
  pullMatchesFilter,
  parseIssueQuery,
  serializeIssueQuery,
  activeIssueView,
  issueLabelHue,
  issuePickerCandidates,
  ISSUE_FILTER_NONE,
  ISSUE_FILTER_ME,
} = global.__issueFilterExports;
""" + body
    result = subprocess.run(
        ["node"], input=script, check=True, capture_output=True, text=True)
    assert result.stderr == ""


def test_issue_metadata_filters_match_real_issue_fields_and_combine():
    _run_issue_filter_javascript(r"""
state.session = { nodeName: "viewer" };
state.issuesView.items = [
  { author: "Alice", labels: ["bug", "backend"], milestone: "v1", assignees: ["Sam"] },
  { author: "Bob", labels: ["enhancement"], milestone: "v2", assignees: ["viewer"] },
  { author: "Alice", labels: ["documentation"], milestone: "", assignees: [] },
];
const [first, second, third] = state.issuesView.items;

assert.deepEqual(
  issueFilterOptions("author").map((option) => option.label),
  ["Any author", "Alice", "Bob"],
);
assert.deepEqual(
  issueFilterOptions("label").map((option) => option.label),
  ["Any label", "backend", "bug", "documentation", "enhancement"],
);
assert.deepEqual(
  issueFilterOptions("milestone").map((option) => option.label),
  ["Any milestone", "No milestone", "v1", "v2"],
);
assert.deepEqual(
  issueFilterOptions("assignee").map((option) => option.label),
  ["Anyone", "Assigned to me", "Anyone assigned", "Unassigned", "Sam", "viewer"],
);

assert(issueMatchesFilters(first, { author: "alice", label: "BUG", milestone: "v1", assignee: "sam" }));
assert(!issueMatchesFilters(second, { author: "alice", label: "", milestone: "", assignee: "" }));
assert(issueMatchesFilters(second, { author: "", label: "", milestone: "", assignee: ISSUE_FILTER_ME }));
assert(issueMatchesFilters(third, { author: "", label: "", milestone: ISSUE_FILTER_NONE, assignee: ISSUE_FILTER_NONE }));
assert(!issueMatchesFilters(first, { author: "", label: "", milestone: ISSUE_FILTER_NONE, assignee: "" }));
""")


def test_issue_type_and_sort_controls_are_functional():
    _run_issue_filter_javascript(r"""
const standard = { number: 1, wantsAgent: false, createdAtMs: 100, updatedAtMs: 400, commentCount: 1, priority: 0 };
const agent = { number: 2, wantsAgent: true, createdAtMs: 200, updatedAtMs: 300, commentCount: 4, priority: 2 };
const urgent = { number: 3, wantsAgent: false, createdAtMs: 300, updatedAtMs: 200, commentCount: 2, priority: 1 };

assert(issueMatchesFilters(agent, { type: "agent" }));
assert(!issueMatchesFilters(standard, { type: "agent" }));
assert(issueMatchesFilters(standard, { type: "standard" }));
assert.deepEqual(issueSortItems([standard, agent, urgent], "newest").map((item) => item.number), [3, 2, 1]);
assert.deepEqual(issueSortItems([standard, agent, urgent], "oldest").map((item) => item.number), [1, 2, 3]);
assert.deepEqual(issueSortItems([standard, agent, urgent], "priority").map((item) => item.number), [3, 2, 1]);
assert.deepEqual(issueSortItems([standard, agent, urgent], "comments").map((item) => item.number), [2, 3, 1]);
""")


def test_issue_rows_use_real_labels_and_live_comment_counts():
    parser = DASHBOARD_JS[
        DASHBOARD_JS.index("function parseIssueJson")
        : DASHBOARD_JS.index("function issueDependencyNumbers")
    ]
    records = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoRecordList")
        : DASHBOARD_JS.index("async function loadRepoCollection")
    ]
    assert 'event?.type === "comment"' in parser
    assert 'deletedEventIds.has(String(event.id || ""))' in parser
    assert "commentCount," in parser
    assert 'data-repo-issue-label="${escapeHtml(label)}"' in records
    assert "Number(item.commentCount || 0)" in records
    assert "${formatCount(commentCount)}" in records


def test_pull_state_filter_keeps_closed_and_merged_history_visible():
    _run_issue_filter_javascript(r"""
assert(pullIsOpen({ state: "OPEN" }));
assert(pullMatchesFilter({ state: "open" }, "open"));
assert(!pullMatchesFilter({ state: "closed" }, "open"));
assert(pullMatchesFilter({ state: "closed" }, "closed"));
assert(pullMatchesFilter({ state: "merged" }, "closed"));
assert(pullMatchesFilter({ state: "unknown" }, "closed"));
assert(pullMatchesFilter({ state: "open" }, "all"));
assert(pullMatchesFilter({ state: "merged" }, "all"));
""")


def test_issue_search_box_is_wired_to_a_delegated_input_listener():
    assert 'event.target?.matches?.(\'[data-repo-filter-query="issues"]\')' in DASHBOARD_JS
    listener = DASHBOARD_JS[
        DASHBOARD_JS.index(
            'event.target?.matches?.(\'[data-repo-filter-query="issues"]\')')
        : DASHBOARD_JS.index(
            'event.target?.matches?.("[data-repo-branch-search]")')
    ]
    # The box carries qualifiers and free text together, so the listener parses
    # it back into filters (which re-renders) instead of storing raw text.
    assert 'applyIssueQueryText(event.target.value || "");' in listener
    assert "renderIssueQueryHighlight(event.target);" in listener


def test_issue_and_pull_search_boxes_reflect_live_state_and_rerender():
    assert 'value="${escapeHtml(`${serializePullQuery()} `)}"' in DASHBOARD_JS
    assert 'event.target?.matches?.(\'[data-repo-filter-query="pulls"]\')' in DASHBOARD_JS
    pull_listener = DASHBOARD_JS[
        DASHBOARD_JS.index(
            'event.target?.matches?.(\'[data-repo-filter-query="pulls"]\')')
        : DASHBOARD_JS.index(
            'event.target?.matches?.("[data-repo-branch-search]")')
    ]
    assert 'applyPullQueryText(event.target.value || "");' in pull_listener
    apply = DASHBOARD_JS[
        DASHBOARD_JS.index("function applyPullQueryText")
        : DASHBOARD_JS.index("function syncPullQueryBox")
    ]
    assert "state.pullsView.query = parsed.query;" in apply
    assert "setPullFilter(parsed.status);" in apply
    set_filter = DASHBOARD_JS[
        DASHBOARD_JS.index("function setPullFilter")
        : DASHBOARD_JS.index("const PULL_VIEWS")
    ]
    assert "state.repoCollectionPages.pulls = 1;" in set_filter
    assert 'renderRepoCollection("pulls");' in set_filter


def test_opening_a_different_repo_resets_the_issue_view_state():
    # Refresh and tab re-entry preserve the current controls, while opening a
    # different repository must start from Open with no search or metadata
    # filters from the previous repository.
    render = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoDetail")
        : DASHBOARD_JS.index("navigateHistory(detailPath)")
    ]
    reset = render[
        render.index("state.issuesView = {")
        : render.index("state.loadedRepoTabs = {};")
    ]
    for marker in (
        'filter: "open"',
        'items: []',
        'query: ""',
        'author: "", label: String(routeQuery.get("label") || "").trim(), milestone: "", assignee: ""',
        'type: "", sort: "recently_updated"',
    ):
        assert marker in reset
    assert 'state.pullsView = { filter: "open", query: "" };' in render


def test_issue_search_qualifiers_round_trip_through_the_filters():
    _run_issue_filter_javascript(r"""
state.session = { nodeName: "viewer" };
state.issuesView.filter = "open";
state.issuesView.query = "";
Object.assign(state.issuesView.filters, {
  author: "Alice", label: "help wanted", milestone: ISSUE_FILTER_NONE,
  assignee: ISSUE_FILTER_ME, type: "agent", sort: "oldest",
});
state.issuesView.query = "parser";
const text = serializeIssueQuery();
assert.equal(
  text,
  'is:issue state:open author:Alice assignee:@me label:"help wanted" no:milestone type:agent sort:created-asc parser',
);

const parsed = parseIssueQuery(text);
assert.equal(parsed.status, "open");
assert.equal(parsed.query, "parser");
assert.deepEqual(
  { ...parsed.filters },
  { author: "Alice", label: "help wanted", milestone: ISSUE_FILTER_NONE, assignee: ISSUE_FILTER_ME,
    mentions: "", involves: "", type: "agent", sort: "oldest" },
);

// No state qualifier searches every issue, as on GitHub; unknown qualifiers
// stay as free text rather than being silently dropped.
const loose = parseIssueQuery("is:issue label:bug colour:red api");
assert.equal(loose.status, "all");
assert.equal(loose.filters.label, "bug");
assert.equal(loose.query, "colour:red api");
assert.equal(parseIssueQuery("author:@me").filters.author, "viewer");
assert.equal(parseIssueQuery("no:assignee").filters.assignee, ISSUE_FILTER_NONE);
""")


def test_issue_views_match_their_filters_and_mentions_stand_alone():
    _run_issue_filter_javascript(r"""
state.session = { nodeName: "sam" };
const filters = state.issuesView.filters;
Object.assign(filters, { author: "", assignee: "", mentions: "", involves: "" });
assert.equal(activeIssueView().id, "issues");
Object.assign(filters, { assignee: ISSUE_FILTER_ME });
assert.equal(activeIssueView().id, "assigned");
Object.assign(filters, { assignee: "", author: "SAM" });
assert.equal(activeIssueView().id, "created");
Object.assign(filters, { author: "", mentions: ISSUE_FILTER_ME });
assert.equal(activeIssueView().id, "mentioned");

const mention = { body: "Ping @sam about this", events: [] };
const longer = { body: "Ping @samantha", events: [{ type: "comment", body: "email sam@example.test" }] };
const inComment = { body: "", events: [{ type: "comment", body: "cc @Sam." }] };
const view = { mentions: ISSUE_FILTER_ME };
assert(issueMatchesFilters(mention, view));
assert(!issueMatchesFilters(longer, view));
assert(issueMatchesFilters(inComment, view));

const involved = { author: "bob", assignees: [], body: "", events: [{ type: "vote", authorName: "sam" }] };
assert(issueMatchesFilters(involved, { involves: ISSUE_FILTER_ME }));
assert(!issueMatchesFilters({ author: "bob", assignees: [], body: "", events: [] }, { involves: ISSUE_FILTER_ME }));
""")


def test_issue_label_tints_are_stable_and_never_purple():
    _run_issue_filter_javascript(r"""
assert.equal(issueLabelHue("infra"), issueLabelHue("INFRA "));
const names = Array.from({ length: 2000 }, (_, index) => `label-${index}`);
for (const name of names) {
  const hue = issueLabelHue(name);
  assert(hue >= 0 && hue < 360, name);
  assert(!(hue >= 235 && hue < 320), `${name} -> ${hue}`);
}
""")


def test_assignee_picker_suggests_people_before_anyone_types():
    # Issue #688: assigning used to mean typing a username blind. The picker
    # offers you, this issue's participants, the owner, then the people most
    # active across the loaded issues, each with the reason it is suggested.
    _run_issue_filter_javascript(r"""
state.session = { nodeName: "viewer" };
state.repoRecordDetail = {
  repo: { owner: "acme", name: "widgets" },
  parsed: {
    values: { authorName: "alice" },
    issueEvents: [{ type: "comment", authorName: "bob" }, { type: "vote", authorName: "alice" }],
  },
};
state.issuesView.items = [
  { author: "carol", assignees: ["dave"], labels: ["bug"], milestone: "v1", events: [] },
  { author: "carol", assignees: [], labels: ["bug", "ui"], milestone: "v1", events: [{ authorName: "erin" }] },
  { author: "dave", assignees: [], labels: [], milestone: "", events: [] },
];
const people = issuePickerCandidates("assignees");
assert.deepEqual(
  people.map((entry) => [entry.value, entry.reason]),
  [["viewer", "you"], ["alice", "participant"], ["bob", "participant"], ["acme", "owner"],
   ["carol", "active"], ["dave", "active"], ["erin", "active"]],
);
assert.equal(people.find((entry) => entry.value === "dave").note, "Active on 2 issues");

assert.deepEqual(issuePickerCandidates("labels").map((entry) => entry.value), ["bug", "ui"]);
assert.deepEqual(issuePickerCandidates("milestone").map((entry) => entry.value), ["v1"]);
""")
