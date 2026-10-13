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

HARNESS = """
  const DT = 1 / 60;
  const SPEED = 14;
  const api = bikes.functions({
    THREE: null,
    ctx: { currentFloorY: 0, bikeLoopLength: movement.roundedHexagonLoopLength() },
    fns: {},
  });
  const makeState = () => {
    const position = { x: 0, y: 0, z: 0, set(x, y, z) { this.x = x; this.y = y; this.z = z; } };
    const state = {
      bike: { position, rotation: { y: 0, z: 0 } },
      route: roads.createRoadRoute(),
      offRoad: false,
      snapArmed: false,
      hold: bikes.createBikeSteerHold(),
      blend: { x: 0, z: 0, heading: 0 },
      steer: 0,
      along: 0,
    };
    return state;
  };
  const wrap = (a) => Math.atan2(Math.sin(a), Math.cos(a));
  const pose = (state) => ({ x: state.bike.position.x, z: state.bike.position.z, heading: state.bike.rotation.y });
  const centreDistance = (state) => {
    const p = {};
    roads.roadRoutePoint(state.route, p);
    return Math.hypot(p.x - state.bike.position.x, p.z - state.bike.position.z);
  };
  const graph = roads.worldRoadGraph();
  const townEdgeInto = (junctionIndex) => {
    const junction = layout.WORLD_ROAD_JUNCTIONS[junctionIndex];
    const node = graph.nodes.find((n) => Math.hypot(n.x - junction.x, n.z - junction.z) < 1e-6);
    return graph.edges.find((e) => e.to === node.id && e.hexes.includes(0) && e.turns.some((t) => t.hex === 0));
  };
  const placeOnEdge = (state, edge, s) => {
    state.route.edge = edge.id;
    state.route.hex = 0;
    state.route.next = roads.roadDefaultTurn(edge, 0);
    state.route.s = s;
    state.route.choice = 0;
    api.rideBikeAlongRoad(state, 0, 0, 0);
  };
"""


