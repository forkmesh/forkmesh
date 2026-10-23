import json
import math
import subprocess
from pathlib import Path

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = WORLD / "scene"
ROADS = SCENE / "districts" / "roads.js"
BIKES = SCENE / "districts" / "bikes.js"
MOVEMENT = SCENE / "movement.js"
LAYOUT = SCENE / "layout.js"
AVATARS = SCENE / "avatars.js"
THREE = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"


def run_node(body):
    script = f"""
      import * as roads from {json.dumps(ROADS.as_uri())};
      import * as movement from {json.dumps(MOVEMENT.as_uri())};
      import * as layout from {json.dumps(LAYOUT.as_uri())};
      const out = (value) => process.stdout.write(JSON.stringify(value));
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_road_graph_is_derived_from_the_drawn_hex_tiles():
    graph = run_node(
        """
        const g = roads.worldRoadGraph();
        const same = roads.worldRoadGraph() === g;
        const side = (layout.WORLD_BIKE_LANE_APOTHEM * 2) / Math.sqrt(3);
        const junctionDegrees = layout.WORLD_ROAD_JUNCTIONS.map((j) => {
          const node = g.nodes.find((n) => Math.hypot(n.x - j.x, n.z - j.z) < 1e-6);
          return node ? node.out.length : -1;
        });
        const edgesConnect = g.edges.every((e) =>
          g.nodes[e.from].out.includes(e.id) &&
          g.edges[e.reverse].from === e.to &&
          g.edges[e.reverse].to === e.from &&
          Math.abs(e.length - side) < 1e-6 &&
          e.hexes.length >= 1 && e.hexes.length <= 2,
        );
        const everyEdgeContinues = g.edges.every((e) => e.turns.length >= 1);
        out({
          same,
          nodes: g.nodes.length,
          directed: g.edges.length,
          degrees: g.nodes.map((n) => n.out.length),
          junctionDegrees,
          edgesConnect,
          everyEdgeContinues,
          tiles: layout.WORLD_HEX_TILES.length,
        });
        """
    )
    assert graph["same"]
    assert graph["tiles"] == 7
    assert graph["nodes"] == 24
    assert graph["directed"] == 60
    assert sorted(graph["degrees"]) == [2] * 12 + [3] * 12
    assert graph["junctionDegrees"] == [3] * 12
    assert graph["edgesConnect"]
    assert graph["everyEdgeContinues"]


def test_turn_arcs_join_the_straights_tangentially():
    result = run_node(
        """
        const g = roads.worldRoadGraph();
        let worstGap = 0;
        let worstBend = 0;
        const point = {};
        for (const turn of g.turns) {
          const route = { graph: g, edge: turn.from, next: turn, hex: turn.hex, s: g.straightLength - 1e-9, choice: 0 };
          roads.roadRoutePoint(route, point);
          const before = { ...point };
          route.s = g.straightLength + 1e-9;
          roads.roadRoutePoint(route, point);
          worstGap = Math.max(worstGap, Math.hypot(point.x - before.x, point.z - before.z));
          route.s = g.pieceLength - 1e-9;
          roads.roadRoutePoint(route, point);
          const endArc = { ...point };
          const after = { graph: g, edge: turn.edge, next: g.edges[turn.edge].turns[0], hex: turn.hex, s: 0, choice: 0 };
          roads.roadRoutePoint(after, point);
          worstGap = Math.max(worstGap, Math.hypot(point.x - endArc.x, point.z - endArc.z));
          worstBend = Math.max(worstBend, Math.abs(Math.atan2(
            Math.sin(point.heading - endArc.heading),
            Math.cos(point.heading - endArc.heading),
          )));
        }
        out({ worstGap, worstBend, turns: g.turns.length, radius: g.cornerRadius, bike: layout.WORLD_BIKE_LANE_CORNER_RADIUS });
        """
    )
    assert result["turns"] == 96
    assert result["radius"] == result["bike"]
    assert result["worstGap"] < 1e-6
    assert result["worstBend"] < 1e-6



def test_outer_junctions_curve_one_road_into_the_next():
    result = run_node(
        """
        const g = roads.worldRoadGraph();
        const outer = layout.WORLD_ROAD_JUNCTIONS.filter((j) => j.bend);
        const point = {};
        const rides = outer.map((junction) => {
          const node = g.nodes.find((n) => Math.hypot(n.x - junction.x, n.z - junction.z) < 1e-6);
          const through = g.turns.filter((t) => g.edges[t.from].to === node.id &&
            !g.edges[t.from].hexes.some((hex) => g.edges[t.edge].hexes.includes(hex)));
          let offRoad = 0;
          let closest = Infinity;
          for (const turn of through) {
            const route = { graph: g, edge: turn.from, next: turn, hex: turn.hex, s: 0, choice: 0 };
            for (let s = g.straightLength - 40; s < g.pieceLength; s += 0.5) {
              route.s = s;
              roads.roadRoutePoint(route, point);
              if (!movement.desertWalkSurfaceContains(point.x, point.z, 0.5)) offRoad += 1;
              closest = Math.min(closest, Math.hypot(point.x - junction.x, point.z - junction.z));
            }
          }
          const lane = Math.hypot(junction.bend.x - junction.x, junction.bend.z - junction.z);
          return { through: through.length, offRoad, closest, lane };
        });
        out({ count: outer.length, rides });
        """
    )
    assert result["count"] == 6
    for ride in result["rides"]:
        assert ride["through"] == 2
        assert ride["offRoad"] == 0
        assert 10 < ride["closest"] < 13
        assert math.isclose(ride["lane"], 740 / math.sqrt(3), rel_tol=1e-9)


def test_outer_junction_dashes_follow_the_through_road():
    script = f"""
      import * as THREE from {json.dumps(THREE.as_uri())};
      import * as roads from {json.dumps(ROADS.as_uri())};
      import * as layout from {json.dumps(LAYOUT.as_uri())};
      const dashes = roads.createHexRoadNetwork(THREE).children.find((c) => c.name === "forkmesh-world-bike-center-groove");
      const matrix = new THREE.Matrix4();
      const centres = [];
      for (let i = 0; i < dashes.count; i += 1) {{
        dashes.getMatrixAt(i, matrix);
        centres.push([matrix.elements[12], matrix.elements[14]]);
      }}
      const corner = layout.WORLD_BIKE_LANE_CORNER_RADIUS;
      const rows = layout.WORLD_ROAD_JUNCTIONS.filter((j) => j.bend).map((junction) => {{
        const reach = (corner * 2) / Math.sqrt(3);
        const toBend = Math.atan2(junction.bend.z - junction.z, junction.bend.x - junction.x);
        const cx = junction.x + Math.cos(toBend) * reach;
        const cz = junction.z + Math.sin(toBend) * reach;
        const near = centres.filter(([x, z]) => Math.hypot(x - junction.x, z - junction.z) < 40);
        return {{
          near: near.length,
          onBend: near.filter(([x, z]) => Math.abs(Math.hypot(x - cx, z - cz) - corner) < 0.01).length,
        }};
      }});
      process.stdout.write(JSON.stringify(rows));
    """
    rows = json.loads(
        subprocess.run(
            ["node", "--input-type=module", "-e", script],
            check=True,
            text=True,
            capture_output=True,
        ).stdout
    )
    assert len(rows) == 6
    for row in rows:
        assert row["onBend"] >= 6
        assert row["near"] - row["onBend"] <= 4


def test_the_default_route_is_the_old_town_loop():
    result = run_node(
        """
        const g = roads.worldRoadGraph();
        const total = movement.roundedHexagonLoopLength();
        const route = roads.createRoadRoute(g);
        roads.roadRoutePlaceOnLoop(route, 0.38 * total);
        const a = {};
        const b = {};
        let miss = 0;
        let turn = 0;
        for (let step = 0; step <= 3000; step += 1) {
          movement.roundedHexagonLoopPoint(0.38 * total + (step * total) / 1500, b);
          roads.roadRoutePoint(route, a);
          miss = Math.max(miss, Math.hypot(a.x - b.x, a.z - b.z));
          turn = Math.max(turn, Math.abs(Math.atan2(Math.sin(a.heading - b.heading), Math.cos(a.heading - b.heading))));
          roads.roadRouteAdvance(route, total / 1500, 0);
        }
        for (let step = 0; step <= 3000; step += 1) roads.roadRouteAdvance(route, -total / 1500, 0);
        movement.roundedHexagonLoopPoint(0.38 * total, b);
        roads.roadRoutePoint(route, a);
        out({ miss, turn, back: Math.hypot(a.x - b.x, a.z - b.z), hex: route.hex });
        """
    )
    assert result["miss"] < 1e-6
    assert result["turn"] < 1e-6
    assert result["back"] < 1e-6
    assert result["hex"] == 0



def test_riding_the_outer_edge_stays_on_the_outer_road_unless_steered_in():
    result = run_node(
        """
        const g = roads.worldRoadGraph();
        const start = g.edges.find((e) => e.hexes.length === 1);
        const lap = 18 * g.pieceLength;
        const ride = (direction) => {
          const route = roads.createRoadRoute(g);
          route.edge = start.id;
          route.hex = start.hexes[0];
          route.next = roads.roadDefaultTurn(start, route.hex);
          route.s = g.straightLength / 2;
          const seen = new Set();
          let inner = 0;
          for (let step = 0; step < 3600; step += 1) {
            roads.roadRouteAdvance(route, (direction * lap) / 3600, 0);
            const edge = g.edges[route.edge];
            if (edge.hexes.length !== 1) inner += 1;
            seen.add(Math.min(edge.id, edge.reverse));
          }
          return { inner, roads: seen.size, back: route.edge === start.id, s: route.s };
        };
        const junction = g.nodes.find((n) =>
          n.out.length === 3 && n.out.filter((id) => g.edges[id].hexes.length === 1).length === 2,
        );
        const incoming = g.edges.find((e) => e.to === junction.id && e.hexes.length === 1);
        const inwardSide = incoming.turns.find((t) => g.edges[t.edge].hexes.length === 2).side;
        const steerAt = (steer) => {
          const route = roads.createRoadRoute(g);
          route.edge = incoming.id;
          route.hex = incoming.hexes[0];
          route.next = roads.roadDefaultTurn(incoming, route.hex);
          route.s = g.straightLength - 40;
          for (let step = 0; step < 400; step += 1) {
            roads.roadRouteAdvance(route, 0.5, route.edge === incoming.id ? steer : 0);
          }
          return g.edges[route.edge].hexes.length;
        };
        out({
          forward: ride(1),
          backward: ride(-1),
          straight: steerAt(0),
          outward: steerAt(-inwardSide),
          inward: steerAt(inwardSide),
          options: incoming.turns.length,
        });
        """
    )
    for lap in (result["forward"], result["backward"]):
        assert lap["inner"] == 0
        assert lap["roads"] == 18
        assert lap["back"]
        assert abs(lap["s"] - result["forward"]["s"]) < 1e-6
    assert result["options"] == 2
    assert result["straight"] == 1
    assert result["outward"] == 1
    assert result["inward"] == 2

def ride_through_junction(steer, start_before=45, hold_until=0):
    return run_node(
        f"""
        const g = roads.worldRoadGraph();
        const junction = layout.WORLD_ROAD_JUNCTIONS[2];
        const node = g.nodes.find((n) => Math.hypot(n.x - junction.x, n.z - junction.z) < 1e-6);
        const incoming = g.edges.find((e) => e.to === node.id && e.hexes.includes(0) && e.turns.some((t) => t.hex === 0));
        const route = roads.createRoadRoute(g);
        route.edge = incoming.id;
        route.hex = 0;
        route.next = roads.roadDefaultTurn(incoming, 0);
        route.s = g.straightLength - {start_before};
        const p = {{}};
        roads.roadRoutePoint(route, p);
        let last = {{ ...p }};
        let maxStep = 0;
        let minStep = Infinity;
        let maxBend = 0;
        let pending = 0;
        for (let step = 0; step < 400; step += 1) {{
          const remaining = g.straightLength - route.s;
          const steer = route.edge === incoming.id && remaining > {hold_until} ? {steer} : 0;
          roads.roadRouteAdvance(route, 0.5, steer);
          if (route.edge === incoming.id && route.s < g.straightLength) pending = roads.roadRouteTurnPending(route);
          roads.roadRoutePoint(route, p);
          const moved = Math.hypot(p.x - last.x, p.z - last.z);
          maxStep = Math.max(maxStep, moved);
          minStep = Math.min(minStep, moved);
          maxBend = Math.max(maxBend, Math.abs(Math.atan2(Math.sin(p.heading - last.heading), Math.cos(p.heading - last.heading))));
          last = {{ ...p }};
        }}
        const outgoing = g.edges[route.edge];
        const cross = incoming.ux * outgoing.uz - incoming.uz * outgoing.ux;
        const outward = Math.hypot(g.nodes[outgoing.to].x - layout.WORLD_BIKE_LANE_CENTER_X, g.nodes[outgoing.to].z - layout.WORLD_BIKE_LANE_CENTER_Z) >
          Math.hypot(node.x - layout.WORLD_BIKE_LANE_CENTER_X, node.z - layout.WORLD_BIKE_LANE_CENTER_Z) + 1;
        out({{
          from: outgoing.from === node.id,
          cross,
          hex: route.hex,
          outward,
          onTown: outgoing.hexes.includes(0),
          pending,
          maxStep,
          minStep,
          maxBend,
          options: incoming.turns.map((t) => t.side).sort(),
        }});
        """
    )


def test_left_right_and_straight_at_a_three_way_junction():
    left = ride_through_junction(-1)
    right = ride_through_junction(1)
    straight = ride_through_junction(0)
    for ride in (left, right, straight):
        assert ride["from"]
        assert ride["options"] == [-1, 1]
        assert 0.499 < ride["minStep"] <= ride["maxStep"] < 0.501
        assert ride["maxBend"] < 0.01
    assert left["cross"] < -0.8
    assert right["cross"] > 0.8
    assert left["pending"] == -1
    assert right["pending"] == 1
    assert left["outward"] and not left["onTown"]
    assert right["onTown"] and not right["outward"]
    assert straight["onTown"] and straight["hex"] == 0
    assert left["hex"] != right["hex"]


def test_the_choice_is_latched_a_short_distance_before_the_junction():
    early = ride_through_junction(-1, start_before=55, hold_until=30)
    too_early = ride_through_junction(-1, start_before=200, hold_until=100)
    assert early["outward"] and early["cross"] < 0
    assert too_early["onTown"] and too_early["hex"] == 0


def test_route_stepping_is_allocation_free_and_steering_reads_the_ride_keys():
    roads = ROADS.read_text(encoding="utf-8")
    advance = roads.split("export function roadRouteAdvance(", 1)[1].split("\nexport function", 1)[0]
    point = roads.split("export function roadRoutePoint(", 1)[1].split("\nexport function", 1)[0]
    for body in (advance, point):
        for allocation in ("new ", "=>", "= {", "= [", "...", ".map(", ".filter(", ".find("):
            assert allocation not in body
    controls = (SCENE / "runtime" / "camera-controls.js").read_text(encoding="utf-8")
    assert "rightInput + legacyRightInput + ctx.touchMovement.x" in controls
    assert "ctx.movementInputState.bikeSteer = bikeSteer;" in controls
    walk = (SCENE / "runtime" / "walk-player.js").read_text(encoding="utf-8")
    assert "input.bikeSteer" in walk
    assert "fns.rideBikeAlongRoad(" in walk


def test_handlebars_are_shared_resources_and_the_arms_reach_the_grips():
    bikes = BIKES.read_text(encoding="utf-8")
    handlebar = bikes.split("function addBikeHandlebar(", 1)[1].split("\n}\n", 1)[0]
    assert "resources.material(THREE" in handlebar
    assert handlebar.count('resources.primitive(THREE, "CylinderGeometry"') == 5
    assert "new THREE.Mesh(geometry, material)" in handlebar
    assert "state.handlebar.rotation.y = state.barYaw;" in bikes
    assert "poseAvatarArms(" in bikes
    script = f"""
      import * as THREE from {json.dumps(THREE.as_uri())};
      import * as bikes from {json.dumps(BIKES.as_uri())};
      import * as avatars from {json.dumps(AVATARS.as_uri())};
      const avatar = new THREE.Group();
      const make = (x) => {{
        const arm = new THREE.Mesh(new THREE.BoxGeometry(0.29, avatars.AVATAR_ARM_LENGTH, 0.32));
        arm.position.set(x, avatars.AVATAR_ARM_Y, 0);
        avatar.add(arm);
        return arm;
      }};
      avatar.userData = {{ leftArm: make(-avatars.AVATAR_ARM_X), rightArm: make(avatars.AVATAR_ARM_X) }};
      const end = (arm, y) => arm.localToWorld(new THREE.Vector3(0, y, 0));
      avatars.poseAvatarArms(avatar, bikes.BIKE_ARM_PITCH, bikes.BIKE_ARM_PITCH);
      avatar.updateMatrixWorld(true);
      const half = avatars.AVATAR_ARM_LENGTH / 2;
      const shoulder = end(avatar.userData.rightArm, half);
      const hand = end(avatar.userData.rightArm, -half);
      const seatY = avatars.seatedAvatarY(bikes.BIKE_SEAT_Y + 0.05);
      avatars.poseAvatarArms(avatar, 0, 0);
      const rest = avatar.userData.rightArm.position.toArray();
      const leftTurn = bikes.bikeSteeringTargets(-1, 0, {{}});
      const rightTurn = bikes.bikeSteeringTargets(1, 0, {{}});
      process.stdout.write(JSON.stringify({{
        shoulder: shoulder.toArray(),
        hand: [hand.x, hand.y + seatY, hand.z],
        grip: [bikes.BIKE_GRIP_X, bikes.BIKE_GRIP_Y, bikes.BIKE_GRIP_Z],
        rest,
        leftTurn,
        rightTurn,
        leftArmOnLeftTurn: bikes.bikeGripArmPitch(leftTurn.barYaw, -1),
        rightArmOnLeftTurn: bikes.bikeGripArmPitch(leftTurn.barYaw, 1),
      }}));
    """
    result = json.loads(
        subprocess.run(
            ["node", "--input-type=module", "-e", script],
            check=True,
            text=True,
            capture_output=True,
        ).stdout
    )
    assert abs(result["shoulder"][1] - (2.08 + 0.625)) < 1e-9
    assert abs(result["shoulder"][2]) < 1e-9
    assert result["hand"][2] < -0.8
    assert abs(result["hand"][1] - result["grip"][1]) < 1e-9
    assert abs(result["hand"][2] - result["grip"][2]) < 1e-9
    assert result["rest"] == [0.73, 2.08, 0]
    assert result["leftTurn"]["barYaw"] > 0 and result["leftTurn"]["lean"] > 0
    assert result["rightTurn"]["barYaw"] < 0 and result["rightTurn"]["lean"] < 0
    assert result["leftArmOnLeftTurn"] < result["rightArmOnLeftTurn"]


def test_bench_press_lies_face_up_under_the_bar():
    rides = (SCENE / "runtime" / "rides.js").read_text(encoding="utf-8")
    bench = rides.split('if (ctx.gymState.active === "bench") {', 1)[1].split("} else if", 1)[0]
    assert 'ctx.player.rotation.order = "YXZ";' in bench
    assert "ctx.player.rotation.set(Math.PI / 2, Math.PI, 0);" in bench
    assert "Math.PI / 2,\n        Math.PI / 2," in bench
    for pose in ("-0.72", "-1.55", "-0.9 +", "-1.2 +"):
        assert pose not in rides
    script = f"""
      import * as THREE from {json.dumps(THREE.as_uri())};
      import * as constants from {json.dumps((SCENE / "gameplay-constants.js").as_uri())};
      import * as avatars from {json.dumps(AVATARS.as_uri())};
      const body = new THREE.Object3D();
      body.rotation.order = "YXZ";
      body.rotation.set(Math.PI / 2, Math.PI, 0);
      body.updateMatrixWorld(true);
      const face = new THREE.Vector3(0, 0, -1).transformDirection(body.matrixWorld);
      const head = new THREE.Vector3(0, 1, 0).transformDirection(body.matrixWorld);
      const [x, y, z] = constants.GYM_BENCH_LIE_POSITION;
      process.stdout.write(JSON.stringify({{
        face: face.toArray(),
        head: head.toArray(),
        shoulderZ: z - avatars.AVATAR_SHOULDER_Y,
        handY: y + avatars.AVATAR_ARM_LENGTH - 0.18,
        top: constants.GYM_BENCH_PRESS_TOP_Y,
      }}));
    """
    result = json.loads(
        subprocess.run(
            ["node", "--input-type=module", "-e", script],
            check=True,
            text=True,
            capture_output=True,
        ).stdout
    )
    assert result["face"][1] > 0.999
    assert result["head"][2] < -0.999
    assert math.isclose(result["shoulderZ"], -1.4 - 0.78, abs_tol=1e-9)
    assert math.isclose(result["handY"], result["top"], abs_tol=1e-9)
