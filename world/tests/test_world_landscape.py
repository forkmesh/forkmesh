import json
import math
from pathlib import Path
import subprocess
from world_scene_source import world_scene_module, world_scene_source


ROOT = Path(__file__).resolve().parents[2] / "app"
SCENE_PATH = ROOT.parent / "world" / "public" / "world" / "world-scene.js"


def source():
    return world_scene_source()


def test_world_uses_a_mixed_city_and_woodland_surface():
    scene = source()
    for contract in (
        '"forkmesh-continuous-city-land"',
        'group.name = "forkmesh-town-mixed-landscape"',
        '"forkmesh-town-path-junction"',
        "`forkmesh-town-path-${id}`",
        "const treeLayout = deterministicTreeLayout();",
        "createTownTreeInstances(THREE, treeLayout, treeColors)",
        "function cityGrassTexture(THREE)",
        "new THREE.DataTexture(",
    ):
        assert contract in scene
    assert "city-park-grass-v1.webp" not in scene
    assert '"forkmesh-town-stone-plaza"' not in scene
    assert '"forkmesh-town-plaza-edge"' not in scene
    assert "forkmesh-town-path-edge-" not in scene
    assert "forkmesh-town-grass-patch" not in scene
    assert 'makeMaterial(THREE, "#174434"' not in scene


def test_static_town_trees_share_two_instanced_draw_batches():
    scene = source()
    tree_instances = scene.split(
        "function createTownTreeInstances(THREE, layout, colors)", 1
    )[1].split("function createLandscapePath", 1)[0]
    assert tree_instances.count("new THREE.InstancedMesh(") == 2
    assert 'trunk.name = "forkmesh-town-tree-trunks";' in tree_instances
    assert 'crown.name = "forkmesh-town-tree-crowns";' in tree_instances
    assert "trunk.setMatrixAt(index, transform.matrix);" in tree_instances
    assert "crown.setMatrixAt(index, transform.matrix);" in tree_instances
    assert "crown.setColorAt(" in tree_instances
    assert "new THREE.Mesh(" not in tree_instances
    assert "function createTree(" not in scene


def test_world_land_uses_one_flat_textured_surface():
    scene = source()
    for contract in (
        '"forkmesh-continuous-city-land"',
        "const CONTINUOUS_CITY_RADIUS = 365;",
        "const CONTINUOUS_CITY_CORNER_RADIUS = 70;",
        "CONTINUOUS_CITY_CORNER_RADIUS / CONTINUOUS_CITY_RADIUS,",
        "cityGrassMaterial(THREE)",
        "continuousCityLand.userData.ground = true;",
    ):
        assert contract in scene
    assert '"forkmesh-continuous-city-foundation"' not in scene
    assert "new THREE.CylinderGeometry(1, 1, 6.4, 128)" not in scene
    assert "function createTerrainFoundation(" not in scene
    assert "hosted-repository-terrain-foundation" not in scene
    assert "forkmesh-office-terrain-foundation" not in scene
    assert 'ring.name = `${name}-strata-${index + 1}`' not in scene
    assert 'root.name = `${name}-root-${index + 1}`' not in scene


def test_repository_and_office_paths_share_the_continuous_grass():
    scene = source()
    for contract in (
        '"hosted-repository-promenade"',
        '"south-members"',
        "[0, MEMBER_PATH_END_Z]",
        '"forkmesh-leaderboard-promenade"',
        '"forkmesh-town-path-north-office"',
        "[OFFICE_ISLAND_CENTER[0], officeEntranceZ]",
        'officeBridge.name = "forkmesh-office-bridge"',
        "concreteBrickMaterial(THREE)",
    ):
        assert contract in scene
    assert "addRoundedCausewayEnds" not in scene
    assert "rounded-pavement" not in scene
    assert "causeway-earth" not in scene
    assert "forkmesh-curved-district-land" not in scene
    assert '"forkmesh-member-promenade"' not in scene
    assert '"forkmesh-office-land-promenade"' not in scene
    assert "for (let x = -5.4; x <= 5.4; x += 1.2)" not in scene


