from pathlib import Path
import tomllib
from world_scene_source import world_scene_module, world_scene_source


ROOT = Path(__file__).resolve().parents[1]
PUBLIC = ROOT / "public"
WORLD_PUBLIC = ROOT.parent / "world" / "public" / "world"
ENTRY = (ROOT / "src" / "entry.py").read_text(encoding="utf-8")
RUNTIME = (ROOT / "src" / "app_runtime.py").read_text(encoding="utf-8")
SCENE = world_scene_source()
WORLD = (WORLD_PUBLIC / "world.js").read_text(encoding="utf-8")
PAGE = (PUBLIC / "leaderboards.html").read_text(encoding="utf-8")
CLIENT = (PUBLIC / "leaderboards.js").read_text(encoding="utf-8")
SCHEMA = (ROOT / "src" / "schema.py").read_text(encoding="utf-8")
STATIC_ROUTES = (ROOT / "src" / "static_routes.py").read_text(encoding="utf-8")
WRANGLER = tomllib.loads((ROOT / "wrangler.toml").read_text(encoding="utf-8"))
QT_ROOT = ROOT.parent / "desktop" / "src"


def _wallet_leaderboard_body():
    return ENTRY.split("async def wallet_leaderboard", 1)[1].split(
        "async def leaderboards_overview", 1)[0]


def _tip_recipient_leaderboard_body():
    return ENTRY.split("async def tip_recipient_leaderboard", 1)[1].split(
        "# --- Member SOL wallets", 1)[0]


def test_shared_public_endpoint_exposes_every_board():
    assert '"/api/leaderboards"' in ENTRY
    overview = ENTRY.split(
        "async def leaderboards_overview", 1
    )[1].split("async def prune_site_referrers", 1)[0]
    # One source at a time. Gathering the five put five Pyodide tasks in
    # flight inside one request, and a re-entered task wedges the isolate into
    # answering 1101 for every later request (see
    # tests/test_worker_task_concurrency.py). Each source keeps its own cache,
    # so the endpoint stays cheap despite the sequential reads.
    assert "asyncio.gather(" not in overview
    assert "result = await source()" in overview
    # A single failing board still must not take the hub down (adhoc #225).
    assert "degraded.append(name)" in overview
    for board_id in (
        "activity",
        "uptime",
        "node-storage",
        "repos",
        "mirrors",
        "hosted",
        "largest",
        "data-hosted",
        "contributors",
        "issue-reporters",
        "referrals",
        "referring-sites",
        "wallets",
        "tip-recipients",
        "funds-mainnodes",
        "funds-contributors",
        "funds-projects",
    ):
        assert f'"{board_id}"' in ENTRY


def test_contributor_board_resolves_human_account_names():
    network = ENTRY.split("async def network_leaderboards", 1)[1].split(
        "# --- Referral program", 1)[0]
    assert "AS display_name" in network
    assert "JOIN users u ON u.user_bi=n.user_bi" in network
    assert "JOIN users u ON u.user_bi=d.account_bi" in network
    assert "GROUP BY display_name" in network
    assert "Unlinked contributor" in network
    assert 'r.get("display_name", "")' in network


def test_leaderboard_cards_use_theme_aware_surface_tokens():
    assert "var(--surface" not in PAGE
    assert "var(--card)" in PAGE


def test_member_sol_wallet_board_ranks_published_addresses_only():
    body = _wallet_leaderboard_body()
    # Only public, active, non-private user profiles carrying a valid address.
    assert 'rec.get("profile_private")' in body
    assert '_account_kind(rec) != "user"' in body
    assert 'rec.get("status") != "active"' in body
    assert 'SOLANA_RE.match(wallet)' in body
    # Balance is the lowest-level public read, ranked highest first.
    assert "_display_balance_lamports(" in body
    assert 'board.sort(key=lambda entry: (-entry["lamports"], entry["name"]))' in body
    # Bounded on-chain work: an edge-cached response plus a rotating refresh
    # slice, with the un-refreshed remainder reported rather than hidden.
    assert "WALLET_LEADERBOARD_CACHE_KEY" in body
    assert "WALLET_BALANCE_REFRESH_PER_REBUILD" in body
    assert '"refreshed": refreshed' in body
    # The rotation reads through the shared hourly cache, so a warm board costs
    # no public-RPC subrequests at all, and a failed RPC keeps the last stored
    # reading instead of publishing an unchecked address as zero (both live in
    # _display_balance_lamports now).
    assert "force=refresh" in body
    assert "stored=stored" in body
    assert '"/api/leaderboards/wallets"' in RUNTIME
    assert "CREATE TABLE IF NOT EXISTS wallet_balances" in SCHEMA
    assert (
        "CREATE TABLE IF NOT EXISTS wallet_balances"
        in (ROOT / "migrations" / "0118_wallet_balances.sql").read_text(
            encoding="utf-8")
    )


