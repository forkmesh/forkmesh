"""The island's boards stand on one ring and each one refreshes on its own.

The hub, social, and leaderboard boards used to stand on three separate
circles, each fetching everything on it as soon as a visitor walked onto it.
They now share one ring, grouped by section, and every board with a feed has a
small semicircle pad in front of it: a board's request starts only when a
visitor steps onto that board's pad, and only that board spins.
"""

from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
WORLD = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text(encoding="utf-8")
RING = (
    ROOT.parent / "world" / "public" / "world" / "scene" / "districts" / "billboard-circles.js"
).read_text(encoding="utf-8")


def _between(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


def test_one_ring_replaces_the_three_circles():
    assert "BILLBOARD_CIRCLE_SPECS" not in SCENE
    assert "billboardCircle(" not in SCENE
    spec = _between(SCENE, "const BILLBOARD_RING = Object.freeze({", "});")
    for key in (
        "minRadius:",
        "groundMargin:",
        "entryGap:",
        "gap:",
        "rimWidth:",
        "color:",
        "accent:",
        "muted:",
        "padRadius:",
        "padExit:",
        "cooldownMs:",
    ):
        assert key in spec, key
    assert 'sections: Object.freeze(["hub", "social", "leaderboards"]),' in spec
    assert 'group.name = "forkmesh-billboard-ring";' in RING
    assert RING.count("ctx.leaderboardInterior.add(group);") == 1


def test_the_ring_draws_with_three_materials_and_one_pad_draw_call():
    assert RING.count("new THREE.Mesh(") == 3
    assert RING.count("new THREE.InstancedMesh(") == 1
    assert 'districtGroundMaterial(THREE, "leaderboards")' in RING
    assert 'resources.uniqueMaterial(THREE, DECAL, "MeshBasicMaterial")' in RING
    assert "polygonOffset: true," in RING
    assert "ring.apron.scale.set(outer, outer, 1);" in RING
    assert "ring.rim.geometry = rimGeometry(radius);" in RING
    assert ".dispose();\n    ring.rim" not in RING
    assert "pads.setMatrixAt(" in RING
    assert "ring.pads.setColorAt(" in RING
    assert "pads.computeBoundingSphere();" in RING
    assert "new THREE.CircleGeometry" not in RING
    assert "new THREE.RingGeometry" not in RING


def test_boards_are_grouped_by_section_and_leave_an_entrance():
    layout = _between(RING, "function relayout() {", "function syncSpinners")
    assert "const records = ring.records.sort(ringOrder);" in layout
    assert "return left.rank - right.rank || left.order - right.order;" in RING
    assert "span += record.width + BILLBOARD_RING.gap;" in layout
    assert "(span + BILLBOARD_RING.entryGap) / TAU," in layout
    assert "widest * 0.75," in layout
    assert "const step = span ? (TAU - entry) / span : 0;" in layout
    assert "let cursor = entry / 2;" in layout
    assert "BILLBOARD_CIRCLE_SURFACE_Y - record.baseY," in layout
    assert "object.rotation.y = Math.atan2(-outX, -outZ);" in layout


def test_each_fetching_board_gets_a_semicircle_pad_in_front_of_it():
    layout = _between(RING, "function relayout() {", "function syncSpinners")
    assert "if (!record.feed) continue;" in layout
    assert "const reach = radius - record.frontZ - BILLBOARD_RING.padGap;" in layout
    assert "Math.PI,\n    Math.PI,\n  );" in RING
    probe = _between(RING, "function padUnderPlayer() {", "function updateBillboardPads")
    assert 'if (ctx.officeSceneMode !== "town" || !ring.padded.length) return null;' in probe
    assert "const reach = record === activePad ? slack : BILLBOARD_RING.padRadius;" in probe
    assert "dx * record.inX + dz * record.inZ >= -BILLBOARD_RING.padGap" in probe
    # A board painted from data the page already holds gets no pad and no ring.
    assert 'placeBillboardOnIsland(worldBulletin, { circle: "hub" });' in SCENE
    assert 'placeBillboardOnIsland(worldGeneralChatBoard, { circle: "hub" });' in SCENE
    assert 'placeBillboardOnIsland(worldDiscordBoard, { circle: "social" });' in SCENE


def test_each_pad_has_its_own_cooldown_and_its_own_spinner():
    enter = _between(RING, "function enterPad(record) {", "function leavePad")
    assert "const refetch = now - record.refreshedAt >= record.cooldownMs;" in enter
    assert "spinnerHolders.set(record.feed, record);" in enter
    assert "onBillboardNearby({ feed: record.feed, refetch });" in enter
    assert "onBillboardAway({ feed: record.feed });" in RING
    sync = _between(RING, "function syncSpinners(feed) {", "function placeBillboardOnIsland")
    assert "const show = loading && (!holder || holder === record);" in sync
    assert "for (const spinner of spinning) spinner.rotation.z = -time * 0.006;" in RING
    assert "cooldownMs: 5_000," in SCENE
    assert "updateBillboardPads();" in SCENE
    assert "updateBoardProximity" not in SCENE
    assert "proximityBoards" not in SCENE


def test_the_shell_starts_no_billboard_request_before_an_approach():
    bootstrap = WORLD.split("  async bootstrap() {", 1)[1].split(
        "\n  setLoadingProgress(percent, label) {", 1
    )[0]
    boot = bootstrap.split("this.billboardFeedsNear.clear();", 1)[1].split(
        "this.syncRepositoryScene();", 1)[0]
    assert "this.syncMastodonKiosk();" in boot
    assert "this.syncSocialBanners();" in boot
    assert "loadMastodonBoard" not in boot
    assert "loadSocialBanners" not in boot
    assert "loadReferralLeaderboard" not in boot
    assert 'remaining <= 0 && this.billboardFeedsNear.has("social")' in WORLD
    assert 'remaining <= 0 && this.billboardFeedsNear.has("mastodon")' in WORLD
    for gone in ("socialCircleNear", "leaderboardCircleNear", "CircleWatch"):
        assert gone not in WORLD


def test_each_feed_owns_its_requests_and_their_spinners():
    watch = _between(
        WORLD, "  startBillboardWatch(feed, { refetch = true } = {}) {", "  stopBillboardWatch(feed) {"
    )
    for feed in ("qa-board", "leaderboards", "mastodon", "social"):
        assert f'feed === "{feed}"' in watch
    for call in (
        'this.world?.setBillboardRefreshing?.("leaderboards", true);',
        'this.world?.setBillboardRefreshing?.("leaderboards", false);',
        'this.world?.setBillboardRefreshing?.("mastodon", loading);',
        'this.world?.setBillboardRefreshing?.("social", loading);',
        'this.world?.setBillboardRefreshing?.("qa-board", true);',
        'this.world?.setBillboardRefreshing?.("qa-board", false);',
    ):
        assert call in WORLD
    assert "if (this.leaderboardLoad) return this.leaderboardLoad;" in WORLD