def test_repository_and_leaderboard_districts_share_textured_flush_paths():
    scene = source()
    for contract in (
        "const WORLD_PATH_WIDTH = OFFICE_BRIDGE_WIDTH;",
        "const WORLD_PATH_HEIGHT = 0.08;",
        "const WORLD_PATH_SURFACE_Y = 0.105;",
        (
            "const REPOSITORY_GROUND_RADIUS = "
            "REPOSITORY_ISLAND_RING_RADIUS + 7;"
        ),
        "function createDistrictGroundCircle(",
        "radius = DISTRICT_GROUND_RADIUS,",
        "`forkmesh-${kind}-textured-ground`",
        '"repositories",\n      REPOSITORY_GROUND_RADIUS,',
        'createDistrictGroundCircle(\n      THREE,\n      "leaderboards",\n      LEADERBOARD_DISTRICT_GROUND_RADIUS,\n    )',
        "repositoryConnectionLength,\n      WORLD_PATH_HEIGHT,\n      WORLD_PATH_WIDTH,",
        "leaderboardConnectionLength,\n      WORLD_PATH_HEIGHT,\n      WORLD_PATH_WIDTH,",
        "repositoryPromenade.position.y = WORLD_PATH_CENTER_Y;",
        "leaderboardPromenade.position.y = WORLD_PATH_CENTER_Y;",
        'importKiosk.position.set(0, 0, 0);',
        'color: repository ? "#c1bbb0" : "#8a829b"',
    ):
        assert contract in scene
    assert "new THREE.BoxGeometry(repositoryConnectionLength, 0.08, 7.2)" not in scene
    assert "new THREE.BoxGeometry(leaderboardConnectionLength, 0.08, 7.2)" not in scene
    assert "guide.position.set(0, 0.15, z)" not in scene
    assert "beacon.position.set(x, 0.72, z)" not in scene


def test_continuous_visible_foundation_drives_walkability():
    scene = source()
    for contract in (
        "return roundedHexagonContains(",
        "WORLD_BIKE_LANE_CENTER_X,\n    CONTINUOUS_CITY_CENTER_Z,\n    CONTINUOUS_CITY_RADIUS,",
        "BEACH_ROAD_MIN_X + margin",
        "BEACH_RADIUS - margin",
    ):
        assert contract in scene
    assert "WORLD_LOOP_CONTROL_POINTS" not in scene


def test_world_edge_walk_is_gone():
    scene = source()
    for retired in (
        "VOID_WALK_",
        "voidWalkSurfaceContains",
        "createVoidWalkRails",
        "forkmesh-void-walk-rails",
        '"void-walk-rails"',
    ):
        assert retired not in scene


def test_desert_plots_ring_the_hexagonal_city_with_start_world_signs():
    scene = source()
    for contract in (
        "const DESERT_PLOT_APOTHEM = 88;",
        "const DESERT_PLOTS = Object.freeze(",
        "const DESERT_HEXES = Object.freeze(",
        "const WORLD_HEX_PITCH = CONTINUOUS_CITY_RADIUS * 2 + WORLD_ROAD_WIDTH;",
        "CONTINUOUS_CITY_RADIUS + WORLD_ROAD_WIDTH + DESERT_PLOT_APOTHEM;",
        "centers: DESERT_HEXES\n        .filter((_, side) => !petals.has(side))\n"
        "        .map((hex) => [hex.x, -hex.z]),",
        "function createDesertPlots(THREE, interactive)",
        'sand.name = "forkmesh-desert-plot-sand";',
        "sand.userData.ground = true;",
        '"START YOUR",',
        '"WORLD HERE",',
        '"YOUR OWN INSTANCE · YOUR NAME",',
        'face.userData.interactive = "start-world-sign";',
        '"desert-plots", "Desert plots · start your world signs", "Terrain",',
        "desertWalkSurfaceContains(x, z, radius) ||",
        'if (hit?.object?.userData?.interactive === "start-world-sign") {\n'
        "      onStartWorldSignSelect(hit.object.userData.plot);",
    ):
        assert contract in scene
    assert scene.count("desertWalkSurfaceContains(x, z, radius) ||") == 2
    assert "DESERT_PLOTS.find((plot) => plot.hex === side)," in scene
    assert "signPlots.forEach((plot, index) => {" in scene
    assert "DESERT_PLOTS.forEach((plot, index) => {" not in scene


