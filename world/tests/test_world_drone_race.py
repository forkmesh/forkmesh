import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = WORLD / "scene"
RULES = SCENE / "drone-race.js"
RUNTIME = SCENE / "runtime" / "drone-race.js"
COURSE = SCENE / "districts" / "quadcopters.js"
MOVEMENT = SCENE / "movement.js"
CONSTANTS = SCENE / "gameplay-constants.js"
WORLD_JS = WORLD / "world.js"
NODE = shutil.which("node")

pytestmark = pytest.mark.skipif(NODE is None, reason="node is required")


def run_node(body):
    script = """
      const rules = await import(%s);
      const course = await import(%s);
      const movement = await import(%s);
      const constants = await import(%s);
      const out = {};
      %s
      console.log(JSON.stringify(out));
    """ % (
        json.dumps(RULES.as_uri()),
        json.dumps(COURSE.as_uri()),
        json.dumps(MOVEMENT.as_uri()),
        json.dumps(CONSTANTS.as_uri()),
        body,
    )
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True, capture_output=True, text=True, timeout=60,
    )
    return json.loads(result.stdout)


GATE_HELPERS = """
  const { rings, start } = course.droneCourseGates();
  const along = (gate, distance, side = 0) => ({
    x: gate.center[0] + gate.normal[0] * distance + side,
    y: gate.center[1] + gate.normal[1] * distance,
    z: gate.center[2] + gate.normal[2] * distance,
  });
"""


def test_a_ring_counts_only_when_crossed_forward_inside_its_radius():
    out = run_node(GATE_HELPERS + """
      const ring = rings[3];
      const lateral = [ring.normal[2], 0, -ring.normal[0]];
      const length = Math.hypot(...lateral);
      const offset = (distance) => ({
        x: lateral[0] / length * distance, z: lateral[2] / length * distance,
      });
      const shifted = (point, distance) => {
        const o = offset(distance);
        return { x: point.x + o.x, y: point.y, z: point.z + o.z };
      };
      out.forward = rules.gateCrossing(ring, along(ring, -2), along(ring, 2));
      out.backward = rules.gateCrossing(ring, along(ring, 2), along(ring, -2));
      out.inside = rules.gateCrossing(
        ring, shifted(along(ring, -2), 7), shifted(along(ring, 2), 7));
      out.outside = rules.gateCrossing(
        ring, shifted(along(ring, -2), 9), shifted(along(ring, 2), 9));
      out.farAbove = rules.gateCrossing(
        ring,
        { ...along(ring, -2), y: ring.center[1] + 30 },
        { ...along(ring, 2), y: ring.center[1] + 30 });
      out.noCross = rules.gateCrossing(ring, along(ring, -6), along(ring, -1));
      out.fastFrame = rules.gateCrossing(ring, along(ring, -60), along(ring, 60));
      out.radius = ring.radius;
      out.ringCount = rings.length;
      out.startRadius = start.radius;
      out.startBetween = start.angle < rings[0].angle
        && start.angle > rings[rings.length - 1].angle - Math.PI * 2;
    """)
    assert out["forward"] == 1
    assert out["backward"] == -1
    assert out["inside"] == 1
    assert out["outside"] == 0
    assert out["farAbove"] == 0
    assert out["noCross"] == 0
    assert out["fastFrame"] == 1
    assert out["radius"] == 8
    assert out["ringCount"] == 10
    assert out["startRadius"] == 9.5
    assert out["startBetween"]


def test_rings_count_in_order_and_wrong_way_or_skipped_rings_do_not():
    out = run_node(GATE_HELPERS + """
      const race = rules.createRaceState(rings.length);
      const events = [];
      const fly = (gate, now, direction = 1) => events.push(
        rules.advanceRace(race, rings, start,
          along(gate, -2 * direction), along(gate, 2 * direction), now));
      fly(rings[0], 0);
      out.beforeStart = { running: race.running, next: race.nextRing };
      fly(start, 1000);
      fly(rings[0], 2000);
      fly(rings[2], 3000);
      out.afterSkip = race.nextRing;
      fly(rings[1], 4000, -1);
      out.afterWrongWay = race.nextRing;
      fly(rings[0], 4500);
      fly(rings[1], 5000);
      out.afterSecond = race.nextRing;
      out.events = events;
    """)
    assert out["beforeStart"] == {"running": False, "next": 0}
    assert out["events"] == [
        "", "start", "ring", "out-of-order", "wrong-way", "out-of-order", "ring",
    ]
    assert out["afterSkip"] == 1
    assert out["afterWrongWay"] == 1
    assert out["afterSecond"] == 2


