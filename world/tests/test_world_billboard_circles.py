"""The island's boards stand on three circles and fetch on approach.

One 43-unit perimeter used to carry the public rankings, the social feeds, and
the operational boards together. They are split into a social circle, a
leaderboard circle, and a hub circle. The two circles with remote feeds do not
request anything until a visitor walks onto the circle that carries them.
"""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")


def test_three_circles_are_declared_with_their_own_ground_and_rim():
    assert "const BILLBOARD_CIRCLE_SPECS = Object.freeze({" in SCENE
    assert "const LEADERBOARD_DISTRICT_GROUND_RADIUS = 56;" in SCENE
    specs = SCENE.split("const BILLBOARD_CIRCLE_SPECS = Object.freeze({", 1)[1]
    specs = specs.split("// Width of the accent band", 1)[0]
    for circle_id in ("hub", "social", "leaderboards"):
        assert f"{circle_id}: Object.freeze({{" in specs
    # Each circle is a distinct disc: its own centre, tint, and accent rim.
    assert specs.count("color:") == 3
    assert specs.count("accent:") == 3
    assert specs.count("minRing:") == 3
    assert "`forkmesh-${spec.id}-circle-ground`" in SCENE
    assert "`forkmesh-${spec.id}-circle-rim`" in SCENE
    assert "circle.rim.geometry = new THREE.RingGeometry(" in SCENE


def test_boards_are_given_the_arc_they_actually_need():
    layout = SCENE.split("const relayoutBillboardCircle = (circle) =>", 1)[1]
    layout = layout.split("function placeBillboardOnIsland", 1)[0]
    # Ring size follows the boards on it, so a wide assembly grows the circle
    # rather than overlapping the board beside it.
    assert "span += record.width + BILLBOARD_CIRCLE_GAP;" in layout
    assert "objects.length ? span / (Math.PI * 2) : 0," in layout
    assert "widest * 0.75," in layout
    assert "const slice = record.width + BILLBOARD_CIRCLE_GAP;" in layout
    assert "object.position.y = BILLBOARD_CIRCLE_SURFACE_Y - record.baseY;" in layout


def test_every_fetching_board_carries_a_spinner_keyed_by_its_feed():
    place = SCENE.split("function placeBillboardOnIsland(object, options = {})", 1)[1]
    place = place.split("animated.push((time)", 1)[0]
    assert "if (record.feed) {" in place
    assert "spinner.position.set(0, record.topY + 1.1, 0.4);" in place
    assert "spinner.visible = false;" in place
    assert "billboardFeedRecords.set(record.feed, shared);" in place
    assert "function setBillboardRefreshing(feed, loading)" in SCENE
    assert "record.spinner.visible = loading === true;" in SCENE
    assert "setBillboardRefreshing," in SCENE
    # A board painted from data the page already holds gets no ring at all.
    assert 'placeBillboardOnIsland(worldBulletin, { circle: "hub" });' in SCENE
    assert 'placeBillboardOnIsland(worldGeneralChatBoard, { circle: "hub" });' in SCENE
    assert 'placeBillboardOnIsland(worldDiscordBoard, { circle: "social" });' in SCENE


def test_each_fetching_circle_reports_its_own_approach_and_departure():
    proximity = SCENE.split("const proximityBoards = [", 1)[1].split(
        "].map((board) => ({ ...board, near: false", 1)[0]
    for circle_id, near, away in (
        ("leaderboards", "onLeaderboardCircleNearby", "onLeaderboardCircleAway"),
        ("social", "onSocialCircleNearby", "onSocialCircleAway"),
    ):
        assert f'object: billboardCircle("{circle_id}").group,' in proximity
        assert f'enter: billboardCircle("{circle_id}").radius + 6,' in proximity
        assert f'exit: billboardCircle("{circle_id}").radius + 14,' in proximity
        assert f"onEnter: (refetch) => {near}({{ refetch }})," in proximity
        assert f"onExit: () => {away}()," in proximity
    assert 'object: billboardCircle("hub").group,' not in proximity
    assert "onWorldBoardsCircleNearby" not in SCENE
    # The exit radius stays wider than the entry radius so pacing the boundary
    # cannot thrash the fetch.
    assert "onLeaderboardWallNearby" not in SCENE


def test_the_shell_starts_no_billboard_request_before_an_approach():
    # Boot pushes cached snapshots onto the rebuilt scene and starts the
    # one-second stand clocks, but reads nothing.
    bootstrap = WORLD.split("  async bootstrap() {", 1)[1].split(
        "\n  setLoadingProgress(percent, label) {", 1
    )[0]
    boot = bootstrap.split("this.leaderboardCircleNear = false;", 1)[1].split(
        "this.syncRepositoryScene();", 1)[0]
    assert "this.syncMastodonKiosk();" in boot
    assert "this.syncSocialBanners();" in boot
    assert "loadMastodonBoard" not in boot
    assert "loadSocialBanners" not in boot
    assert "loadReferralLeaderboard" not in boot
    assert "if (!loading && remaining <= 0 && this.socialCircleNear) {" in WORLD
    assert "worldBoardsCircleNear" not in WORLD
    assert "refreshSystemStatusBoard" not in WORLD
    assert "void this.loadSocialBanners();\n  }\n\n  socialRefreshRemaining()" not in WORLD


def test_each_watch_owns_its_boards_requests_and_their_spinners():
    for method in (
        "startLeaderboardCircleWatch({ refetch = true } = {}) {",
        "stopLeaderboardCircleWatch() {",
        "startSocialCircleWatch({ refetch = true } = {}) {",
        "stopSocialCircleWatch() {",
    ):
        assert method in WORLD
    for call in (
        'this.world?.setBillboardRefreshing?.("leaderboards", true);',
        'this.world?.setBillboardRefreshing?.("leaderboards", false);',
        'this.world?.setBillboardRefreshing?.("mastodon", loading);',
        'this.world?.setBillboardRefreshing?.("social", loading);',
        'this.world?.setBillboardRefreshing?.("qa-board", true);',
        'this.world?.setBillboardRefreshing?.("qa-board", false);',
    ):
        assert call in WORLD
    # A concurrent approach must not stack /api/leaderboards reads.
    assert "if (this.leaderboardLoad) return this.leaderboardLoad;" in WORLD
