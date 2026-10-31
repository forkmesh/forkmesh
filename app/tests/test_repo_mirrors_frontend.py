#!/usr/bin/env python3
"""Static website repo Mirrors tab contract tests."""

from pathlib import Path


from _dashboard_shell import assembled_dashboard
from _dashboard_bundle import assembled_dashboard_js

PUBLIC = Path(__file__).resolve().parents[1] / "public"
WWW_PUBLIC = PUBLIC.parents[1] / "www" / "public"
# The dashboard shell is split into HTML partials and prebuilt before deploy;
# assert against the assembled document a browser actually receives.
DASHBOARD = assembled_dashboard()
DASHBOARD_JS = assembled_dashboard_js()
STYLES = (WWW_PUBLIC / "styles.css").read_text(encoding="utf-8")


def test_repo_tab_is_named_mirrors_and_has_table_shell():
    assert 'id="tab-mirrors"' in DASHBOARD
    assert ">Mirrors" in DASHBOARD
    assert 'id="repo-mirrors"' in DASHBOARD
    assert 'id="mirror-summary"' in DASHBOARD
    assert 'id="mirror-list"' in DASHBOARD
    for heading in ("Node", "Status", "Last seen", "Hosted since", "Sync age", "Clone available"):
        assert heading in DASHBOARD


def test_mirrors_css_has_responsive_table_hooks():
    assert ".mirror-summary-grid" in STYLES
    assert ".mirror-table-wrap" in STYLES
    assert ".mirror-health-table" in STYLES
    assert "@media (max-width: 700px)" in STYLES


def test_live_mirror_tab_uses_compact_borderless_table():
    rows = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderMirrorTabRow(")
        : DASHBOARD_JS.index("function renderMirrorRow(")
    ]
    assert "data-mirror-row" in rows
    assert '<tr data-mirror-row' in rows
    # Every value stays on one line with a per-cell ceiling. The ceiling is a
    # cap, not a pinned column width: a full-width table has to spread its slack
    # across the columns that truncate, not hand all of it to one of them.
    assert 'style="max-width:16rem" class="truncate whitespace-nowrap px-1 py-1' in rows
    assert "<article" not in rows
    assert "border" not in rows

    headers = DASHBOARD_JS[
        DASHBOARD_JS.index("function mirrorTableHeader(")
        : DASHBOARD_JS.index("function renderMirrorTabRow(")
    ]
    assert '<th scope="col"' in headers
    assert 'class="inline-block h-3 w-3"' in headers
    assert '<span class="sr-only">${escapeHtml(label)}</span>' in headers
    # Each column names itself next to its icon; a long name gets a short
    # caption so the row still fits, with the full name kept in the tooltip.
    assert "const caption = short || label;" in headers
    # Value columns are capped, not pinned: w-px froze each one at its minimum
    # width, which sent every pixel of a full-width table's slack into the one
    # column that had no pin.
    assert '<td style="max-width:18rem" class="truncate' in headers
    assert 'w-px' not in headers[
        headers.index("function mirrorTableCell(")
        : headers.index("function canDeleteMirrorNodes(")
    ]
    assert '<span class="inline-flex items-center gap-1 leading-none">' in headers
    assert "${escapeHtml(caption)}" in headers
    assert '["refresh-cw", "Last synced", false, "Synced"]' in headers
    assert '["git-pull-request", "Pull requests", false, "Pulls"]' in headers
    for heading in (
        "Node",
        "Status",
        "Revision",
        "Last synced",
        "Endpoint",
        "Replica database",
        "Cache memory",
        "Cache hit rate",
        "Backend disk reads",
        "Disk reads avoided",
        "Cache evictions",
        "Issues",
        "Commits",
        "Clones served",
        "Website requests",
    ):
        assert f'"{heading}"' in headers

    mirror_list = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoMirrorLists(")
        : DASHBOARD_JS.index("const REPO_MIRROR_MIN_REFRESH_MS")
    ]
    assert "data-mirror-table-wrap" in mirror_list
    assert "data-mirror-table" in mirror_list
    assert "border-separate border-spacing-0 whitespace-nowrap" in mirror_list
    assert "mirrorTableHead()" in mirror_list
    assert "data-mirror-card" not in mirror_list