def test_paths_share_a_concrete_brick_texture_and_hex_road_network():
    scene = source()
    for contract in (
        "function concreteBrickTexture(THREE)",
        "function concreteBrickMaterial(THREE)",
        '"/world/assets/concrete-brick-path-v1.webp"',
        "    8,\n    2,",
        "const WORLD_ROAD_WIDTH = 10;",
        "function createHexRoadNetwork(THREE)",
        '"forkmesh-world-bike-lane"',
        '"forkmesh-world-road-edge-lines"',
        '"forkmesh-world-bike-center-groove"',
        "new THREE.InstancedMesh(\n    dashGeometry,",
        "roadRoutePlaceOnLoop(state.route, along);",
    ):
        assert contract in scene
    assert "WORLD_BIKE_LANE_RADIUS" not in scene
    assert "bikeLaneMaterial(THREE)" not in scene
    assert "WORLD_LOOP_CONTROL_POINTS" not in scene
    roads = scene.split("function createHexRoadNetwork(THREE) {", 1)[1].split(
        "\n}\n", 1
    )[0]
    assert roads.count("new THREE.Mesh(") == 2
    assert roads.count("new THREE.InstancedMesh(") == 1


def test_every_leaderboard_is_its_own_card_on_the_leaderboard_circle():
    scene = source()
    physical = world_scene_module("districts/leaderboards.js")
    for contract in (
        'addLeaderboardCard("active-members", makeActiveLeaderboardSign(THREE))',
        'addLeaderboardCard("referrals", makeReferralLeaderboardSign(THREE))',
        'addLeaderboardCard("http-referrers", makeSiteReferrerLeaderboardSign(THREE))',
        "for (const board of WORLD_LEADERBOARD_BOARD_STUBS) {",
        "addLeaderboardCard(board.id, makeLeaderboardStatSign(THREE, board));",
        'circle: "leaderboards",',
        'feed: "leaderboards",',
    ):
        assert contract in physical
    # The one 5x5 wall that replaced the cards is gone, painter included.
    assert '"forkmesh-leaderboard-super-panel"' not in scene
    assert "leaderboardSuperPanel" not in scene
    assert "function leaderboardGridTexture" not in scene
    assert "function leaderboardGridBoardDescriptors" not in scene
    assert "for (let index = 0; index < 25; index += 1)" not in scene
    assert '"forkmesh-leaderboard-ring-walk"' not in scene


def test_leaderboard_circle_is_open_without_a_cover_or_door_gate():
    scene = source()
    district = scene.split(
        'leaderboardDistrict.name = "forkmesh-leaderboard-district"', 1
    )[1].split("const leaderboardBeacon", 1)[0]
    assert 'leaderboardInterior.name = "forkmesh-leaderboard-interior"' in district
    assert "leaderboardDistrict.add(leaderboardInterior);" in district
    assert "leaderboardInterior.visible = false" not in district
    assert "createOpaqueDistrictCover" not in scene
    assert "createLeaderboardOpaqueCover" not in scene
    assert "forkmesh-leaderboard-opaque-cover" not in scene
    assert "leaderboard-cover" not in scene


def test_open_node_yard_draws_every_cabinet_at_full_detail():
    scene = source()
    district = scene.split(
        'nodeDistrict.name = "forkmesh-node-district"', 1
    )[1].split("const systemCapacityPlatform", 1)[0]
    assert '"forkmesh-node-opaque-cover"' not in scene
    assert "function constrainNodeCover" not in scene
    assert 'nodeInterior.name = "forkmesh-node-interior"' in district
    assert "nodeInterior.visible = false" not in district
    assert "nodeInterior.add(fountainLandmark)" in district
    assert "nodeDistrict.add(nodeInterior);" in district
    assert "nodeInterior.add(cabinet)" in scene
    # No camera-distance level of detail: the cabinets are the yard, so the
    # instanced box stand-ins and the whole detail switch are gone.
    for removed in (
        "NODE_DETAIL_ENTER_DISTANCE",
        "NODE_DETAIL_EXIT_DISTANCE",
        "NODE_LOD_MAX_INSTANCES",
        "createMirrorNodeBoxLod",
        "forkmesh-node-box-lod",
        "nodeBoxLod",
        "nodeDetailVisible",
        "nodeDetailLevel",
        "nodeLodVisible",
        "updateNodeDetailLevel",
    ):
        assert removed not in scene, f"node yard LOD leftover: {removed}"
    assert 'if (worldElementEnabled("node-cabinets")) {' in scene
    assert "if (!group.parent?.visible) return;" in scene


