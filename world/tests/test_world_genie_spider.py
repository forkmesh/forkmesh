import functools
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORLD = ROOT / "world" / "public" / "world"
WORLD_JS = (WORLD / "world.js").read_text(encoding="utf-8")
CORE = (WORLD / "scene" / "core.js").read_text(encoding="utf-8")
ANIMATE = (WORLD / "scene" / "runtime" / "animate.js").read_text(encoding="utf-8")
RUNTIME = (WORLD / "scene" / "runtime" / "genie-spider.js").read_text(encoding="utf-8")

HARNESS = r'''
import { pathToFileURL } from "node:url";
const base = process.argv[1];
const g = await import(pathToFileURL(base + "/scene/genie-spider.js").href);
const out = {};
const acts = g.GENIE_SPIDER_ACTS;
out.count = acts.length;
out.ids = new Set(acts.map((a) => a.id)).size;
out.titles = new Set(acts.map((a) => a.title)).size;
out.motions = [...new Set(acts.map((a) => a.motion))].filter((name) => !g.GENIE_MOTIONS[name]);
out.kinds = [...new Set(acts.flatMap((a) => a.on))].filter((kind) => !g.GENIE_TARGET_KINDS.includes(kind));
out.perKind = Object.fromEntries(g.GENIE_TARGET_KINDS.map((k) => [k, acts.filter((a) => a.on.includes(k)).length]));
out.distinctMotions = new Set(acts.map((a) => a.motion)).size;
const bad = [];
for (const a of acts) {
  for (let i = 0; i <= 24; i += 1) {
    const p = g.genieMotionPose(a, i / 24, i * 0.25, { radius: 1.2, height: 3, reach: 2.5 });
    for (const key of ["x", "y", "z", "yaw", "pitch", "roll", "scale", "stretch", "crouch"]) if (!Number.isFinite(p[key])) bad.push(`${a.id}.${key}`);
    if (!(p.eyes in g.GENIE_EYES)) bad.push(`${a.id}.eyes`);
    if (a.fx !== "none") for (let k = 0; k < 18; k += 1) {
      const q = g.genieParticle(a.fx, k, 18, i / 24, i * 0.25, 2.5);
      if (q && q.some((v) => !Number.isFinite(v))) bad.push(`${a.id}.fx`);
    }
  }
}
out.bad = [...new Set(bad)];
const climb = g.genieActById("head-throne");
out.climbTop = g.genieMotionPose(climb, 0.5, 3, { radius: 1.1, height: 4.9, reach: 3 }).y;
out.climbStart = g.genieMotionPose(climb, 0, 0, { radius: 1.1, height: 4.9, reach: 3 }).z;
out.dangleUp = g.genieMotionPose(g.genieActById("web-yo-yo"), 0.5, 2, {}).y;
out.legs = ["stand", "walk", "dance", "tuck", "spread", "wave", "drum", "punch"].map((mode) => g.genieLegTargets(mode, { phase: 1, time: 2 }).length);
out.hips = g.genieLegHips().length;
const bag = [];
const drawn = new Set();
for (let i = 0; i < 100; i += 1) drawn.add(g.drawGenieAct(bag, g.GENIE_TARGET_KINDS, () => 0.5).id);
out.drawnAll = drawn.size;
const groundBag = [];
out.groundOnly = Array.from({ length: 40 }, () => g.drawGenieAct(groundBag, [], () => 0.3)).every((a) => a.on.includes("ground"));
out.targetKind = [g.genieActTargetKind(g.genieActById("splash"), ["fountain", "player"]), g.genieActTargetKind(g.genieActById("web-wrap"), ["board"]), g.genieActTargetKind(g.genieActById("disco"), ["player"])];
out.reactions = Object.values(g.GENIE_REACTION_ACTS).filter((id) => !g.genieActById(id));
out.allowed = [
  g.genieSpiderAllowed({}),
  g.genieSpiderAllowed({ cookie: "a=1; forkmesh_genie=off" }),
  g.genieSpiderAllowed({ cookie: "forkmesh_genie=on", offFlag: "1" }),
  g.genieSpiderAllowed({ offFlag: "1" }),
  g.genieSpiderAllowed({ worldFlag: "off" }),
  g.genieSpiderAllowed({ cookie: "forkmesh_genie=off", worldFlag: "on" }),
];
console.log(JSON.stringify(out));
'''


@functools.cache
def _run():
    done = subprocess.run(["node", "--input-type=module", "-e", HARNESS, str(WORLD)], capture_output=True, text=True)
    assert done.returncode == 0, done.stderr[-2000:]
    return json.loads(done.stdout)


def test_the_spider_knows_one_hundred_distinct_acts():
    data = _run()
    assert data["count"] == 100
    assert data["ids"] == 100 and data["titles"] == 100
    assert data["motions"] == [] and data["kinds"] == []
    assert data["distinctMotions"] >= 35
    assert data["bad"] == []


def test_acts_interact_with_many_kinds_of_things_in_the_world():
    per_kind = _run()["perKind"]
    assert per_kind["ground"] >= 25 and per_kind["player"] >= 20 and per_kind["avatar"] >= 8
    for kind in ("bot", "fountain", "campfire", "bike", "car", "drone", "swing", "portal", "landmark", "board", "thing"):
        assert per_kind[kind] >= 2, kind


def test_motions_climb_dangle_and_move_all_eight_legs():
    data = _run()
    assert abs(data["climbTop"] - 4.9) < 1e-6
    assert data["climbStart"] == -3
    assert data["dangleUp"] > 4
    assert data["legs"] == [8] * 8 and data["hips"] == 8


def test_acts_come_from_a_shuffled_bag_that_fits_what_is_nearby():
    data = _run()
    assert data["drawnAll"] == 100
    assert data["groundOnly"] is True
    assert data["targetKind"] == ["fountain", "board", "ground"]
    assert data["reactions"] == []


def test_the_spider_honours_the_site_wide_hide_and_the_world_switch():
    assert _run()["allowed"] == [True, False, True, False, False, True]
    assert 'data-world-genie-spider' in WORLD_JS
    assert 'const WORLD_GENIE_SPIDER_KEY = "forkmesh.world.genie";' in WORLD_JS
    assert "this.world?.setGenieSpiderEnabled?.(genieSpider.checked);" in WORLD_JS


def test_the_spider_is_registered_with_the_scene_and_animated_every_frame():
    assert 'import * as genieSpiderRuntime from "./runtime/genie-spider.js";' in CORE
    for api in ("playGenieSpiderAct,", "reactGenieSpider,", "setGenieSpiderEnabled,", "genieSpiderState,", "genieSpiderActs,"):
        assert CORE.count(api) == 2, api
    assert "fns.updateGenieSpider?.(delta, time);" in ANIMATE
    assert "this.world?.reactGenieSpider?.(emoji);" in WORLD_JS


def test_the_spider_stays_cheap_to_draw():
    model = (WORLD / "scene" / "genie-spider.js").read_text(encoding="utf-8")
    assert model.count("new THREE.InstancedMesh(") == 3
    assert "const PARTICLES = 18;" in RUNTIME
    assert 'if (ctx.officeSceneMode && ctx.officeSceneMode !== "town") return false;' in RUNTIME
    assert "if (!reducedMotion && s.mode === \"idle\" && now >= s.nextActAt) chooseAct(now);" in RUNTIME
