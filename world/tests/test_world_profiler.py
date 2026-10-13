"""Behaviour of the admin performance bar's profiler, scene audit and debug log."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (WORLD / "world.css").read_text(encoding="utf-8")

NODE = shutil.which("node")

AUDIT_SCRIPT = r"""
import { auditWorldScene, createWorldAuditState, createWorldProfiler, describeWorldCallback } from "./world-profiler.js";
import { createWorldDebugLog, describeLongAnimationFrame } from "./world-debug-log.js";

const windmill = { rotation: { z: 0 } };
const matrix = () => ({ elements: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1], getMaxScaleOnAxis: () => 1 });
const node = (props = {}) => ({ visible: true, children: [], matrixAutoUpdate: true, matrixWorld: matrix(), userData: {}, ...props });
const box = { type: "BoxGeometry", parameters: { width: 1, height: 1, depth: 1 }, attributes: { position: { count: 24, version: 0, array: new Float32Array(72) } }, index: { count: 36 }, boundingSphere: { radius: 0.2 } };
const boxCopies = Array.from({ length: 30 }, () => ({ ...box, attributes: { position: { ...box.attributes.position } } }));
const standard = (props = {}) => ({ type: "MeshStandardMaterial", color: null, opacity: 1, transparent: false, side: 0, visible: true, ...props });
const shared = standard();
const glass = Array.from({ length: 30 }, () => standard({ transparent: true }));
const ghosts = Array.from({ length: 3 }, () => standard({ transparent: true, opacity: 0 }));
const scene = node({ children: [] });
const town = node({ name: "town" });
scene.children.push(town);
for (let i = 0; i < 40; i += 1) town.children.push(node({ isMesh: true, geometry: box, material: shared, castShadow: true, parent: town }));
boxCopies.forEach((geometry, i) => town.children.push(node({ isMesh: true, geometry, material: glass[i], frustumCulled: false, parent: town })));
ghosts.forEach((material) => town.children.push(node({ isMesh: true, geometry: box, material, parent: town })));
const renderer = { info: { memory: { geometries: 40, textures: 0 }, programs: new Array(3) } };
const state = createWorldAuditState();
let clock = 0;
const now = () => clock;
auditWorldScene({ scene, renderer, state, now, ownerOf: () => "Town props" });
clock = 5000;
const audit = auditWorldScene({ scene, renderer, state, now, ownerOf: () => "Town props" });

let time = 0;
const profiler = createWorldProfiler({ renderer: null, camera: "main", now: () => time, ownerOf: () => "Town props" });
const ticks = [];
const tick = () => { time += 3; ticks.push(time); };
profiler.setEnabled(true);
profiler.beginFrame();
time += 2; profiler.mark("housekeeping");
profiler.runCallbacks([tick, function spinWindmill() { windmill.rotation.z += 1; time += 1; }], 0, 0);
profiler.mark("ambient");
profiler.noteDraw("main", scene, box, shared, town.children[0], null);
profiler.noteDraw("shadow-camera", null, box, shared, town.children[0], null);
time += 5; profiler.mark("render");
profiler.endFrame();
const profile = profiler.sample();