def test_leaderboard_contents_are_always_mounted_in_the_open_district():
    scene = source()
    district = scene.split(
        'leaderboardDistrict.name = "forkmesh-leaderboard-district"', 1
    )[1].split("const leaderboardBeacon", 1)[0]
    assert (
        'leaderboardInterior.name = "forkmesh-leaderboard-interior"'
        in district
    )
    assert "leaderboardInterior.visible = false" not in district
    assert "leaderboardInterior.add(\n    createDistrictGroundCircle" in district
    # Boards mount on their own circle, and every circle mounts on the open
    # district interior, so nothing is hidden behind a cover or a gate.
    assert "leaderboardInterior.add(group);" in scene
    assert "circle.group.add(object);" in scene
    assert "updateLeaderboardCircleOccupancy" not in scene
    assert "leaderboardCoverContainsWorldPoint" not in scene


def test_only_office_and_beach_isolate_scenes():
    scene = source()
    isolation = scene.split("function syncEnclosureSceneVisibility", 1)[1].split(
        "function updateRepositoryRingExpansion", 1
    )[0]
    for mode in ("office", "beach"):
        assert f'? "{mode}"' in isolation or f': "{mode}"' in isolation
    for open_district in ("repositories", "leaderboards", "members", "nodes"):
        assert f'"{open_district}"' not in isolation
    assert "enclosureHiddenWorldRoots.set(root, root.visible)" in isolation
    assert "root.visible = false" in isolation
    assert "enclosureHiddenWorldRoots.forEach" in isolation
    assert "world.userData.activeEnclosureScene = next" in isolation
    assert "syncEnclosureSceneVisibility();" in scene


def test_remote_district_packages_replace_mainland_with_low_triangle_horizon():
    """Only a remote district package space replaces the furnished mainland."""

    scene = source()
    isolation = scene.split("function syncEnclosureSceneVisibility", 1)[1].split(
        "function compactDistrictDiagnostics", 1
    )[0]
    for contract in (
        'const remoteHorizon = next === "remote-district";',
        'const panoramic = next === "office";',
        "enclosureExteriorIsCostly(root, roots[0])",
        'mainlandHigh.visible = !remoteHorizon;',
        'packageSpaces.get(currentSpace)?.remoteHorizon',
        '? "remote-district"',
        'mainlandLow.visible = remoteHorizon;',
        'world.userData.mainlandDetailLevel = remoteHorizon ? "low" : "detailed";',
        'root.visible = false;',
    ):
        assert contract in isolation
    assert 'lod.name = "forkmesh-mainland-distance-lod"' in scene
    assert "const MAINLAND_DISTANT_TRIANGLE_BUDGET = 120;" in scene
    assert "mainlandDistantTriangleCount = triangleWeight(low)" in scene


def test_leaderboard_has_no_wall_or_door_collision_constraint():
    scene = source()
    assert "leaderboardDoorCrossingIsClear" not in scene
    assert "constrainLeaderboardCover" not in scene
    assert "LEADERBOARD_COVER_APOTHEM" not in scene
    assert "LEADERBOARD_DOOR_WIDTH" not in scene


def test_two_clickable_bikes_use_normal_movement_and_collision():
    scene = source()
    for contract in (
        'addWorldBike(0, "#77d9ff", 0.38)',
        'addWorldBike(1, "#9ef7c6", 0.43)',
        "function rideBike(index)",
        "function applyBikeRidePose(moving, delta)",
        "function dismountBike(",
        "function toggleNearestBikeRide()",
        'event.code === "KeyE"',
        "placeBikeOnLane(",
        "BIKE_RIDE_SPEED_MULTIPLIER",
        "input.ridingBike ? BIKE_RIDE_SPEED_MULTIPLIER : 1",
        "worldWalkSurfaceContains(",
        "hit.object.userData.bikeIndex",
        "circularRideCameraYaw(",
        "const previousBikeHeading = state.bike.rotation.y",
        '"RIDE BIKE"',
        '"CLICK OR E · W/S PEDAL · A/D STEER"',
        "state.hint.visible = false",
    ):
        assert contract in scene