def test_tip_recipient_board_folds_the_public_ledger_tip_receipts():
    body = _tip_recipient_leaderboard_body()
    # The ledger's own member-tip source table, not a separate tally that
    # could drift from what /api/ledger publishes for the same receipts.
    assert "FROM tip_notice AS t " in body
    assert "JOIN users AS u ON u.user_bi=t.to_bi " in body
    assert "SUM(t.lamports) AS lamports," in body
    assert "GROUP BY t.to_bi " in body
    assert "ORDER BY SUM(t.lamports) DESC,MAX(t.ts) DESC,t.to_bi LIMIT ?" in body
    # Contributor rewards write a tip notice only to notify the recipient. The
    # ledger publishes their canonical contributor-reward entry instead, so
    # they must not be counted as ordinary member tips here.
    assert "FROM contributor_reward_estimates " in body
    assert "tx_signature=t.signature AND status<>'cancelled'" in body
    # A devnet/testnet lamport is free to mint, so a faucet airdrop must not be
    # able to outrank every real tip.
    assert "WHERE t.network='mainnet'" in body
    # Same "counts as a public member" predicate as the accounts directory,
    # and ranking happens before it, so the scan reaches past the board depth.
    assert "if not _is_public_roster_member(rec):" in body
    assert "TIP_RECIPIENT_SCAN_LIMIT" in body
    assert "board[:TIP_RECIPIENT_LEADERBOARD_LIMIT]" in body
    assert "TIP_RECIPIENT_SCAN_LIMIT = 60" in ENTRY
    assert "TIP_RECIPIENT_LEADERBOARD_LIMIT = 10" in ENTRY
    # Edge-cached like every other hub source, so the hub's own five-minute
    # reload does not re-scan the table per visitor.
    assert "TIP_RECIPIENT_LEADERBOARD_CACHE_KEY" in body
    # The amount is the sender's claim; say so rather than presenting it as a
    # verified or custodial figure.
    assert "the sender's own reported figure" in body
    assert '"custody": "none-peer-to-peer-self-custodial"' in body
    assert "CREATE TABLE IF NOT EXISTS tip_notice" in SCHEMA


def test_tip_recipient_board_is_a_hub_source_with_profile_links():
    overview = ENTRY.split(
        "async def leaderboards_overview", 1
    )[1].split("async def prune_site_referrers", 1)[0]
    assert '("tips", lambda: tip_recipient_leaderboard(env)),' in overview
    assert (
        "network, referrals, sites, users, reporters, wallets, tips = payloads"
        in overview
    )
    # One directory pass builds the handles both member boards link, and the
    # cached source rows are copied rather than decorated in place.
    assert "mastodon_by_name = {}" in overview
    assert "link_profile(dict(row))" in overview
    assert 'board(\n            "tip-recipients", "Top tip recipients",' in overview


def test_tip_recipient_board_reaches_the_world_and_the_hub():
    assert 'id: "tip-recipients",' in SCENE
    assert 'title: "TOP TIP RECIPIENTS",' in SCENE
    assert 'board.id === "tip-recipients"' in CLIENT
    # Both surfaces already render SOL, so the board reuses that formatter
    # rather than adding a second spelling of the same amount.
    assert 'valueKind: "sol",' in SCENE
    assert 'case "sol": {' in CLIENT


def test_member_sol_wallet_board_reaches_the_world_and_the_hub():
    assert 'id: "wallets",' in SCENE
    assert 'title: "MEMBER SOL WALLETS",' in SCENE
    assert 'board.id === "wallets"' in CLIENT


def test_issue_reporter_board_links_member_profiles_and_mastodon():
    assert 'id: "issue-reporters",' in SCENE
    assert 'valueKind: "issues",' in SCENE
    assert 'case "issues":' in CLIENT
    assert 'profile.className = "leaderboard-profile-link"' in CLIENT
    assert 'mastodon.className = "leaderboard-detail leaderboard-mastodon-link"' in CLIENT
    assert 'mastodon.rel = "noopener noreferrer"' in CLIENT


def test_website_has_a_searchable_filterable_leaderboard_hub():
    assert "<title>Leaderboards · ForkMesh</title>" in PAGE
    assert '<script src="/leaderboards.js?v=${version}" defer></script>' in PAGE
    assert "data-leaderboards-grid" in PAGE
    assert "data-leaderboards-search" in PAGE
    for category in ("all", "network", "repositories", "community", "historical"):
        assert f'data-category="{category}"' in PAGE
    assert 'fetch("/api/leaderboards"' in CLIENT
    assert "formatValue(board, row)" in CLIENT
    assert "/leaderboards" not in WRANGLER["assets"]["run_worker_first"]
    assert WRANGLER["assets"]["html_handling"] == "drop-trailing-slash"
    assert '"/leaderboards.html"' in STATIC_ROUTES
    assert not (PUBLIC / "_redirects").exists()


def test_category_filter_state_is_only_applied_to_filter_buttons():
    assert 'querySelectorAll(".leaderboards-filter")' in CLIENT
    assert 'querySelectorAll("[data-category]")' not in CLIENT


