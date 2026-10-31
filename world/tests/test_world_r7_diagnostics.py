import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
NODE = shutil.which("node")

SCRIPT = r"""
import { auditWorldScene, createWorldAuditState, createWorldProfiler } from "./world-profiler.js";
import { describeLongAnimationFrame, describeRenderStall, noteJsonRun } from "./world-debug-log.js";
import { createRenderStallMonitor } from "./scene/render-stalls.js";
import { settledMatrixWalk } from "./scene/settled-matrices.js";
import {
  compareWorldLoads,
  computeWorldLoadSuggestions,
  describeSceneElement,
  groupSceneElementPasses,
  worldBuildLabel,
  worldLoadColdCache,
  worldLoadSummary,
} from "./world-boot.js";

let time = 0;
const profiler = createWorldProfiler({ renderer: null, camera: "main", now: () => time });
const animated = profiler.watchCallbacks([]);
const refresh = () => { time += 4; };
const spin = () => { time += 0.01; };
animated.push(refresh);
profiler.claimCallbacks("ForkMesh Office");
animated.push(spin, () => {});
profiler.claimCallbacks("Office logo fountain");
profiler.setEnabled(true);
const frame = () => {
  profiler.beginFrame();
  profiler.runCallbacks(animated, time, 0);
  profiler.mark("ambient");
  time += 16;
  profiler.mark("render");
  profiler.endFrame();
};
for (let index = 0; index < 60; index += 1) frame();
const full = profiler.sample();
frame();
const early = profiler.sample();

const node = (props = {}) => ({ visible: true, children: [], matrixAutoUpdate: false, matrixWorld: { elements: new Array(16).fill(0) }, userData: {}, ...props });
const settled = new WeakSet();
const scene = node({ userData: { settledMatrixWalk: { walks: 0, visited: 0, composed: 0, thawed: 0, isSettled: (object) => settled.has(object) } } });
const still = node({ name: "still" });
const moving = node({ name: "moving", matrixAutoUpdate: true });
settled.add(still);
scene.children.push(still, moving);
const walk = scene.userData.settledMatrixWalk;
const state = createWorldAuditState();
let clock = 0;
auditWorldScene({ scene, renderer: { info: { memory: {}, programs: [] } }, state, now: () => clock, ownerOf: (object) => object === moving ? "Bikes" : "Town" });
walk.walks = 100; walk.visited = 50000; walk.composed = 2000; walk.thawed = 0;
clock = 1000;
const profile = { frames: 60, elapsedMs: 1000, cpuMs: 4, phases: [{ id: "matrices", label: "Scene matrix update", avgMs: 2, peakMs: 30 }], callbacks: [], passes: {}, gpu: {} };
const audit = auditWorldScene({ scene, renderer: { info: { memory: {}, programs: [] } }, state, profile, now: () => clock, ownerOf: (object) => object === moving ? "Bikes" : "Town" });

const monitor = createRenderStallMonitor({ renderer: { getContext: () => ({}), info: { programs: [] } }, camera: {}, now: () => clock });
monitor.beginFrame();
settledMatrixWalk.visited += 9800;
settledMatrixWalk.composed += 9500;
clock += 44; monitor.mark("matrices");
const stall = monitor.endFrame(80, 20);

noteJsonRun("https://app.forkmesh.com/api/accounts/jett?full=1", 1000);
const json = describeLongAnimationFrame({ startTime: 990, duration: 70, blockingDuration: 20, scripts: [
  { invoker: "Response.json.then", sourceURL: "https://world.forkmesh.com/world/world-runtime.js", sourceFunctionName: "", sourceCharPosition: 271452, duration: 59, startTime: 1003 },
] });

const elements = groupSceneElementPasses([
  { id: "office", label: "Office far proxy", ms: 56, objects: 0, meshes: 0, count: 1 },
  { id: "warm", label: "First view shader warm-up", ms: 3, objects: 0, meshes: 0, count: 1 },
  { id: "warm-2", label: "First view shader warm-up · pass 2", ms: 31, objects: 0, meshes: 0, count: 1 },
  { id: "warm-3", label: "First view shader warm-up · pass 3", ms: 20, objects: 0, meshes: 0, count: 1 },
  { id: "bikes", label: "World bikes", ms: 8, objects: 28, meshes: 24, count: 2 },
]);

const previous = { at: 0, build: "v0.9.2 aaaaaaaaaaaa", interactiveMs: 1060, firstFrameMs: 854, phases: {}, apis: {}, networkBytes: 210000, blockedMs: 895, worstMs: 163 };
const measure = { interactiveMs: 2620, firstFrameMs: 2000, phases: [], resources: [{ kind: "module", cache: "network", bytes: 1003000, at: 10, start: 0, ms: 10, name: "/world/world-scene.js" }], longFrames: [], build: { version: "0.9.3", revision: "bbbbbbbbbbbbcccc", deploy: "bbbbbbbbbbbbcccc-dirty-1" } };
const record = { build: worldBuildLabel(measure.build), interactiveMs: 2620, firstFrameMs: 2000, phases: {}, apis: {}, networkBytes: 1003000, blockedMs: 2510, worstMs: 352 };
measure.regressions = compareWorldLoads(previous, record);
measure.coldCache = worldLoadColdCache(record, previous);
const summary = worldLoadSummary(measure, measure.regressions, previous);
const tips = computeWorldLoadSuggestions(measure, null);

console.log(JSON.stringify({
  full: { frames: full.frames, callbacks: full.callbacks.map((entry) => entry.label) },
  earlySame: early === full,
  findings: audit.findings.map((finding) => ({ id: finding.id, fix: finding.fix })),
  stats: { unsettled: audit.stats.unsettledObjects, perWalk: audit.stats.walkVisitedPerWalk },
  stall: describeRenderStall(stall),
  json: json.text,
  jsonKey: json.culprits[0].key,
  elements: elements.map(describeSceneElement),
  summary,
  regression: tips.find((tip) => tip.id === "regressions"),
}));
"""