def test_two_cloned_quadcopters_stay_fully_expanded_and_fly_on_three_axes():
    scene = source()
    prototype = scene[
        scene.index("function createWorldQuadcopterPrototype()"):
        scene.index("function addWorldQuadcopter(")
    ]
    assert "THREE.LOD" not in prototype
    assert "addLevel(" not in prototype
    assert "forkmesh-world-quadcopter-far" not in scene
    assert prototype.count("rotor.userData.quadcopterRotor = true;") == 1
    assert "for (const [rotorIndex, [x, z]] of [" in prototype
    assert 'name: "forkmesh-world-quadcopter-frame"' in prototype
    assert 'seatMarker.name = "forkmesh-world-quadcopter-seat";' in prototype
    for contract in (
        "function createWorldQuadcopterPrototype()",
        "const quadcopterPrototype = createWorldQuadcopterPrototype();",
        "const quadcopter = quadcopterPrototype.clone(true);",
        'quadcopter.name = `forkmesh-world-quadcopter-${index + 1}`',
        "addWorldQuadcopter(0, [-34, 0.24, -42]);",
        "addWorldQuadcopter(1, [34, 0.24, -42]);",
        "const QUADCOPTER_HORIZONTAL_SPEED = 42",
        "const QUADCOPTER_VERTICAL_SPEED = 28",
        "const QUADCOPTER_MAX_ALTITUDE = 480",
        "function rideQuadcopter(index)",
        "function dismountQuadcopter(",
        "function toggleNearestQuadcopterRide()",
        "function updateQuadcopterRide(input, delta, time)",
        '["Space", "KeyC", "ControlLeft", "ControlRight"]',
        "hit.object.userData.quadcopterIndex",
        "prepareRoofParachute()",
        "QUADCOPTER_RIDING_ACTIVITY",
        "getQuadcopterState:",
        '"FLY QUADCOPTER"',
        '"CLICK OR E · WASD · SPACE UP · C DOWN"',
        "const legacyForwardInput =",
        "movement.addScaledVector(forward, legacyForwardInput);",
        "movement.addScaledVector(right, legacyRightInput);",
    ):
        assert contract in scene


def test_dismounting_parks_the_quadcopter_where_a_walker_can_reach_it():
    scene = source()
    for contract in (
        "const QUADCOPTER_REST_ALTITUDE = 0.24;",
        "const QUADCOPTER_PARK_CLEARANCE = 4;",
        "export function parkedQuadcopterPoint(",
        "function parkQuadcopter(state)",
        "walkSurfaceContains: activeWalkSurfaceContains,",
    ):
        assert contract in scene
    dismount = scene.split("function dismountQuadcopter(", 1)[1].split(
        "function updateQuadcopterRide(", 1
    )[0]
    # Landing the airframe is what makes the vehicle mountable again: the mount
    # test in toggleNearestQuadcopterRide measures all three axes.
    assert "parkQuadcopter(state);" in dismount
    assert "state.quadcopter.position.x + Math.cos(player.rotation.y)" in dismount
    assert "state.quadcopter.position.z - Math.sin(player.rotation.y)" in dismount


