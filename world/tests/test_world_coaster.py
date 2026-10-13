import json
import shutil
import subprocess
from pathlib import Path

import pytest

WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = WORLD / "scene"
TRACK = SCENE / "coaster.js"
COURSE = SCENE / "districts" / "quadcopters.js"
CONSTANTS = SCENE / "gameplay-constants.js"
NODE = shutil.which("node")

pytestmark = pytest.mark.skipif(NODE is None, reason="node is required")


def run_node(body):
    script = """
      const coaster = await import(%s);
      const course = await import(%s);
      const constants = await import(%s);
      const track = coaster.coasterTrack();
      const out = {};
      %s
      console.log(JSON.stringify(out));
    """ % (
        json.dumps(TRACK.as_uri()),
        json.dumps(COURSE.as_uri()),
        json.dumps(CONSTANTS.as_uri()),
        body,
    )
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True, capture_output=True, text=True, timeout=120,
    )
    return json.loads(result.stdout)


def test_the_track_circles_the_whole_town_hexagon_and_closes():
    out = run_node("""
      const cx = 0, cz = -45;
      let winding = 0;
      let previous = Math.atan2(track.z[0] - cz, track.x[0] - cx);
      let minHex = Infinity, maxHex = 0, minY = Infinity, gap = 0;
      for (let i = 1; i < track.count; i += 1) {
        const angle = Math.atan2(track.z[i] - cz, track.x[i] - cx);
        let turn = angle - previous;
        if (turn > Math.PI) turn -= Math.PI * 2;
        if (turn < -Math.PI) turn += Math.PI * 2;
        winding += turn;
        previous = angle;
        let hex = 0;
        for (let k = 0; k < 6; k += 1) {
          const a = k * Math.PI / 3;
          hex = Math.max(hex, (track.x[i] - cx) * Math.cos(a) + (track.z[i] - cz) * Math.sin(a));
        }
        minHex = Math.min(minHex, hex);
        maxHex = Math.max(maxHex, hex);
        minY = Math.min(minY, track.y[i]);
        gap = Math.max(gap, Math.hypot(track.x[i] - track.x[i - 1], track.y[i] - track.y[i - 1], track.z[i] - track.z[i - 1]));
      }
      const last = track.count - 1;
      out.turns = winding / (Math.PI * 2);
      out.minHex = minHex;
      out.maxHex = maxHex;
      out.minY = minY;
      out.gap = gap;
      out.closure = Math.hypot(track.x[0] - track.x[last], track.y[0] - track.y[last], track.z[0] - track.z[last]);
      out.length = track.length;
      out.plan = coaster.COASTER_PLAN_LENGTH;
    """)
    assert abs(out["turns"]) == pytest.approx(1, abs=0.01)
    assert out["minHex"] > 260
    assert out["maxHex"] < 345
    assert out["minY"] >= 1
    assert out["gap"] < 0.6
    assert out["closure"] < 1
    assert out["length"] > out["plan"]


def test_the_double_loop_corkscrew_and_helix_turn_riders_over():
    out = run_node("""
      const loops = track.sections.filter((s) => s.inversion && s.f0 === s.f1);
      const corkscrew = track.sections.find((s) => s.mark === "Corkscrew");
      const helix = track.sections.find((s) => s.mark === "Helix");
      const lowestUp = (section) => {
        let up = Infinity, top = 0;
        for (let i = Math.ceil(section.start / track.step); i * track.step <= section.end; i += 1) {
          up = Math.min(up, track.uy[i]);
          top = Math.max(top, track.y[i]);
        }
        return { up, top };
      };
      out.loops = loops.map(lowestUp);
      out.loopGap = loops.length === 2 ? loops[1].f0 - loops[0].f0 : 0;
      out.corkscrew = lowestUp(corkscrew);
      out.helix = lowestUp(helix);
      out.marks = track.marks.map((m) => m.name);
    """)
    assert len(out["loops"]) == 2
    assert out["loopGap"] <= 60
    for loop in out["loops"]:
        assert loop["up"] < -0.95
        assert loop["top"] >= 30
    assert out["corkscrew"]["up"] < -0.95
    assert out["helix"]["up"] > 0.3
    assert out["marks"] == [
        "Lift hill", "First drop", "Double loop", "Banked turn",
        "Airtime hills", "Ramp jump", "Freefall", "Corkscrew", "Helix", "Tunnel",
    ]