def test_a_full_lap_along_the_course_times_the_lap_and_keeps_the_best():
    out = run_node(GATE_HELPERS + """
      const race = rules.createRaceState(rings.length);
      let previous = null;
      let now = 10_000;
      let flown = 0;
      const fly = (from, to, speed) => {
        const events = [];
        const step = 0.002;
        for (let angle = from; angle < to; angle += step) {
          const [x, y, z] = course.droneCoursePoint(angle);
          const point = { x, y, z };
          if (previous) {
            const event = rules.advanceRace(race, rings, start, previous, point, now);
            if (event) events.push([event, Math.round(now)]);
            const distance = Math.hypot(x - previous.x, y - previous.y, z - previous.z);
            flown += distance;
            now += (distance / speed) * 1000;
          }
          previous = point;
        }
        return events;
      };
      const lapStart = start.angle - 0.03;
      out.first = fly(lapStart, lapStart + Math.PI * 2 + 0.06, 50);
      out.lapLength = flown * Math.PI * 2 / (Math.PI * 2 + 0.06);
      out.firstLap = race.lastLapMs;
      out.bestAfterFirst = race.bestMs;
      out.newBestFirst = race.newBest;
      out.second = fly(lapStart + Math.PI * 2 + 0.06, lapStart + Math.PI * 4 + 0.06, 40);
      out.secondLap = race.lastLapMs;
      out.bestAfterSecond = race.bestMs;
      out.newBestSecond = race.newBest;
      out.running = race.running;
      out.laps = race.laps;
      const early = rules.createRaceState(rings.length, 30_000);
      rules.advanceRace(early, rings, start, along(start, -2), along(start, 2), 0);
      rules.advanceRace(early, rings, start, along(rings[0], -2), along(rings[0], 2), 500);
      out.restart = rules.advanceRace(
        early, rings, start, along(start, -2), along(start, 2), 900);
      out.restartState = { next: early.nextRing, lapStart: early.lapStart, best: early.bestMs };
      out.format = [rules.formatLapTime(61_234), rules.formatLapTime(9_870)];
      out.presence = rules.racePresence(early, 2900);
      out.remote = rules.remoteRace({ ring: 99, lap: -5, best: 1e12 }, 10);
    """)
    first = [event for event, _ in out["first"]]
    assert first == ["start"] + ["ring"] * 10 + ["finish"]
    start_ms = out["first"][0][1]
    finish_ms = out["first"][-1][1]
    assert abs(out["firstLap"] - (finish_ms - start_ms)) <= 1
    assert out["firstLap"] == pytest.approx(out["lapLength"] / 50 * 1000, rel=0.03)
    assert out["bestAfterFirst"] == out["firstLap"]
    assert out["newBestFirst"] is True
    assert [event for event, _ in out["second"]] == ["ring"] * 10 + ["finish"]
    assert out["secondLap"] > out["firstLap"]
    assert out["bestAfterSecond"] == out["firstLap"]
    assert out["newBestSecond"] is False
    assert out["running"] is True
    assert out["laps"] == 2
    assert out["restart"] == "restart"
    assert out["restartState"] == {"next": 0, "lapStart": 900, "best": 30_000}
    assert out["format"] == ["1:01.23", "0:09.87"]
    assert out["presence"] == {"ring": 0, "lap": 2000, "best": 30_000}
    assert out["remote"] == {"ring": 10, "lap": 0, "best": 0}