def run_node(body):
    script = f"""
      import * as roads from {json.dumps(ROADS.as_uri())};
      import * as bikes from {json.dumps(BIKES.as_uri())};
      import * as movement from {json.dumps(MOVEMENT.as_uri())};
      import * as layout from {json.dumps(LAYOUT.as_uri())};
      const out = (value) => process.stdout.write(JSON.stringify(value));
      {HARNESS}
      {body}
    """
    result = subprocess.run(
        ["node", "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(result.stdout)


def test_long_press_leaves_but_a_tap_does_not():
    result = run_node(
        """
        const hold = bikes.createBikeSteerHold();
        const press = (steer, seconds) => {
          let left = false;
          let at = -1;
          for (let t = 0; t < seconds - 1e-9; t += DT) {
            if (bikes.bikeSteerHold(hold, steer, DT) && at < 0) { left = true; at = t + DT; }
          }
          bikes.bikeSteerHold(hold, 0, DT);
          return { left, at };
        };
        const tap = press(-1, 0.25);
        const long = press(1, 0.6);
        const soft = press(0.4, 2);
        const stick = press(-0.8, 0.6);
        hold.steerArmed = false;
        let disarmed = false;
        for (let t = 0; t < 1; t += DT) disarmed = disarmed || bikes.bikeSteerHold(hold, 1, DT);
        bikes.bikeSteerHold(hold, 0, DT);
        const rearmed = press(1, 0.6);
        let flip = false;
        for (let t = 0; t < 0.3; t += DT) flip = flip || bikes.bikeSteerHold(hold, 1, DT);
        for (let t = 0; t < 0.3; t += DT) flip = flip || bikes.bikeSteerHold(hold, -1, DT);
        out({ tap, long, soft, stick, disarmed, rearmed, flip, holdSeconds: bikes.BIKE_LEAVE_ROAD_HOLD_SECONDS });
        """
    )
    assert 0.4 <= result["holdSeconds"] <= 0.5
    assert not result["tap"]["left"]
    assert result["long"]["left"]
    assert abs(result["long"]["at"] - result["holdSeconds"]) < 1 / 30
    assert not result["soft"]["left"]
    assert result["stick"]["left"]
    assert not result["disarmed"]
    assert result["rearmed"]["left"]
    assert not result["flip"]


def test_a_tap_on_the_road_keeps_the_route_and_a_long_press_leaves_it():
    result = run_node(
        """
        const ride = (steer, seconds) => {
          const state = makeState();
          api.placeBikeOnLane(state, 900);
          for (let t = 0; t < seconds; t += DT) api.rideBikeAlongRoad(state, SPEED * DT, steer, DT);
          for (let t = 0; t < 1; t += DT) api.rideBikeAlongRoad(state, SPEED * DT, 0, DT);
          return { offRoad: state.offRoad, off: centreDistance(state) };
        };
        out({ tap: ride(1, 0.25), long: ride(1, 0.7) });
        """
    )
    assert not result["tap"]["offRoad"]
    assert result["tap"]["off"] < 1e-9
    assert result["long"]["offRoad"]
    assert result["long"]["off"] > 3


def test_off_road_steering_integrates_heading_and_position():
    result = run_node(
        """
        const p = { x: 10, z: 20, heading: 0 };
        bikes.bikeOffRoadStep(p, 0, 5, DT);
        const straight = { ...p };
        const circle = { x: 0, z: 0, heading: 0.3 };
        const turnTime = (2 * Math.PI) / bikes.BIKE_OFFROAD_TURN_RATE;
        const steps = Math.round(turnTime / DT);
        let minHeading = 0;
        let farthest = 0;
        for (let i = 0; i < steps; i += 1) {
          bikes.bikeOffRoadStep(circle, 1, SPEED * turnTime / steps, turnTime / steps);
          farthest = Math.max(farthest, Math.hypot(circle.x, circle.z));
        }
        const right = { x: 0, z: 0, heading: 0 };
        bikes.bikeOffRoadStep(right, 1, 1, 0.1);
        const left = { x: 0, z: 0, heading: 0 };
        bikes.bikeOffRoadStep(left, -1, 1, 0.1);
        const back = { x: 0, z: 0, heading: 0 };
        bikes.bikeOffRoadStep(back, 0, -2, DT);
        out({ straight, circle, farthest, radius: SPEED / bikes.BIKE_OFFROAD_TURN_RATE, right, left, back, rate: bikes.BIKE_OFFROAD_TURN_RATE });
        """
    )
    assert result["straight"] == {"x": 10, "z": 15, "heading": 0}
    assert math.hypot(result["circle"]["x"], result["circle"]["z"]) < 0.5
    assert abs(result["circle"]["heading"] - 0.3) < 1e-6
    assert abs(result["farthest"] - 2 * result["radius"]) < 0.5
    assert math.isclose(result["right"]["heading"], -0.1 * result["rate"])
    assert result["right"]["x"] > 0
    assert result["left"]["x"] < 0
    assert result["back"]["z"] == 2


def test_snap_needs_a_nearby_road_and_a_shallow_angle():
    result = run_node(
        """
        const grid = roads.worldRoadSnapGrid();
        const edge = townEdgeInto(2);
        const node = graph.nodes[edge.from];
        const along = graph.cornerInset + 120;
        const cx = node.x + edge.ux * along;
        const cz = node.z + edge.uz * along;
        const nx = -edge.uz;
        const nz = edge.ux;
        const heading = Math.atan2(-edge.ux, -edge.uz);
        const probe = (offset, turn) => {
          const hit = roads.createRoadSnapHit();
          const found = roads.roadSnapFind(grid, cx + nx * offset, cz + nz * offset, heading + turn, hit);
          return { found, near: hit.near, item: hit.item, s: hit.s, distance: hit.distance };
        };
        const arcTurn = edge.turns[0];
        const arcAngle = arcTurn.startAngle + arcTurn.sweep / 2;
        const sign = arcTurn.sweep > 0 ? 1 : -1;
        const arcHit = roads.createRoadSnapHit();
        const arcFound = roads.roadSnapFind(
          grid,
          arcTurn.centerX + Math.cos(arcAngle) * (graph.cornerRadius + 1),
          arcTurn.centerZ + Math.sin(arcAngle) * (graph.cornerRadius + 1),
          Math.atan2(Math.sin(arcAngle) * sign, -Math.cos(arcAngle) * sign),
          arcHit,
        );
        out({
          edge: edge.id,
          reverse: edge.reverse,
          edges: graph.edges.length,
          aligned: probe(2, 0),
          fortyFive: probe(-2.5, (45 * Math.PI) / 180),
          steep: probe(1, (60 * Math.PI) / 180),
          crossing: probe(0, Math.PI / 2),
          far: probe(8, 0),
          backwards: probe(1, Math.PI),
          arc: { found: arcFound, item: arcHit.item, turn: arcTurn.id, s: arcHit.s, straight: graph.straightLength, piece: graph.pieceLength },
          snapDistance: roads.ROAD_SNAP_DISTANCE,
          snapAngle: roads.ROAD_SNAP_ANGLE,
        });
        """
    )
    assert result["snapDistance"] == 3
    assert math.isclose(result["snapAngle"], math.radians(50))
    aligned = result["aligned"]
    assert aligned["found"] and aligned["item"] == result["edge"]
    assert abs(aligned["s"] - 120) < 1e-6 and abs(aligned["distance"] - 2) < 1e-6
    assert result["fortyFive"]["found"] and result["fortyFive"]["item"] == result["edge"]
    assert not result["steep"]["found"] and result["steep"]["near"]
    assert not result["crossing"]["found"] and result["crossing"]["near"]
    assert not result["far"]["found"] and not result["far"]["near"]
    assert result["backwards"]["found"] and result["backwards"]["item"] == result["reverse"]
    arc = result["arc"]
    assert arc["found"] and arc["item"] == result["edges"] + arc["turn"]
    assert arc["straight"] < arc["s"] < arc["piece"]


def test_snap_grid_matches_a_brute_force_search():
    result = run_node(
        """
        const grid = roads.worldRoadSnapGrid();
        const brute = roads.createRoadSnapGrid(graph, 1e5);
        let seed = 7;
        const rand = () => { seed = (seed * 16807) % 2147483647; return seed / 2147483647; };
        let checked = 0;
        let mismatches = 0;
        let hits = 0;
        const a = roads.createRoadSnapHit();
        const b = roads.createRoadSnapHit();
        for (let i = 0; i < 4000; i += 1) {
          const e = graph.edges[Math.floor(rand() * graph.edges.length)];
          const n = graph.nodes[e.from];
          const t = rand() * (e.length + 60) - 30;
          const off = (rand() - 0.5) * 10;
          const x = n.x + e.ux * t - e.uz * off;
          const z = n.z + e.uz * t + e.ux * off;
          const h = rand() * Math.PI * 2 - Math.PI;
          const fa = roads.roadSnapFind(grid, x, z, h, a);
          const fb = roads.roadSnapFind(brute, x, z, h, b);
          checked += 1;
          if (fa) hits += 1;
          if (fa !== fb || a.near !== b.near || (fa && Math.abs(a.distance - b.distance) > 1e-9)) mismatches += 1;
        }
        out({ checked, mismatches, hits, cells: grid.cols * grid.rows, bruteCells: brute.cols * brute.rows });
        """
    )
    assert result["bruteCells"] == 1
    assert result["mismatches"] == 0
    assert result["hits"] > 200


def ride_back_onto_the_road(approach_degrees, lateral=18):
    return run_node(
        f"""
        const state = makeState();
        const edge = townEdgeInto(2);
        placeOnEdge(state, edge, 20);
        for (let t = 0; t < 0.6; t += DT) api.rideBikeAlongRoad(state, SPEED * DT, 1, DT);
        const left = state.offRoad;
        const node = graph.nodes[edge.from];
        const nx = -edge.uz;
        const nz = edge.ux;
        const along = graph.cornerInset + 60;
        const side = Math.sign((state.bike.position.x - node.x) * nx + (state.bike.position.z - node.z) * nz) || 1;
        const approach = ({approach_degrees} * Math.PI) / 180;
        const dirX = edge.ux * Math.cos(approach) - nx * side * Math.sin(approach);
        const dirZ = edge.uz * Math.cos(approach) - nz * side * Math.sin(approach);
        state.bike.position.x = node.x + edge.ux * along + nx * side * {lateral};
        state.bike.position.z = node.z + edge.uz * along + nz * side * {lateral};
        state.bike.rotation.y = Math.atan2(-dirX, -dirZ);
        let last = pose(state);
        let snapFrame = -1;
        let snapJump = 0;
        let snapTurn = 0;
        let maxJump = 0;
        let maxTurn = 0;
        let crossed = false;
        for (let frame = 0; frame < 600; frame += 1) {{
          const wasOff = state.offRoad;
          api.rideBikeAlongRoad(state, SPEED * DT, 0, DT);
          const now = pose(state);
          const jump = Math.hypot(now.x - last.x, now.z - last.z);
          const turn = Math.abs(wrap(now.heading - last.heading));
          maxJump = Math.max(maxJump, jump);
          maxTurn = Math.max(maxTurn, turn);
          if (wasOff && !state.offRoad && snapFrame < 0) {{
            snapFrame = frame;
            snapJump = jump;
            snapTurn = turn;
          }}
          const signed = (now.x - node.x) * nx + (now.z - node.z) * nz;
          if (Math.sign(signed) === -side && Math.abs(signed) > 5) crossed = true;
          last = now;
          if (snapFrame >= 0 && frame > snapFrame + 180) break;
        }}
        out({{
          left,
          snapped: snapFrame >= 0,
          snapJump,
          snapTurn,
          maxJump,
          maxTurn,
          crossed,
          offRoad: state.offRoad,
          blend: state.blend,
          centre: centreDistance(state),
          alongEdge: state.route.edge === edge.id || graph.edges[state.route.edge].from === edge.to,
          step: SPEED * DT,
          blendStep: roads.ROAD_SNAP_DISTANCE * (1 - Math.exp(-bikes.BIKE_SNAP_BLEND_RATE * DT)),
        }});
        """
    )


def test_an_aligned_approach_snaps_back_without_a_pop():
    ride = ride_back_onto_the_road(30)
    assert ride["left"]
    assert ride["snapped"]
    assert not ride["offRoad"]
    assert ride["snapJump"] <= ride["step"] + 1e-9
    assert ride["snapTurn"] < 1e-9
    assert ride["maxJump"] < ride["step"] + ride["blendStep"] + 1e-6
    assert ride["blendStep"] < ride["step"]
    assert ride["maxTurn"] < 0.2
    assert ride["centre"] < 0.01
    assert ride["blend"] == {"x": 0, "z": 0, "heading": 0}
    assert ride["alongEdge"]
    assert not ride["crossed"]


def test_a_steep_crossing_does_not_snap():
    ride = ride_back_onto_the_road(70, lateral=10)
    assert ride["left"]
    assert not ride["snapped"]
    assert ride["offRoad"]
    assert ride["crossed"]


def test_riding_parallel_but_far_from_the_road_does_not_snap():
    ride = ride_back_onto_the_road(0, lateral=8)
    assert not ride["snapped"]
    assert ride["offRoad"]


def test_junction_steering_works_after_rejoining():
    result = run_node(
        """
        const ride = (steer) => {
          const state = makeState();
          const edge = townEdgeInto(2);
          placeOnEdge(state, edge, 10);
          for (let t = 0; t < 0.6; t += DT) api.rideBikeAlongRoad(state, SPEED * DT, -1, DT);
          const left = state.offRoad;
          for (let t = 0; t < 1.5; t += DT) api.rideBikeAlongRoad(state, SPEED * DT, 0, DT);
          const node = graph.nodes[edge.from];
          const along = graph.cornerInset + 150;
          state.bike.position.x = node.x + edge.ux * along - edge.uz * 1.5;
          state.bike.position.z = node.z + edge.uz * along + edge.ux * 1.5;
          state.bike.rotation.y = Math.atan2(-edge.ux, -edge.uz) + 0.3;
          let frames = 0;
          while (state.offRoad && frames < 600) { api.rideBikeAlongRoad(state, SPEED * DT, 0, DT); frames += 1; }
          const rejoined = !state.offRoad && state.route.edge === edge.id;
          while (graph.straightLength - state.route.s > 40) api.rideBikeAlongRoad(state, SPEED * DT, 0, DT);
          for (let t = 0; t < 0.2; t += DT) api.rideBikeAlongRoad(state, SPEED * DT, steer, DT);
          const pending = roads.roadRouteTurnPending(state.route);
          for (let i = 0; i < 1200 && state.route.edge === edge.id; i += 1) api.rideBikeAlongRoad(state, SPEED * DT, 0, DT);
          const outgoing = graph.edges[state.route.edge];
          return {
            left,
            rejoined,
            pending,
            offRoad: state.offRoad,
            cross: edge.ux * outgoing.uz - edge.uz * outgoing.ux,
            onTown: outgoing.hexes.includes(0),
          };
        };
        out({ left: ride(-1), right: ride(1), none: ride(0) });
        """
    )
    for ride in result.values():
        assert ride["left"]
        assert ride["rejoined"]
        assert not ride["offRoad"]
    assert result["left"]["pending"] == -1 and result["left"]["cross"] < -0.8
    assert not result["left"]["onTown"]
    assert result["right"]["pending"] == 1 and result["right"]["cross"] > 0.8
    assert result["right"]["onTown"]
    assert result["none"]["onTown"]


def test_off_road_cue_follows_the_steering():
    result = run_node(
        """
        const state = makeState();
        state.offRoad = true;
        state.steer = 0.5;
        const half = bikes.bikeSteeringTargets(bikes.bikeCueSide(state), 0, {});
        state.steer = -3;
        const full = bikes.bikeSteeringTargets(bikes.bikeCueSide(state), 0, {});
        out({ half, full, bar: bikes.BIKE_TURN_BAR_YAW, lean: bikes.BIKE_TURN_LEAN });
        """
    )
    assert math.isclose(result["half"]["barYaw"], -0.5 * result["bar"])
    assert math.isclose(result["half"]["lean"], -0.5 * result["lean"])
    assert math.isclose(result["full"]["barYaw"], result["bar"])
    assert math.isclose(result["full"]["lean"], result["lean"])


def test_off_road_frame_path_is_allocation_free_and_wired_to_the_walk_collision():
    roads = ROADS.read_text(encoding="utf-8")
    bikes = BIKES.read_text(encoding="utf-8")

    def body(source, name):
        return source.split(f"function {name}(", 1)[1].split("\n}\n", 1)[0].split("\n  }\n", 1)[0]

    for source, name in (
        (roads, "roadSnapFind"),
        (roads, "roadRouteSnap"),
        (bikes, "bikeSteerHold"),
        (bikes, "bikeOffRoadStep"),
        (bikes, "bikeSnapBlendStart"),
        (bikes, "bikeSnapBlendDecay"),
        (bikes, "rideBikeOffRoad"),
        (bikes, "rideBikeAlongRoad"),
        (bikes, "setBikeOnRoute"),
    ):
        code = body(source, name)
        for allocation in ("new ", "=>", "= {", "= [", "...", ".map(", ".filter(", ".find("):
            assert allocation not in code, (name, allocation)
    assert "const snapHit = createRoadSnapHit();" in bikes
    assert "worldRoadSnapGrid()" in bikes
    walk = (SCENE / "runtime" / "walk-player.js").read_text(encoding="utf-8")
    ride = walk.split("if (ctx.bikeRide) {", 1)[1].split("} else if (movement.lengthSq())", 1)[0]
    assert "input.bikeSteer,\n          delta," in ride
    assert "ctx.player.position.x = state.bike.position.x;" in ride
    after = walk.split("} else if (movement.lengthSq())", 1)[1]
    assert after.index("ctx.activeWalkSurfaceContains(") < after.index("fns.applyBikeRidePose(walking, delta);")
    assert after.index("fns.constrainTownOfficeWalls(") < after.index("fns.applyBikeRidePose(walking, delta);")
    assert "state.bike.position.x = ctx.player.position.x;" in bikes
