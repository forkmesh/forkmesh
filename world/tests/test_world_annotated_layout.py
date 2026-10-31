from pathlib import Path
from world_scene_source import world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE = world_scene_source()
DATA = (ROOT.parent / "world" / "public" / "world" / "world-data.js").read_text()
APP = (ROOT.parent / "world" / "public" / "world" / "world.js").read_text()
ENTRY = (ROOT / "src" / "entry.py").read_text()


def test_town_has_four_solid_cardinal_paved_routes():
    for route in (
        '"east-repositories",\n      [junctionHalfWidth, 0],\n'
        '      [REPOSITORY_CONNECTION_MIN_X, 0]',
        '"forkmesh-town-path-north-office",\n      [0, -OFFICE_BRIDGE_WIDTH / 2],\n'
        '      [OFFICE_ISLAND_CENTER[0], officeEntranceZ]',
        '"west-billboards",\n      [-junctionHalfWidth, 0],\n'
        '      [LEADERBOARD_CONNECTION_MAX_X, 0]',
        '"south-members",\n      [0, junctionHalfWidth],\n'
        '      [0, MEMBER_PATH_END_Z]',
    ):
        assert route in SCENE
    assert '"forkmesh-town-path-junction"' in SCENE
    assert '"forkmesh-town-stone-plaza"' not in SCENE
    assert "function addRoundedCausewayEnds(" not in SCENE
    assert '"forkmesh-continuous-city-land"' in SCENE
    assert "concreteBrickMaterial(THREE)" in SCENE
    for district in ("repository", "leaderboard", "office", "member"):
        assert f"{district}" in SCENE


def test_all_repositories_share_one_east_island_ring():
    assert "position: [130, 0, 0]" in DATA
    assert "const coreRecords = [];" in SCENE
    assert "const hostedRecords = records;" in SCENE
    assert "const islandRecord = true;" in SCENE
    assert "repository-always-expanded-profile:" in SCENE
    assert "repository-expanded-ring-" in SCENE
    assert "label.visible = true;" in SCENE


def test_repository_rings_expand_only_for_a_visitor_inside_the_circle():
    """Portals are bare discs from the town; the rings are a room-scale detail."""
    # Every profile is built folded, including the ones a catalog refresh
    # rebuilds while somebody is already standing in the ring.
    assert "applyRepositoryRingExpansion(expandedProfile);" in SCENE
    assert "node.userData.repositoryExpandedProfile = expandedProfile;" in SCENE
    assert "function applyRepositoryRingExpansion(profile)" in SCENE
    assert "profile.visible = repositoryRingExpansion > 0.002;" in SCENE
    assert "const REPOSITORY_RING_COLLAPSED_SCALE = 1.08 / 1.58;" in SCENE
    assert "function updateRepositoryRingExpansion(delta)" in SCENE
    assert "distance <= REPOSITORY_RING_EXPAND_RADIUS" in SCENE
    assert "distance > REPOSITORY_RING_COLLAPSE_RADIUS" in SCENE


def test_repository_ring_has_no_dome_or_shell():
    for gone in (
        "createRepositoryEnclosure",
        "repository-enclosure",
        "repositoryEnclosureState",
        "REPOSITORY_ENCLOSURE_",
        "constrainRepositoryEnclosureWall",
        "repositoryEnclosureKeptRoots",
        'mode === "repositories"',
    ):
        assert gone not in SCENE


def test_billboards_stand_around_one_ring_grouped_by_section():
    assert "function placeBillboardOnIsland(object, options = {})" in SCENE
    assert "function relayout() {" in SCENE
    assert "ring.records.push(record);" in SCENE
    assert "outX * radius," in SCENE
    assert "outZ * radius," in SCENE
    assert "const BILLBOARD_RING = Object.freeze({" in SCENE
    assert "BILLBOARD_CIRCLE_SPECS" not in SCENE
    assert 'sections: Object.freeze(["hub", "social", "leaderboards"]),' in SCENE
    assert 'group.name = "forkmesh-billboard-ring";' in SCENE
    # Each board names the section of the ring it stands in.
    for object_name, circle_id in (
        ("worldBulletin", "hub"),
        ("worldGeneralChatBoard", "hub"),
        ("worldQaBoard", "hub"),
        ("worldDiscordBoard", "social"),
        ("mastodonKiosk", "social"),
    ):
        placement = SCENE.split(f"placeBillboardOnIsland({object_name}", 1)
        assert len(placement) == 2, object_name
        assert f'circle: "{circle_id}"' in placement[1][:120], object_name
    # The blog remains host content; X and Reddit declare the same section in
    # their portable package modules and are placed through the shared host.
    blog = SCENE.split("placeBillboardOnIsland(blogBanner", 1)
    assert len(blog) == 2
    assert 'circle: "social"' in blog[1][:160]
    assert "record.billboard = placeBillboardOnIsland(" in SCENE


def test_members_share_continuous_land_with_path_under_dirt_and_open_entrance():
    assert "const MEMBER_ISLAND_CENTER_Z = 130;" in SCENE
    assert "const MEMBER_PATH_END_Z = MEMBER_ISLAND_CENTER_Z;" in SCENE
    assert '"forkmesh-continuous-city-land"' in SCENE
    assert '"forkmesh-member-island"' not in SCENE
    assert '"forkmesh-member-island-connection"' not in SCENE
    assert '"forkmesh-member-promenade"' not in SCENE
    assert '"forkmesh-members-circle-path-sign"' not in SCENE
    assert '"🔥  MEMBERS CIRCLE"' not in SCENE
    assert "const CAMPFIRE_GROUND_Y = WORLD_PATH_SURFACE_Y + 0.01;" in SCENE
    assert "campfireGround.position.y = CAMPFIRE_GROUND_Y;" in SCENE
    assert (
        "startHereBoard.position.set(0, 0, MEMBER_ISLAND_CENTER_Z + 38);"
        in SCENE
    )
    assert "startHereBoard.rotation.y = Math.PI;" in SCENE
    assert "function worldWalkSurfaceContains(x, z" in SCENE
    assert "position: [0, 0, 130]" in DATA
    assert 'landmarkObjects.set("campfire", campfire);' in SCENE
    assert "const POSITION_RADIUS = 16_000;" in APP
    assert "or abs(x) > 1900 or abs(y) > 100 or abs(z) > 1900" in ENTRY


def test_live_nodes_remain_in_the_central_service_yard():
    assert (
        "const SERVER_CABINET_YARD_ORIGIN = Object.freeze([18, 0, 0]);"
        in SCENE
    )
    assert "west-billboards:network-node" not in SCENE
    assert "south-members:network-node" not in SCENE
