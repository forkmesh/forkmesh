#!/usr/bin/env python3
"""Static contracts for the public Network page."""

import re
from pathlib import Path


PUBLIC = Path(__file__).resolve().parents[1] / "public"
WWW_PUBLIC = PUBLIC.parents[1] / "www" / "public"
NETWORK_PAGES = (
    PUBLIC / "network.html",
)
STYLES = (WWW_PUBLIC / "styles.css").read_text(encoding="utf-8")
STATIC_PAGE = (WWW_PUBLIC / "static-page.js").read_text(encoding="utf-8")


def _read(page: Path) -> str:
    return page.read_text(encoding="utf-8")


def test_network_page_uses_command_center_surface():
    for page in NETWORK_PAGES:
        html = _read(page)

        assert "network-command-page" in html
        assert 'class="network-command-hero"' in html
        assert 'class="network-console"' in html
        assert 'class="network-console-metrics"' in html
        assert 'id="network-clients"' not in html
        assert "Online clients" not in html
        assert 'data-network-hosts' in html
        assert 'data-network-hosts-description' in html
        assert 'data-network-repos' in html
        assert 'data-network-repos-description' in html
        assert "Desktop mirrors reachable by this relay right now." in html
        assert "Public repositories indexed for discovery." in html
        assert "Storage boundary" in html
        assert 'class="network-bento-section"' not in html
        assert 'class="stats-grid network-stats"' not in html


def test_network_page_groups_operational_data_into_single_grid():
    for page in NETWORK_PAGES:
        html = _read(page)

        assert 'class="network-ops-grid"' in html
        assert 'class="network-ops-card network-ops-card-activity"' in html
        assert 'class="network-ops-card network-ops-card-wallets"' in html
        assert 'class="network-ops-card network-ops-card-leaderboards"' in html
        assert 'id="online-graph" class="online-graph network-activity-graph"' in html
        assert 'id="network-wallets" class="wallet-table network-wallet-table"' in html
        assert 'id="leaderboards" class="leaderboards network-leaderboards"' in html
        assert "User wallets" in html
        assert "Node wallets" not in html
        assert 'class="panel doc-panel"' not in html
        assert 'class="docs-grid"' not in html


def test_network_page_removes_redundant_architecture_and_longest_hosted_sections():
    for page in NETWORK_PAGES:
        html = _read(page)

        assert 'class="network-architecture-panel"' not in html
        assert "Cloudflare coordinates the route" not in html
        assert "Desktop hosts keep the code" not in html
        assert "Longest hosted" not in html
        assert 'arr("hosted")' not in html


def test_network_command_center_styles_exist():
    for marker in (
        ".network-command-page",
        ".network-command-hero",
        ".network-console",
        ".network-console-metrics",
        ".network-route-map",
        ".network-ops-grid",
        ".network-ops-card",
        ".network-console-metrics small",
        ".network-console-metrics article:last-child",
        "grid-template-columns: minmax(0, 1.05fr) minmax(360px, 0.95fr);",
        ".network-route-step.is-relay > span",
    ):
        assert marker in STYLES


def test_network_command_center_surfaces_use_theme_tokens():
    network_block = STYLES[
        STYLES.index("/* ===== Network command center ===== */"):
        STYLES.index("/* ===== Doc content ===== */")
    ]

    assert "#111212" not in network_block
    assert "rgba(9, 9, 9" not in network_block
    assert "var(--kit-surface)" in network_block
    assert "var(--kit-line)" in network_block
    assert "var(--kit-card-shadow)" in network_block


def test_network_page_carries_no_decorative_green():
    network_block = STYLES[
        STYLES.index("/* ===== Network command center ===== */"):
        STYLES.index("/* ===== Doc content ===== */")
    ]
    html = _read(NETWORK_PAGES[0])

    assert "network-console-led" not in html
    assert 'class="eyebrow"' not in html
    for gone in (
        ".network-console-led",
        ".network-console::before",
        "var(--primary)",
        "var(--purple)",
        "rgba(74, 222, 128",
        "rgba(163, 113, 247",
    ):
        assert gone not in network_block, gone
    relay = re.search(
        r"(?ms)^\.network-route-step\.is-relay > span\s*\{(?P<body>[^}]*)\}",
        STYLES,
    )
    assert relay, "styles.css missing the relay step badge rule"
    assert "background: var(--kit-tile-fill);" in relay.group("body")


def test_network_page_opens_on_the_page_kit():
    html = _read(NETWORK_PAGES[0])

    assert '<body class="kit-page">' in html
    assert 'class="network-hero-copy kit-hero"' in html
    assert 'class="kit-title" id="network-title"' in html
    assert "How a clone travels" in html
    assert "<strong>ForkMesh relay</strong>" in html
    assert "const BOARD_ROWS = 5;" in html
    assert "rows.slice(0, BOARD_ROWS)" in html
    assert ".wallet-row[hidden]" in html


def test_network_wallets_lead_with_published_wallets_and_fold_the_rest():
    renderer = STATIC_PAGE[
        STATIC_PAGE.index("function eligibilityLabel(node)"):
        STATIC_PAGE.index("applyTheme();")
    ]

    assert 'if (node.eligibilityReason === "no_mirrors") return "Not mirroring";' in renderer
    assert "const PAYOUT_ROWS_SHOWN = 6;" in renderer
    assert "payoutRank(left) - payoutRank(right)" in renderer
    assert "if (index >= PAYOUT_ROWS_SHOWN) {" in renderer
    assert 'more.className = "wallet-more";' in renderer


def test_network_stats_bindings_update_all_metric_instances():
    for marker in (
        'setText("#network-clients, [data-network-clients]"',
        'setText("#network-hosts, [data-network-hosts]"',
        'setText("#network-repos, [data-network-repos]"',
        "[data-network-clients]",
        "[data-network-hosts]",
        "[data-network-repos]",
    ):
        assert marker in STATIC_PAGE


def test_network_zero_counts_are_explained_as_normal_empty_states():
    for marker in (
        '"[data-network-hosts-description]"',
        "No managed mirror hosts are reachable right now.",
        '"[data-network-repos-description]"',
        "No public repositories have been cataloged yet.",
    ):
        assert marker in STATIC_PAGE


def test_network_wallet_renderer_uses_user_labels():
    payout_renderer = STATIC_PAGE[
        STATIC_PAGE.index("function renderPayoutNodes("):
        STATIC_PAGE.index("applyTheme();")
    ]

    assert 'const headings = ["User", "Wallet", "Balance", "Payouts"]' in payout_renderer
    assert "No user payout wallets are listed yet." in payout_renderer
    assert 'const headings = ["Node"' not in payout_renderer


def test_network_wallets_resolve_when_stats_api_is_unavailable():
    poll_stats = STATIC_PAGE[
        STATIC_PAGE.index("async function pollStats(options)"):
        STATIC_PAGE.index("function primeCachedStats(")
    ]
    catch_block = poll_stats[poll_stats.index("} catch (error) {"):]

    assert "renderPayoutNodes([]);" in catch_block
