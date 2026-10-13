import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_module, world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CHAT = (Path(__file__).resolve().parents[2] / "app" / "public" / "dashboard-chat.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
NODE = shutil.which("node")

STALL_SCRIPT = r"""
import { createRenderStallMonitor } from "./scene/render-stalls.js";
import { describeFirstSight, describeRenderStall } from "./world-debug-log.js";

let clock = 0;
const gl = {
  texImage2D(...args) { clock += 40; },
  getProgramParameter() { clock += 25; return true; },
  linkProgram() { clock += 1; },
};
const programs = [];
const renderer = { getContext: () => gl, info: { programs } };
const camera = { name: "main" };
const scene = { name: "scene" };
let warming = false;
const monitor = createRenderStallMonitor({
  renderer,
  camera,
  ownerOf: (object) => object.element,
  isWarming: () => warming,
  now: () => clock,
});
const tabletop = { name: "tabletop", element: "Office interior" };
const material = { type: "MeshPhysicalMaterial", name: "" };

monitor.beginFrame();
monitor.beginDraw(camera, scene, material, tabletop);
programs.push({ name: "MeshPhysicalMaterial" });
gl.linkProgram();
gl.getProgramParameter();
gl.texImage2D(0, 0, 0, 0, 0, { width: 512, height: 256 });
monitor.endDraw();
monitor.beginDraw(camera, null, material, tabletop);
clock += 12;
monitor.endDraw();
const stall = monitor.endFrame(90, 80);

monitor.beginFrame();
warming = true;
monitor.beginDraw(camera, scene, material, tabletop);
programs.push({ name: "MeshBasicMaterial" });
monitor.endDraw();
warming = false;
const quiet = monitor.endFrame(12, 10);

const reflection = { name: "reflection-face" };
monitor.beginFrame();
monitor.beginDraw(reflection, scene, { type: "MeshStandardMaterial", name: "aquarium" }, { name: "rock", element: "Office interior" });
programs.push({ name: "MeshStandardMaterial" });
monitor.endDraw();
monitor.endFrame(20, 18);

const snapshot = monitor.snapshot();
console.log(JSON.stringify({
  stall,
  quiet,
  text: describeRenderStall(stall),
  firstSight: snapshot.firstSight,
  firstSightText: snapshot.firstSight.map(describeFirstSight),
  totals: snapshot.totals,
  hooked: typeof gl.texImage2D === "function" && gl.texImage2D.__forkmeshStallHook === true,
}));
"""


@pytest.fixture(scope="module")
def stalls():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", STALL_SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(completed.stdout)


def test_a_long_frame_is_split_into_compiles_uploads_and_passes(stalls):
    stall = stalls["stall"]
    assert stall["programs"] == 1
    assert stall["programNames"] == ["MeshPhysicalMaterial · Office interior"]
    assert stall["waitMs"] == 25
    assert stall["compileMs"] == 1
    assert stall["uploads"] == 1
    assert stall["uploadBytes"] == 512 * 256 * 4
    assert stall["uploadMs"] == 40
    assert stall["uploadNames"] == ["Office interior · tabletop 40 ms"]
    assert stall["shadowDraws"] == 1
    assert stall["shadowMs"] == 12
    text = stalls["text"]
    assert text.startswith("Render frame 90 ms · draw 80 ms")
    assert "1 program compile, 26 ms waiting (MeshPhysicalMaterial · Office interior)" in text
    assert "shadow map 1 draws 12 ms" in text
    assert "1 texture upload 40 ms, 0.5 MB" in text
    assert stalls["hooked"] is True


def test_short_frames_and_warm_passes_are_not_reported(stalls):
    assert stalls["quiet"] is None
    passes = [entry["pass"] for entry in stalls["firstSight"]]
    assert passes == ["main", "other"]
    assert stalls["totals"]["stalls"] == 1
    assert stalls["totals"]["firstSight"] == 2
    assert stalls["firstSightText"][0] == (
        "First-sight compile: MeshPhysicalMaterial · Office interior (tabletop)"
    )
    assert stalls["firstSightText"][1] == (
        'First-sight compile in the other pass: MeshStandardMaterial "aquarium" · Office interior (rock)'
    )


def test_the_scene_wires_the_stall_monitor_into_every_draw_and_frame():
    assert "const renderStalls = createRenderStallMonitor({" in SCENE
    assert "isWarming: () => shaderCompileGuard.warming," in SCENE
    draw = SCENE[SCENE.index("renderer.renderBufferDirect = function profiledRenderBufferDirect("):]
    draw = draw[: draw.index("\n  };")]
    assert draw.index("renderStalls.beginDraw(drawCamera, drawScene, material, object);") < draw.index(
        "return unprofiledRenderBufferDirect.call("
    ) < draw.index("renderStalls.endDraw();")
    animate = world_scene_module("runtime/animate.js")
    assert "ctx.renderStalls.beginFrame();" in animate
    assert "ctx.renderStalls.endFrame(frameWorkMs, performance.now() - renderStartedAt);" in animate
    assert "renderStalls: ctx.renderStalls.snapshot()," in world_scene_module("runtime/diagnostics.js")


def test_the_debug_log_and_report_carry_stalls_first_sight_and_program_owners():
    note = APP[APP.index("  notePerfLog(snapshot) {"):]
    note = note[: note.index("    if (previous && renderer) {")]
    assert '.forEach((entry) => log.add("SHADER", describeFirstSight(entry)));' in note
    assert '.forEach((entry) => log.add("LONG", describeRenderStall(entry)));' in note
    report = APP[APP.index("  perfReportMarkdown("):]
    assert '"## Render stalls (frames over 50 ms, newest last)",' in report
    assert '"## First-sight compiles (newest last)",' in report
    assert '"## Programs by World element",' in report
    assert "renderStalls: scene.renderStalls || null," in APP


def test_far_members_wait_hidden_for_their_layers_instead_of_drawing_whole():
    batches = world_scene_module("figure-batches.js")
    assert 'desired = desired === "high" ? "waiting" : "full";' in batches
    assert 'if (record.state !== "high" && record.state !== "waiting") continue;' in batches
    assert "const FIGURE_STAGING_CANVAS = Object.freeze({ willReadFrequently: true });" in batches
    assert "copyBudget = gpuBusy() ? 0 : frameCopyBudget();" in batches
    assert "if (nearTexturesReady(record, uploadStartedAt)) continue;" in batches
    canvas = world_scene_module("canvas.js")
    assert "worldPanelSoftwareDepth > 0" in canvas
    assert "canvas.getContext(\"2d\", SOFTWARE_CANVAS_OPTIONS)" in canvas
    assert "softwareCanvas(() => canvasArtTexture(...args))" in world_scene_module("avatars.js")
    assert "softwareCanvas(() => canvasTexture(...args))" in world_scene_module("avatar-badges.js")


def test_point_light_count_is_checked_every_frame():
    animate = world_scene_module("runtime/animate.js")
    assert animate.index("fns.ensurePointLightCount();") < animate.index("ctx.scene.updateMatrixWorld();")
    quality = world_scene_module("runtime/quality-environment.js")
    ensure = quality[quality.index("function ensurePointLightCount() {"):]
    ensure = ensure[: ensure.index("\n  }\n")]
    assert "if (visible !== ctx.qualityProfile.pointLightBudget) updateLocalPointLightBudget();" in ensure


def test_warm_compiles_read_lights_without_walking_the_scene():
    diagnostics = world_scene_module("runtime/diagnostics.js")
    target = diagnostics[diagnostics.index("function warmTarget() {"):]
    target = target[: target.index("\n  function warmBeforeReveal(")]
    assert "at - warmLightsAt >= GPU_WARMUP_LIGHTS_REFRESH_MS" in target
    assert "traverseVisible(visit) {" in target
    assert "get environment() {" in target
    assert "warmMaterialsAsync(ctx.renderer, root, ctx.camera, warmTarget())" in diagnostics
    assert "ctx.camera,\n            warmTarget(),\n          ),\n        ).catch(() => {});" in diagnostics


def test_out_of_reach_shadow_casters_are_parked_and_restored():
    diagnostics = world_scene_module("runtime/diagnostics.js")
    park = diagnostics[diagnostics.index("const parkShadowCaster = (object, reach) => {"):]
    park = park[: park.index("\n    };")]
    assert "object.castShadow = false;" in park
    assert "object.userData.forkmeshShadowParked = true;" in park
    assert "object.castShadow = true;" in park
    assert "if (object.isSkinnedMesh || object.isInstancedMesh || object.isBatchedMesh) return;" in park


def test_chat_island_reads_layout_after_paint_and_caps_live_rows():
    pin = CHAT[CHAT.index("  function pinFullLogToNewest() {"):]
    pin = pin[: pin.index("\n  function scheduleChatScrollThumb() {")]
    assert "afterNextPaint(() => {" in pin
    assert "if (fullLogPinPending) return;" in pin
    assert "requestAnimationFrame(() => setTimeout(callback, 0));" in CHAT
    observer = CHAT[CHAT.index('if (fullLog && "ResizeObserver" in window) {'):]
    observer = observer[: observer.index("}).observe(fullLog);")]
    assert "scheduleChatScrollThumb();" in observer
    assert "syncChatScrollThumb();" not in observer
    side = CHAT[CHAT.index("  function renderSideMessagesNow() {"):]
    side = side[: side.index("\n  function appendSideMessage(")]
    assert "scrollIntoView" not in side
    assert "const LIVE_ROW_LIMIT = 150;" in CHAT
    assert "historyRowIds.push(id);" in CHAT[CHAT.index("function retireOldLiveRows() {"):]
    island = APP[APP.index("  pinHudIslandFeed() {"):]
    island = island[: island.index("\n  bindHudIsland(")]
    assert "window.setTimeout(() => {" in island
    assert "if (island.dataset.expanded !== \"true\") this.pinHudIslandFeed();" in APP


PROGRAMS_SCRIPT = r"""
import { auditWorldScene } from "./world-profiler.js";
const matrix = () => ({ elements: [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1], getMaxScaleOnAxis: () => 1 });
const node = (props = {}) => ({ visible: true, children: [], matrixAutoUpdate: false, matrixWorld: matrix(), userData: {}, layers: { mask: 1 }, ...props });
const geometry = { type: "BoxGeometry", parameters: {}, attributes: { position: { count: 3, version: 0, array: new Float32Array(9) } }, index: null, boundingSphere: { radius: 1 } };
const office = [0, 1, 2].map((index) => ({ type: "MeshStandardMaterial", visible: true, opacity: 1, color: null, name: `desk-${index}`, roughness: index }));
const sky = { type: "MeshBasicMaterial", visible: true, opacity: 1, color: null };
const properties = new Map([
  [office[0], { programs: new Map([["a", {}], ["b", {}]]) }],
  [office[1], { programs: new Map([["a", {}], ["c", {}]]) }],
  [office[2], { programs: new Map([["d", {}]]) }],
  [sky, { programs: new Map([["e", {}]]) }],
]);
const scene = node();
const officeRoot = node({ name: "office" });
const skyRoot = node({ name: "sky" });
scene.children.push(officeRoot, skyRoot);
office.forEach((material) => officeRoot.children.push(node({ isMesh: true, geometry, material, parent: officeRoot, element: "Office interior" })));
skyRoot.children.push(node({ isMesh: true, geometry, material: sky, parent: skyRoot, element: "Sky" }));
const renderer = { info: { memory: { geometries: 1, textures: 0 }, programs: new Array(5) }, properties: { has: (m) => properties.has(m), get: (m) => properties.get(m) } };
const audit = auditWorldScene({ scene, renderer, ownerOf: (object) => object.element || "" });
console.log(JSON.stringify(audit.stats.programsByElement));
"""


def test_audit_counts_programs_by_world_element():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", PROGRAMS_SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    rows = json.loads(completed.stdout)
    assert rows[0] == {
        "label": "Office interior",
        "programs": 4,
        "materials": 3,
        "types": "MeshStandard × 3",
    }
    assert rows[1]["label"] == "Sky"
    assert rows[1]["programs"] == 1