const log = createWorldDebugLog({ capacity: 3, now: () => 1 });
["ERROR", "WARN", "LONG", "LONG", "NOPE"].forEach((category) => log.add(category, { detail: category }));
const loaf = describeLongAnimationFrame({ duration: 180, blockingDuration: 130, startTime: 0, scripts: [
  { invoker: "FrameRequestCallback", sourceURL: "https://forkmesh.com/world/world-scene.js", sourceFunctionName: "animate", sourceCharPosition: 1200, duration: 150, forcedStyleAndLayoutDuration: 12 },
] });
console.log(JSON.stringify({
  findings: audit.findings.map((finding) => ({ id: finding.id, level: finding.level, offenders: finding.offenders })),
  stats: audit.stats,
  profile: { frames: profile.frames, cpuMs: profile.cpuMs, phases: Object.fromEntries(profile.phases.map((p) => [p.id, p.avgMs])), passes: profile.passes, draws: profile.draws, callbacks: profile.callbacks.map((c) => c.label), gpu: profile.gpu },
  disabledRuns: (() => { profiler.setEnabled(false); let ran = 0; profiler.runCallbacks([() => { ran += 1; }], 0, 0); return ran; })(),
  log: { counts: log.counts(), total: log.total, kept: log.entries().map((entry) => entry.category) },
  loaf,
  label: describeWorldCallback((time) => {
    const t = time * 0.001;
    windmill.rotation.z = t;
  }),
}));
"""


@pytest.fixture(scope="module")
def result():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", AUDIT_SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(completed.stdout)


def test_audit_names_three_js_anti_patterns_with_offenders(result):
    findings = {finding["id"]: finding for finding in result["findings"]}
    assert findings["instancing"]["level"] == "caution"
    assert findings["instancing"]["offenders"][0].startswith(
        "Town props · BoxGeometry + MeshStandardMaterial: 40 meshes → 1 draw"
    )
    assert findings["duplicate-geometries"]["offenders"][0] == (
        "Town props · BoxGeometry(1, 1, 1) × 31"
    )
    assert "duplicate-materials" in findings
    assert findings["needless-transparency"]["offenders"] == ["Town props × 30"]
    assert findings["culling-off"]["offenders"] == ["Town props × 30"]
    assert findings["invisible-drawn"]["offenders"] == ["Town props × 3"]
    stats = result["stats"]
    assert stats["instancingSavings"] == 39
    assert stats["duplicateGeometries"] == 30
    assert stats["smallShadowCasters"] == 40
    assert stats["staticAutoMatrixObjects"] == stats["autoMatrixObjects"]
    levels = [finding["level"] for finding in result["findings"]]
    order = {"high": 0, "caution": 1, "info": 2}
    assert levels == sorted(levels, key=order.__getitem__)


def test_profiler_splits_frame_phases_callbacks_and_passes(result):
    profile = result["profile"]
    assert profile["frames"] == 1
    assert profile["phases"]["housekeeping"] == 2
    assert profile["phases"]["ambient"] == 4
    assert profile["phases"]["render"] == 5
    assert profile["cpuMs"] == 11
    assert profile["passes"]["main"] == 1
    assert profile["passes"]["shadow"] == 1
    assert profile["draws"][0]["label"] == "Town props"
    assert profile["draws"][0]["triangles"] == 12
    assert "spinWindmill() — windmill.rotation.z += 1; time += 1;" in profile["callbacks"]
    assert profile["gpu"]["available"] is False
    assert result["disabledRuns"] == 1
    assert result["label"] == "windmill.rotation.z = t;"


def test_debug_log_counts_every_category_and_keeps_a_bounded_tail(result):
    log = result["log"]
    assert log["counts"]["ERROR"] == 1
    assert log["counts"]["WARN"] == 1
    assert log["counts"]["LONG"] == 2
    assert log["total"] == 4
    assert log["kept"] == ["WARN", "LONG", "LONG"]
    assert result["loaf"]["text"].startswith("Main thread blocked 180 ms (130 ms blocking)")
    assert (
        "FrameRequestCallback → animate @ world-scene.js:1200 150 ms, forced layout 12 ms"
        in result["loaf"]["text"]
    )


def test_bar_is_admin_gated_and_profiling_follows_it():
    assert "this.identity?.isAdmin === true || this.settings?.debugPanel === true" in APP
    assert "const admin = wanted && this.identity?.isAdmin === true;" in APP
    assert "this.world?.setProfiling?.(admin);" in APP
    assert "observeWorldMainThread(this.debugLog, {" in APP
    assert 'data-world-perf-admin hidden' in APP
    for tab in ("fixes", "budget", "draws", "blockers", "log"):
        assert f'data-world-perf-tab="{tab}"' in APP
    assert "perfReportMarkdown()" in APP


def test_scene_marks_each_frame_phase_and_times_the_gpu_pass():
    for phase in (
        "housekeeping", "frame-animations", "simulation", "avatars", "effects",
        "camera", "proximity", "mirror", "ambient", "labels", "matrices", "render",
    ):
        assert f'worldProfiler.mark("{phase}");' in SCENE
    assert "worldProfiler.runCallbacks(animated, time, visualDelta);" in SCENE
    assert "worldProfiler.beginGpu();" in SCENE
    assert "scene.matrixWorldAutoUpdate = true;" in SCENE
    assert "worldProfiler.noteDraw(drawCamera, drawScene, geometry, material, object, group);" in SCENE
    assert "auditScene: (profile = null) => {" in SCENE
    assert "contain: layout;" in CSS