def test_live_mirror_table_keeps_dense_values_in_tooltips():
    rows = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderMirrorTabRow(")
        : DASHBOARD_JS.index("function renderMirrorRow(")
    ]
    assert 'const capabilityTitle = operations.length' in rows
    assert 'Capabilities: ${operations.join(", ")}' in rows
    assert "formatCount(operations.length)" in rows
    assert "Source of truth is at" in rows


def test_live_mirror_table_hydrates_cache_diagnostics_directly_from_nodes():
    helpers = DASHBOARD_JS[
        DASHBOARD_JS.index("const MIRROR_HEALTH_CACHE_MS")
        : DASHBOARD_JS.index("// Every node is a single compact table row")
    ]
    loader = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoMirrors(")
        : DASHBOARD_JS.index("function repoMirrorsTabVisible(")
    ]

    assert "5 * 60 * 1000" in helpers
    assert 'url.pathname = "/health";' in helpers
    assert r"/^mirror[0-9]+\.forkmesh\.com$/i" in helpers
    assert 'credentials: "omit"' in helpers
    assert 'cache: "no-store"' in helpers
    assert 'payload.transport !== "direct-https"' in helpers
    for field in (
        "repositoryResponses",
        "dashboardAssets",
        "repositoryCatalog",
        "portableSessions",
        "dashboardReplica",
        "diskReadsAvoided",
        "backendReads",
        "evictions",
    ):
        assert field in helpers
    assert "void hydrateRepoMirrorDiagnostics(mirrors, key);" in loader


def test_mirror_table_can_verify_the_registered_direct_git_url():
    helper = DASHBOARD_JS[
        DASHBOARD_JS.index("function mirrorDirectCheckCell(")
        : DASHBOARD_JS.index("function renderMirrorTabRow(")
    ]
    action = DASHBOARD_JS[
        DASHBOARD_JS.index("async function checkMirrorDirectUrl(")
        : DASHBOARD_JS.index("async function loadRepoMirrors(")
    ]

    assert '"Direct URL check"' in DASHBOARD_JS
    assert "data-mirror-integrity-check" in helper
    assert "directVerified" in helper
    assert "directRefsSha256" in helper
    assert "Signed refs SHA-256" in helper
    assert "tunnel fallback" in helper
    assert "/reachability`" in action
    assert '{ method: "POST", cache: "no-store", headers }' in action
    assert "Object.assign(mirror, data)" in action


def test_mirror_page_shell_has_no_outer_margin_or_border():
    panel = DASHBOARD_JS[
        DASHBOARD_JS.index('<section data-dashboard-repo-tab-panel="mirrors"')
        : DASHBOARD_JS.index('<section data-dashboard-repo-tab-panel="agents"')
    ]
    assert 'class="hidden"><div data-mirror-request' in panel
    assert "mt-4" not in panel
    assert "border" not in panel
    assert "rounded-lg" not in panel


def test_mirrors_action_badge_uses_the_live_membership_list():
    loader = DASHBOARD_JS[
        DASHBOARD_JS.index("async function loadRepoMirrors(")
        : DASHBOARD_JS.index("function repoMirrorsTabVisible(")
    ]

    # The endpoint's summary can lag the returned node records. The rail badge
    # must agree with the list rendered in the Mirrors panel.
    assert "const mirrorCount = mirrors.length;" in loader
    assert "data.summary?.mirrors" not in loader
    assert "updateRepoLiveCounts(repo, { mirrors: mirrorCount }, { mirrorList: true });" in loader
    assert 'setRepoTabCount("mirrors", mirrorCount);' in loader

    counts = DASHBOARD_JS[
        DASHBOARD_JS.index("function updateRepoLiveCounts(")
        : DASHBOARD_JS.index("function repoCount(repo, keys)")
    ]
    assert "repoMirrorLoadedKey === repoKey(repo).toLowerCase()" in counts
    assert 'if (key === "mirrors" && mirrorsListed) return;' in counts