def test_the_ghost_track_samples_at_ten_hertz_and_round_trips_through_storage():
    out = run_node("""
      const track = rules.createGhostTrack(50);
      for (let t = 0; t <= 1000; t += 16) {
        rules.recordGhostSample(track, t, t / 100, 60, -t / 50, Math.PI - 0.01);
      }
      out.count = track.count;
      out.step = track.stepMs;
      const pose = {};
      out.inside = rules.sampleGhostTrack(track, 250, pose);
      out.pose = [Math.round(pose.x * 100) / 100, pose.y];
      out.past = rules.sampleGhostTrack(track, 5000, {});
      const saved = JSON.parse(JSON.stringify(rules.ghostTrackRecord(track)));
      const restored = rules.createGhostTrack(50);
      out.loaded = rules.loadGhostTrack(restored, saved);
      out.restoredCount = restored.count;
      out.rejected = rules.loadGhostTrack(rules.createGhostTrack(50), { stepMs: 33, samples: [1, 2, 3, 4] });
      const full = rules.createGhostTrack(5);
      rules.recordGhostSample(full, 10_000, 1, 2, 3, 0);
      out.capped = full.count;
      const turning = rules.createGhostTrack(4);
      rules.recordGhostSample(turning, 0, 0, 0, 0, Math.PI - 0.1);
      turning.samples.set([0, 0, 0, -Math.PI + 0.1], 4);
      turning.count = 2;
      const mid = {};
      rules.sampleGhostTrack(turning, 50, mid);
      out.shortestTurn = Math.abs(Math.abs(mid.yaw) - Math.PI) < 0.01;
      out.hz = 1000 / constants.DRONE_RACE_GHOST_SAMPLE_MS;
    """)
    assert out["step"] == 100
    assert out["hz"] == 10
    assert out["count"] == 10
    assert out["inside"] is True
    assert out["pose"][1] == 60
    assert out["past"] is False
    assert out["loaded"] is True
    assert out["restoredCount"] == 10
    assert out["rejected"] is False
    assert out["capped"] == 5
    assert out["shortestTurn"] is True


def test_the_parachute_sinks_about_twice_as_fast_and_stays_smooth():
    out = run_node("""
      const fall = (sink) => {
        let velocity = -0.4;
        let y = 80;
        let t = 0;
        let largestStep = 0;
        const dt = 1 / 60;
        while (y > 0) {
          const next = sink(velocity, dt);
          largestStep = Math.max(largestStep, Math.abs(next - velocity));
          velocity = next;
          y += velocity * dt;
          t += dt;
        }
        return { t, velocity, largestStep };
      };
      const old = fall((v, dt) => Math.max(-4.2, v - 4.8 * dt));
      const now = fall(movement.parachuteSinkVelocity);
      out.old = old;
      out.now = now;
      out.terminal = constants.ROOF_PARACHUTE_TERMINAL_VELOCITY;
      out.gravity = constants.ROOF_PARACHUTE_GRAVITY;
    """)
    assert out["terminal"] == -8.4
    assert out["gravity"] == 9.6
    assert out["now"]["velocity"] == pytest.approx(-8.4)
    ratio = out["old"]["t"] / out["now"]["t"]
    assert 1.8 <= ratio <= 2.1
    assert out["now"]["largestStep"] <= 9.6 / 60 + 1e-9
    assert out["now"]["t"] > 8


def test_race_hud_moves_with_transforms_and_never_reads_layout():
    runtime = RUNTIME.read_text(encoding="utf-8")
    assert "style.transform" in runtime
    for forbidden in (
        "getBoundingClientRect",
        "offsetWidth",
        "offsetHeight",
        "clientWidth",
        "getComputedStyle",
        "style.left",
        "style.top",
    ):
        assert forbidden not in runtime, forbidden
    update = runtime[runtime.index("function updateDroneRace"):runtime.index("function droneRacePresence")]
    assert "new THREE." not in update
    assert "localStorage" in runtime
    assert runtime.count("try {") >= 2


def test_race_visuals_are_instanced_and_share_materials():
    course = COURSE.read_text(encoding="utf-8")
    assert course.count("new THREE.InstancedMesh(") == 3
    assert 'primitive(THREE, "BoxGeometry", 1, 1, 1)' in course
    assert re.search(r'material\(\s*THREE,\s*\{ color: "#ffffff" \},\s*"MeshBasicMaterial",?\s*\)', course)
    assert "activeRingMaterial" in course


def test_race_state_rides_existing_move_frames():
    world = WORLD_JS.read_text(encoding="utf-8")
    assert "function droneRaceFrame(value)" in world
    send = world[world.index('} else if (message.type === "move") {\n      safe = {'):]
    send = send[:send.index('} else if (message.type === "ping")')]
    assert "droneRaceFrame(message.race)" in send
    assert "race: droneRaceFrame(message.race)" in world
    rides = (SCENE / "runtime" / "rides.js").read_text(encoding="utf-8")
    assert "race: fns.droneRacePresence()" in rides
    remote = (SCENE / "runtime" / "remote-players.js").read_text(encoding="utf-8")
    assert "avatar.userData.race =" in remote