def test_a_quadcopter_left_at_the_office_parks_outside_its_keep_out_box():
    script = f"""
      import {{ parkedQuadcopterPoint }} from {
          json.dumps(SCENE_PATH.as_uri())
      };
      const office = {{ officeX: 0, officeZ: -207 }};
      process.stdout.write(JSON.stringify({{
        insideTower: parkedQuadcopterPoint(0, -207, office),
        rearCorner: parkedQuadcopterPoint(20, -240, office),
        openTown: parkedQuadcopterPoint(34, -42, office),
        offSurface: parkedQuadcopterPoint(0, -1250, office),
      }}));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    parked = json.loads(result.stdout)
    def clear_of_the_tower(point):
        x = abs(point["x"])
        z = abs(point["z"] + 207)
        return max(z, 0.8660254037844386 * x + 0.5 * z) >= 37

    def inside_rounded_hex(point, centre_x, centre_z, apothem, corner):
        across = abs(point["z"] - centre_z)
        along = abs(point["x"] - centre_x)
        fold = min(-0.8660254037844386 * across + 0.5 * along, 0)
        across += 2 * fold * 0.8660254037844386
        along -= fold
        half_edge = 0.5773502691896258 * (apothem - corner)
        across -= max(-half_edge, min(half_edge, across))
        along -= apothem - corner
        return math.copysign(math.hypot(across, along), along) <= corner

    tiles = [(0.0, -45.0)] + [
        (
            math.cos(side * math.pi / 3) * 740,
            -45 + math.sin(side * math.pi / 3) * 740,
        )
        for side in range(6)
    ]
    junctions = [
        (
            math.cos(math.pi / 6 + corner * math.pi / 3) * 740 / math.sqrt(3) * loop,
            -45 + math.sin(math.pi / 6 + corner * math.pi / 3) * 740 / math.sqrt(3) * loop,
        )
        for loop in (1, 2)
        for corner in range(6)
    ]

    def reachable_on_foot(point):
        return any(
            inside_rounded_hex(point, x, z, 375, 80) for x, z in tiles
        ) or any(math.hypot(point["x"] - x, point["z"] - z) <= 16 for x, z in junctions)

    for key in ("insideTower", "rearCorner", "openTown", "offSurface"):
        assert clear_of_the_tower(parked[key]), key
        assert reachable_on_foot(parked[key]), key
    # A vehicle already parked on open ground is left exactly where it landed.
    assert parked["openTown"] == {"x": 34, "z": -42}
    # Everything else lands within one mount distance (7) of ground a visitor
    # can walk to: just outside the tower's front wall, or the outer road.
    assert -170 < parked["insideTower"]["z"] < -160
    assert -170 < parked["rearCorner"]["z"] < -160
    assert not reachable_on_foot({"x": 0, "z": -1250})
    assert -1115 < parked["offSurface"]["z"] < -800


def test_the_quadcopter_flies_where_the_camera_looks_and_speeds_up():
    script = f"""
      import {{
        quadcopterCruiseSpeed,
        quadcopterFlightPitch,
        quadcopterFlightVector,
      }} from {json.dumps(SCENE_PATH.as_uri())};
      const restingOrbit = Math.asin(16 / Math.hypot(17, 16, 21));
      const flight = (yaw, pitch, forward, right) => {{
        const v = quadcopterFlightVector({{}}, yaw, pitch, forward, right);
        return [v.x, v.y, v.z].map((n) => Number(n.toFixed(4)));
      }};
      process.stdout.write(JSON.stringify({{
        restingOrbitPitch: quadcopterFlightPitch(
          "third-person", restingOrbit, 0.7,
        ),
        orbitLookingUp: quadcopterFlightPitch(
          "third-person", restingOrbit - 0.5, 0,
        ),
        firstPersonLookingDown: quadcopterFlightPitch(
          "first-person", restingOrbit, 0.4,
        ),
        straightUpClamp: quadcopterFlightPitch("third-person", -3, 0),
        levelNorth: flight(0, 0, 1, 0),
        climbing: flight(0, -Math.PI / 4, 1, 0),
        diving: flight(Math.PI / 2, Math.PI / 4, 1, 0),
        strafe: flight(0, 0.6, 0, 1),
        speeds: [0, 1, 2, 3, 60].map(quadcopterCruiseSpeed),
      }}));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    flight = json.loads(result.stdout)
    assert abs(flight["restingOrbitPitch"]) < 1e-9
    assert abs(flight["orbitLookingUp"] + 0.5) < 1e-9
    assert flight["firstPersonLookingDown"] == 0.4
    assert abs(flight["straightUpClamp"] + 1.5707963) < 1e-6
    assert flight["levelNorth"] == [0, 0, -1]
    assert flight["climbing"] == [0, 0.7071, -0.7071]
    assert flight["diving"] == [-0.7071, -0.7071, 0]
    assert flight["strafe"] == [1, 0, 0]
    assert flight["speeds"] == [42, 132, 150, 150, 150]
    scene = source()
    ride = scene.split("function updateQuadcopterRide(", 1)[1].split(
        "function setJetpackEquipped(", 1
    )[0]
    assert "forwardInput > 0 ? quadcopterRide.cruiseSeconds + delta : 0" in ride
    assert "quadcopterFlightPitch(cameraMode, cameraPitch, firstPersonPitch)" in ride
    assert "flight.y + vertical * QUADCOPTER_VERTICAL_SPEED" in ride
    assert "quadcopterRide = { index, cruiseSeconds: 0, speed: 0 };" in scene