@pytest.fixture(scope="module")
def result():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(completed.stdout)


def test_ambient_callbacks_are_named_by_the_element_that_registered_them(result):
    labels = result["full"]["callbacks"]
    assert labels[0] == "ForkMesh Office"
    assert "Office logo fountain #1" in labels
    assert "Office logo fountain #2" in labels
    assert "animated.push(profiler.watchCallbacks" not in SCENE
    assert "worldProfiler.watchCallbacks([])" in SCENE
    assert "worldProfiler.claimCallbacks(label)" in SCENE


def test_a_short_profile_window_returns_the_last_full_reading(result):
    assert result["full"]["frames"] == 60
    assert result["earlySame"] is True


def test_the_matrix_finding_reports_the_walk_instead_of_a_zero_count(result):
    fixes = {finding["id"]: finding["fix"] for finding in result["findings"]}
    fix = fixes["phase-matrices"]
    assert "0 objects never moved" not in fix
    assert "The walk visits 500 objects per frame and recomposes 20" in fix
    assert "Unsettled objects by element:" in fix
    assert result["stats"]["perWalk"] == 500


def test_render_stalls_count_the_objects_the_matrix_walk_touched(result):
    assert "matrix walk 44 ms, 9.8k objects walked, 9.5k recomposed" in result["stall"]


def test_response_json_long_frames_name_the_url(result):
    assert "Response.json.then (app.forkmesh.com/api/accounts/jett)" in result["json"]
    assert result["jsonKey"].endswith("(app.forkmesh.com/api/accounts/jett)")


def test_shader_warm_up_passes_collapse_into_one_line(result):
    assert result["elements"] == [
        "Office far proxy 56 ms",
        "First view shader warm-up 54 ms · 3 passes (3/31/20 ms)",
        "World bikes 8 ms · 28 objects · 24 meshes · 2 parts",
    ]


def test_the_load_summary_names_the_build_and_a_cold_cache(result):
    assert result["summary"][0].startswith("World load summary · build v0.9.3 bbbbbbbbbbbb · interactive")
    assert "new build since the previous load: v0.9.2 aaaaaaaaaaaa → v0.9.3 bbbbbbbbbbbb" in result["summary"][-1]
    tip = result["regression"]
    assert tip["level"] == "info"
    assert tip["detail"].startswith("Cold cache, not a code regression by itself")


def test_the_optimization_report_names_the_build_and_audits_the_same_window():
    report = APP.split("  perfReportMarkdown({ logTail = true } = {}) {", 1)[1].split(
        "\n  async copyPerfReport", 1)[0]
    assert "runningWorldBuildStamp()" in report
    assert "runningWorldDeployFingerprint()" in report
    assert '"unknown"' not in report.split("## Frame", 1)[0]
    assert "the findings above use this window" in report
    copy = APP.split("  perfCopyReport({ logTail = false } = {}) {", 1)[1].split("\n  }\n", 1)[0]
    assert copy.index("getProfile") < copy.index("this.runPerfAudit();")
    button = APP.split("  async copyPerfReport() {", 1)[1].split("\n  }\n", 1)[0]
    assert "this.perfCopyReport({ logTail: true })" in button