def test_world_has_the_full_connected_leaderboard_district():
    assert 'fetchJSON("/api/leaderboards"' in WORLD
    assert "this.world.updateLeaderboards" in WORLD
    assert "const WORLD_LEADERBOARD_BOARD_STUBS" in SCENE
    assert 'continuousCityLand.name = "forkmesh-continuous-city-land"' in SCENE
    assert 'leaderboardConnection.name = "forkmesh-leaderboard-island-connection"' in SCENE
    assert 'leaderboardPromenade.name = "forkmesh-leaderboard-promenade"' in SCENE
    assert "new THREE.BoxGeometry(58, 4.2, 0.35)" not in SCENE
    assert '"forkmesh-leaderboard-ring-walk"' not in SCENE
    # One card per board, on the district's own leaderboard circle.
    physical = world_scene_module("districts/leaderboards.js")
    assert "makeActiveLeaderboardSign" in physical
    assert "makeReferralLeaderboardSign" in physical
    assert "makeSiteReferrerLeaderboardSign" in physical
    assert "makeLeaderboardStatSign(THREE, board)" in physical
    assert 'circle: "leaderboards",' in physical
    assert 'face.userData.interactive = "leaderboard-card"' in physical
    assert "face.userData.boardId = id" in physical
    # The 5x5 wall that replaced the cards, and its packed texture, are gone.
    assert "leaderboardSuperPanel" not in SCENE
    assert "leaderboardGridTexture" not in SCENE
    assert "5 × 5 LIVE GRID" not in SCENE
    assert "function updateLeaderboards(boards = [])" in SCENE
    assert "updateLeaderboards," in SCENE


def test_the_shared_stat_painter_leaves_the_three_richer_cards_alone():
    # /api/leaderboards ships activity/referrals/referring-sites too. Those
    # three faces are painted by their own textures (live world activity, the
    # viewer's own referral link, redacted referrer URLs), so letting the
    # shared statistic painter also claim them would make the two repaints
    # fight over one face on every snapshot.
    assert "const LEADERBOARD_CARDS_WITH_OWN_PAINTER = new Set([" in SCENE
    guard = SCENE.split("const LEADERBOARD_CARDS_WITH_OWN_PAINTER = new Set([", 1)[1]
    guard = guard.split("]);", 1)[0]
    for board_id in ("active-members", "referrals", "http-referrers"):
        assert f'"{board_id}",' in guard
    update = SCENE.split("function updateLeaderboards(boards = [])", 1)[1]
    update = update.split("function updateReferralLeaderboard", 1)[0]
    assert "if (LEADERBOARD_CARDS_WITH_OWN_PAINTER.has(id)) return;" in update
    assert "leaderboardStatTexture(THREE, board)" in update


def test_a_leaderboard_card_only_fetches_when_a_visitor_steps_onto_its_pad():
    # Nothing on the ring is read at boot; stepping onto a card's refresh pad
    # is the only thing that starts a fetch, and only that card spins.
    assert "void this.loadReferralLeaderboard();\n      void this.loadLobbyLinkBoard();" not in WORLD
    assert "startBillboardWatch(feed, { refetch = true } = {}) {" in WORLD
    assert 'feed === "leaderboards"' in WORLD
    assert "if (refetch) void this.loadReferralLeaderboard();" in WORLD
    assert "onBillboardNearby: ({ feed, refetch } = {}) =>" in WORLD
    assert 'this.world?.setBillboardRefreshing?.("leaderboards", true);' in WORLD
    assert 'this.world?.setBillboardRefreshing?.("leaderboards", false);' in WORLD
    assert "onBillboardNearby({ feed: record.feed, refetch });" in SCENE
    assert "function setBillboardRefreshing(feed, loading)" in SCENE
    assert "setBillboardRefreshing," in SCENE


def test_qt_has_no_leaderboard_navigation_or_network_fetch():
    sources = "\n".join(
        path.read_text(encoding="utf-8", errors="replace")
        for path in QT_ROOT.glob("*")
        if path.suffix in {".cpp", ".h"}
    )
    assert "m_leaderboardNavButton" not in sources
    assert "buildLeaderboardsSection" not in sources
    assert 'QStringLiteral("Leaderboards")' not in sources
    assert "/api/network/leaderboards" not in sources


def test_hub_has_a_my_stats_button_for_the_signed_in_reader():
    assert 'data-leaderboards-mine aria-pressed="false">My stats</button>' in PAGE
    assert "data-leaderboards-mine-panel hidden" in PAGE
    assert 'localStorage.getItem("forkmesh.session")' in CLIENT
    assert 'session.kind === "node"' in CLIENT
    assert 'name.startsWith(`${account}/`)' in CLIENT
    assert 'new Set(["referring-sites"])' in CLIENT
    assert 'item.classList.add("is-mine")' in CLIENT
    assert 'encodeURIComponent("/leaderboards?mine=1")' in CLIENT
    assert 'get("mine") === "1"' in CLIENT