def test_avatar_full_detail_reuses_buffers_without_camera_lod():
    scene = source()
    avatar = scene.split("function createAvatar(THREE, identity", 1)[1].split(
        "function applyAvatarLookDirection", 1
    )[0]
    assert 'highDetail.name = "avatar-full-detail"' in avatar
    assert "group.add(highDetail);" in avatar
    assert "new THREE.LOD()" not in avatar
    assert "cloneAvatarFarModel" not in scene
    assert "cloneSharedMesh(" in avatar
    assert "function disposeOwnedMaterial(material, disposeMap = false)" in scene
    assert "disposeOwnedMaterial(child.material, true);" in scene


def test_world_scale_drone_course_has_ten_low_poly_instanced_rings():
    scene = source()
    course = scene.split("function createDroneRaceCourse()", 1)[1].split(
        "const droneRaceCourse = createDroneRaceCourse();", 1
    )[0]
    assert "const DRONE_COURSE_RING_COUNT = 10;" in scene
    assert "const DRONE_COURSE_WEAVE_WIDTH = 18;" in scene
    assert "const DRONE_COURSE_WEAVE_HEIGHT = 14;" in scene
    assert "new THREE.InstancedMesh(" in course
    assert "DRONE_COURSE_RING_COUNT" in course
    assert "new THREE.TorusGeometry(8, 0.55, 4, 12)" in course
    assert "new THREE.TorusGeometry(8, 0.72, 3, 8)" in course
    assert "course.addLevel(detailedRings, 0, 0.12);" in course
    assert "course.addLevel(coarseRings, DRONE_COURSE_COARSE_DISTANCE, 0.16);" in course
    assert "activeCamera.getWorldPosition(cameraCoursePosition);" in course
    assert "cameraCoursePosition.distanceTo(center)" in course
    assert "DRONE_COURSE_DETAIL_ENTER_DISTANCE" in course
    assert 'course.userData.detailLevel = detailedVisible ? "detailed" : "coarse"' in course
    assert 'course.name = "forkmesh-drone-race-course"' in course


def test_car_and_quadcopter_hints_explain_directional_controls_nearby():
    scene = source()
    for contract in (
        '"forkmesh-beach-road-car-control-hint"',
        '"CLICK TO ENTER · WASD DRIVE · SHIFT BOOST"',
        '"forkmesh-world-quadcopter-control-hint"',
        "if (state.hint) state.hint.visible = false",
        "if (state?.hint) state.hint.visible = true",
    ):
        assert contract in scene


def test_treasury_sign_is_centered_with_a_new_window_node_download():
    scene = source()
    for contract in (
        "treasurySign.position.set(0, 1, 0)",
        '"reward-treasury-start-node-button"',
        '"start-node-download"',
        '"START A NODE"',
        '"DOWNLOAD APP  ↗"',
        "onStartNodeDownload();",
    ):
        assert contract in scene


