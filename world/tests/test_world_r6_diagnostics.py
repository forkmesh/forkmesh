import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_module, world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = world_scene_source()
NODE = shutil.which("node")

SCRIPT = r"""
import { createRenderStallMonitor } from "./scene/render-stalls.js";
import { describeLongAnimationFrame, describeRenderStall } from "./world-debug-log.js";

let clock = 0;
const gl = {};
const renderer = { getContext: () => gl, info: { programs: [] } };
const monitor = createRenderStallMonitor({ renderer, camera: {}, now: () => clock });
monitor.beginFrame();
clock += 2; monitor.mark("housekeeping");
clock += 12; monitor.mark("frame-animations");
clock += 3; monitor.mark("avatars");
clock += 4; monitor.mark("labels");
clock += 30; monitor.mark("matrices");
clock += 8; monitor.mark("render");
const stall = monitor.endFrame(62, 8);
monitor.beginFrame();
const quiet = monitor.endFrame(62, 8);

const bare = describeLongAnimationFrame({
  startTime: 500, duration: 113, blockingDuration: 0,
  renderStart: 520, styleAndLayoutStart: 525, scripts: [],
}, { nearbyResource: (start, end) => (start === 500 && end === 613 ? "world.css" : "") });
const gc = describeLongAnimationFrame({
  startTime: 100, duration: 120, blockingDuration: 60,
  renderStart: 200, styleAndLayoutStart: 205, scripts: [],
}, { nearbyResource: () => "world.css" });
const old = describeLongAnimationFrame({ startTime: 0, duration: 80, scripts: [] });

console.log(JSON.stringify({
  stall, quiet,
  text: describeRenderStall(stall),
  quietText: describeRenderStall(quiet),
  bare: bare.text, gc: gc.text, old: old.text,
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


def test_scene_update_names_the_matrix_walk_animation_and_labels(result):
    parts = result["stall"]["updateParts"]
    assert [part["label"] for part in parts] == ["matrix walk", "animation", "labels", "housekeeping"]
    assert [part["ms"] for part in parts] == [30, 15, 4, 2]
    assert result["text"].endswith(
        "scene update 54 ms (matrix walk 30 ms · animation 15 ms · labels 4 ms · housekeeping 2 ms)"
    )
    assert result["quiet"]["updateParts"] == []
    assert result["quietText"].endswith("scene update 54 ms")


def test_the_scene_feeds_profiler_phases_into_the_stall_monitor():
    assert "worldProfiler.mark = function markRenderStallPhase(phase) {" in SCENE
    assert "renderStalls.mark(phase);" in SCENE


def test_unattributed_long_frames_say_where_the_time_went(result):
    assert result["bare"] == (
        "Main thread blocked 113 ms (0 ms blocking) · no script attribution: "
        "20 ms of tasks before rendering (scripts under 5 ms each, GC, parsing or browser work) · 5 ms in frame callbacks · "
        "88 ms style, layout & paint (after world.css loaded)"
    )
    assert result["gc"].startswith(
        "Main thread blocked 120 ms (60 ms blocking) · no script attribution: "
        "100 ms of tasks before rendering (scripts under 5 ms each, GC, parsing or browser work)"
    )
    assert "(parsing or compiling world.css, which arrived just before)" in result["gc"]
    assert "after world.css loaded" not in result["gc"]
    assert result["old"].endswith("no script attribution (rendering, GC or browser work)")


def test_instanced_mirror_batches_are_owned_by_their_sources_element():
    cache = (WORLD / "world-resource-cache.js").read_text(encoding="utf-8")
    assert "batch.mesh.userData.mirrorOwnerSource = pending[0];" in cache
    setup = world_scene_module("runtime/render-setup.js")
    owner = setup[setup.index("function worldElementOwner(object) {"):]
    owner = owner[: owner.index("\n  }\n")]
    assert "object.userData.mirrorOwnerSource" in owner
    assert owner.index("mirrored") < owner.index("profilerOwnerRoots.get(node)")