def test_mirrors_tab_drops_the_reading_column_width_cap():
    tabs = DASHBOARD_JS[
        DASHBOARD_JS.index("function setRepoTab(tab)")
        : DASHBOARD_JS.index("function activateRepoTab(tab)")
    ]
    # Twenty-odd node-health columns do not fit an 80rem reading column, so the
    # Mirrors tab -- and only that tab -- uses the whole viewport.
    assert 'const fullBleed = tab === "mirrors";' in tabs
    assert 'contentGrid?.classList.toggle("max-w-7xl", !fullBleed);' in tabs
    assert 'contentGrid?.classList.toggle("max-w-none", fullBleed);' in tabs
    assert 'max-w-7xl' in DASHBOARD_JS[
        DASHBOARD_JS.index("<div data-repo-content-grid")
        : DASHBOARD_JS.index("<div data-repo-content-grid") + 200
    ]


def test_mirrors_tab_can_hide_offline_nodes():
    toolbar = DASHBOARD_JS[
        DASHBOARD_JS.index("function repoMirrorsOnlineOnly(")
        : DASHBOARD_JS.index("function repaintRepoMirrorLists(")
    ]
    # Off by default: the tab still opens on the full published roster.
    assert "return state.repoMirrorsOnlineOnly === true;" in toolbar
    assert "data-repo-mirror-online-only" in toolbar
    assert 'aria-pressed="${onlineOnly ? "true" : "false"}"' in toolbar
    assert "data-mirror-toolbar" in toolbar
    # The count next to the toggle always reports the whole roster, so hiding
    # the offline nodes never hides the fact that they exist.
    assert "const offline = mirrors.length - online;" in toolbar
    assert 'const visible = repoMirrorsOnlineOnly()' in toolbar
    assert 'ordered.filter((mirror) => mirror.status === "online")' in toolbar
    assert "No mirrors are online right now." in toolbar

    assert "state.repoMirrorsOnlineOnly = !repoMirrorsOnlineOnly();" in DASHBOARD_JS
    assert 'event.target.closest("[data-repo-mirror-online-only]")' in DASHBOARD_JS
    assert "toggleRepoMirrorsOnlineOnly();" in DASHBOARD_JS


def test_repo_about_footer_lists_direct_mirror_nodes():
    detail = (PUBLIC / "dashboard/js/08-repo-detail-network.js").read_text()
    direct = detail.index("<h4 class=\"text-xs font-semibold text-foreground\">Direct access</h4>")
    close_aside = detail.index("</aside>", direct)
    footer = detail[direct:close_aside]
    assert "data-repo-live-mirror-list" in footer
    assert "If the controller is unavailable" in footer
    renderer = DASHBOARD_JS[
        DASHBOARD_JS.index("function renderRepoLiveMirrorList("):
        DASHBOARD_JS.index("function repoMirrorsOnlineOnly(")
    ]
    assert "renderMirrorRow" in renderer
    assert "mirror.status === \"online\"" in renderer


def test_mirror_rows_offer_node_deletion_only_to_administrators():
    helpers = DASHBOARD_JS[
        DASHBOARD_JS.index("function canDeleteMirrorNodes(")
        : DASHBOARD_JS.index("function mirrorTableHead(")
    ]
    assert "return state.session?.isAdmin === true;" in helpers
    # Both the header and the cell are gated, or the column and its rows drift.
    assert helpers.count('if (!canDeleteMirrorNodes()) return "";') == 2
    assert "data-repo-mirror-delete" in helpers
    # Deletion is keyed on the publishing account, with the machine label and
    # node id alongside so a half-registered node still resolves.
    assert 'data-mirror-node="${escapeHtml(account)}"' in helpers
    assert "data-mirror-machine=" in helpers
    assert "data-mirror-id=" in helpers

    delete = DASHBOARD_JS[
        DASHBOARD_JS.index("async function deleteMirrorNode(button)")
        : DASHBOARD_JS.index("function renderRepoRelease(")
    ]
    # Same durable deletion the World admin panel performs: the relay re-checks
    # is_admin, refuses the protected nodes, and requires the typed
    # confirmation. Nothing here is the authorization.
    assert '"/api/world/admin/nodes/delete"' in delete
    assert "const required = `DELETE ${nodeName}`;" in delete
    assert "window.prompt(" in delete
    assert "confirmation: required," in delete
    assert 'authorization: `Bearer ${state.session?.sessionToken || ""}`' in delete
    assert 'code === "not_admin"' in delete
    assert 'code === "protected_node"' in delete
    assert "loadRepoMirrors(state.selectedRepo, { background: true, force: true })" in delete