def test_bike_first_person_yaw_follows_wrapped_lane_heading_delta():
    script = f"""
      import {{ circularRideCameraYaw }} from {
          json.dumps(SCENE_PATH.as_uri())
      };
      const wrapped = circularRideCameraYaw(0.4, 3.1, -3.1);
      const ordinary = circularRideCameraYaw(-0.2, 0.8, 0.5);
      process.stdout.write(JSON.stringify([wrapped, ordinary]));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    wrapped, ordinary = json.loads(result.stdout)
    assert abs(wrapped - 0.4831853071795864) < 1e-9
    assert abs(ordinary - (-0.5)) < 1e-9


def test_connected_beach_has_local_horizon_car_and_clickable_seating():
    scene = source()
    for contract in (
        '"forkmesh-beach-road"',
        '"forkmesh-beach-sand"',
        '"forkmesh-beach-water"',
        '"forkmesh-beach-peripheral-horizon"',
        '"/world/assets/beach-horizon-v1.webp"',
        '"forkmesh-beach-road-car"',
        "function rideCar(index)",
        "function applyCarRidePose(moving, delta)",
        "CAR_RIDE_SPEED_MULTIPLIER",
        "hit.object.userData.carIndex",
        "function addWorldBench({",
        "function sitOnWorldSeat(seat)",
        "hit?.object?.userData?.worldSeat",
    ):
        assert contract in scene


def test_land_is_continuous_and_uses_a_cached_procedural_grass_texture():
    scene = source()
    for contract in (
        '"forkmesh-continuous-city-land"',
        '"/world/assets/concrete-brick-path-v1.webp"',
        "function projectAssetTexture(",
        "const CITY_GRASS_PATTERN_SIZE = 64;",
        "const cityGrassTextures = new WeakMap();",
        "function cityGrassTexture(THREE)",
        "new THREE.DataTexture(",
        "texture.generateMipmaps = true;",
        "cityGrassTextures.set(THREE, texture);",
        "function worldWalkSurfaceContains(",
    ):
        assert contract in scene
    assert "city-park-grass-v1.webp" not in scene


def test_start_here_map_persists_bounded_progress_between_visits():
    scene = source()
    for contract in (
        "const START_HERE_STEPS = Object.freeze([",
        '"forkmesh-start-here-map"',
        '"forkmesh.world.start-here.v1"',
        "validStartHereStepIds.has(step)",
        "localStorage.setItem(",
        'completeStartHereStep("people")',
        'completeStartHereStep("nodes")',
        'completeStartHereStep("repositories")',
        'completeStartHereStep("office")',
    ):
        assert contract in scene


def test_environment_tracks_the_visitors_local_daylight_without_frame_churn():
    scene = source()
    assert "const observedMinute = now.getHours() * 60 + now.getMinutes();" in scene
    assert 'daylightMode === "day"' in scene
    assert 'daylightMode === "night"' in scene
    assert "const easedDaylight =" in scene
    assert "nextEnvironmentCheckAt = time + 15_000;" in scene
    assert "if (minuteOfDay !== localDaylightMinute)" in scene
    assert 'moon.name = "forkmesh-world-moonlight"' in scene
    assert "moon.castShadow = false" in scene
    assert "scene.add(sun.target)" in scene
    assert "(1 - easedDaylight) * 0.9" in scene


def test_bike_loop_follows_the_road_around_the_city_hexagon():
    script = f"""
      import {{
        hexagonSignedDistance,
        roundedHexagonLoopLength,
        roundedHexagonLoopPoint,
      }} from {json.dumps(SCENE_PATH.as_uri())};
      const total = roundedHexagonLoopLength();
      const points = [];
      for (let step = 0; step <= 2000; step += 1) {{
        points.push(roundedHexagonLoopPoint((step / 2000) * total, {{}}));
      }}
      const offsets = points.map((p) =>
        hexagonSignedDistance(p.x, p.z + 45, 295),
      );
      const gaps = points.slice(1).map((p, i) =>
        Math.hypot(p.x - points[i].x, p.z - points[i].z),
      );
      const turns = points.slice(1).map((p, i) =>
        Math.abs(Math.atan2(
          Math.sin(p.heading - points[i].heading),
          Math.cos(p.heading - points[i].heading),
        )),
      );
      const facing = points.slice(1).map((p, i) => {{
        const dx = p.x - points[i].x;
        const dz = p.z - points[i].z;
        const heading = Math.atan2(-dx, -dz);
        return Math.abs(Math.atan2(
          Math.sin(heading - p.heading),
          Math.cos(heading - p.heading),
        ));
      }});
      process.stdout.write(JSON.stringify({{
        total,
        start: points[0],
        end: points.at(-1),
        minOffset: Math.min(...offsets),
        maxOffset: Math.max(...offsets),
        maxGap: Math.max(...gaps),
        maxTurn: Math.max(...turns),
        maxFacing: Math.max(...facing),
      }}));
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    loop = json.loads(result.stdout)
    edge = (295 * 2) / math.sqrt(3)
    assert math.isclose(loop["total"], 6 * edge + 2 * math.pi * 75)
    assert math.isclose(loop["start"]["x"], 370)
    assert math.isclose(loop["start"]["z"], -45)
    assert math.isclose(loop["end"]["x"], 370)
    assert abs(loop["end"]["z"] + 45) < 1e-6
    assert 74.999 < loop["minOffset"] <= loop["maxOffset"] < 75.001
    assert loop["maxGap"] < loop["total"] / 2000 + 1e-6
    assert loop["maxTurn"] < 0.05
    assert loop["maxFacing"] < 0.05