def test_the_ramp_jump_is_a_ballistic_freefall_onto_a_matching_ramp():
    out = run_node("""
      const flight = track.sections.find((s) => !s.rails);
      const index = track.sections.indexOf(flight);
      const kicker = track.sections[index - 1];
      const landing = track.sections[index + 1];
      const g = constants.COASTER_GRAVITY;
      let worst = 0;
      const first = Math.ceil(flight.start / track.step);
      const startSpeed = track.speed[first];
      for (let i = first; i * track.step <= flight.end; i += 1) {
        const expected = Math.sqrt(startSpeed ** 2 + 2 * g * (track.y[first] - track.y[i]));
        worst = Math.max(worst, Math.abs(track.speed[i] - expected));
      }
      const before = Math.floor(flight.end / track.step) - 2;
      const after = Math.ceil(flight.end / track.step) + 2;
      out.kink = track.tx[before] * track.tx[after] + track.ty[before] * track.ty[after] + track.tz[before] * track.tz[after];
      out.speedError = worst;
      out.flightTime = flight.flightTime;
      out.apex = flight.apex;
      out.lipY = track.y[first];
      out.landY = track.y[Math.floor(flight.end / track.step)];
      out.span = flight.f1 - flight.f0;
      out.kickerRails = kicker.rails;
      out.landingRails = landing.rails && landing.landing;
      let roadClear = Infinity;
      for (let i = first; i * track.step <= flight.end; i += 1) {
        if (Math.abs(track.z[i] - (-80)) < 14) roadClear = Math.min(roadClear, track.y[i]);
      }
      out.roadClear = roadClear;
    """)
    assert out["kickerRails"] and out["landingRails"]
    assert out["speedError"] < 0.5
    assert out["flightTime"] > 1.5
    assert out["span"] > 60
    assert out["apex"] > out["lipY"] + 5
    assert out["landY"] < out["lipY"] - 5
    assert out["kink"] > 0.99
    assert out["roadClear"] > 10


def test_speeds_follow_energy_from_the_lift_and_a_lap_stays_short():
    out = run_node("""
      let maxFree = 0, minSpeed = Infinity, lap = 0;
      for (let i = 0; i < track.count; i += 1) {
        const section = track.sections[track.piece[i]];
        if (section.mode === "free") maxFree = Math.max(maxFree, track.y[i]);
        minSpeed = Math.min(minSpeed, track.speed[i]);
        lap += track.step / track.speed[i];
      }
      out.maxFree = maxFree;
      out.minSpeed = minSpeed;
      out.lap = lap;
      out.station = coaster.coasterSample(coaster.coasterStationDistance()).speed;
      out.lift = constants.COASTER_LIFT_HEIGHT;
    """)
    assert out["maxFree"] <= out["lift"]
    assert out["minSpeed"] >= 6.9
    assert 60 < out["lap"] < 95
    assert out["station"] == 7


def test_drone_rings_weave_over_and_under_the_track():
    out = run_node("""
      const { rings, start } = course.droneCourseGates();
      out.rings = rings.map((ring) => {
        let best = Infinity, index = 0;
        for (let i = 0; i < track.count; i += 1) {
          const d = Math.hypot(track.x[i] - ring.center[0], track.z[i] - ring.center[2]);
          if (d < best) { best = d; index = i; }
        }
        let clearance = Infinity;
        for (let i = 0; i < track.count; i += 1) {
          clearance = Math.min(clearance, Math.hypot(
            track.x[i] - ring.center[0], track.y[i] - ring.center[1], track.z[i] - ring.center[2]));
        }
        return {
          side: coaster.droneCourseRingSide(ring.index),
          plan: best,
          above: ring.center[1] - track.y[index],
          clearance,
          bottom: ring.center[1] - ring.radius,
        };
      });
      let startNearest = Infinity;
      for (let i = 0; i < track.count; i += 1) {
        startNearest = Math.min(startNearest, Math.hypot(
          track.x[i] - start.center[0], track.y[i] - start.center[1], track.z[i] - start.center[2]));
      }
      out.start = { nearest: startNearest, bottom: start.center[1] - 11.7 };
    """)
    rings = out["rings"]
    assert len(rings) == 10
    assert sum(1 for ring in rings if ring["side"] < 0) >= 4
    for ring in rings:
        assert ring["plan"] < 1.5
        assert ring["clearance"] > 9
        assert ring["bottom"] > 2
        if ring["side"] > 0:
            assert ring["above"] > 10
        else:
            assert ring["above"] < -10
    assert out["start"]["nearest"] > 12
    assert out["start"]["bottom"] > 2


def test_the_ride_is_wired_into_movement_input_and_the_scene():
    walk = (SCENE / "runtime" / "walk-player.js").read_text()
    pointer = (SCENE / "runtime" / "pointer.js").read_text()
    core = (SCENE / "core.js").read_text()
    assert walk.index("fns.updateCoasterRide(") < walk.index("fns.updateJetpackFlight(")
    assert "fns.toggleNearestCoasterRide() ||" in pointer
    assert "hit?.object?.userData?.coasterTrain" in pointer
    assert "await coasterDistrict.build({" in core
    assert "getCoasterState: () => fns.coasterState()" in core
    rides = (SCENE / "runtime" / "rides.js").read_text()
    assert rides.count('ctx.officeSceneMode !== "town" || ctx.coasterRide') == 3
