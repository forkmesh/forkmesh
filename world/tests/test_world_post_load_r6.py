import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_module, world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE = world_scene_source()
NODE = shutil.which("node")

PACER_SCRIPT = r"""
import { COMPLETION_STATUS_KHR, createUploadBudget, installProgramPacer, textureUploadBytes } from "./scene/gpu-pacer.js";

const log = [];
const done = new Set();
const gl = {
  LINK_STATUS: 0x8b82,
  getExtension: (name) => (name === "KHR_parallel_shader_compile" ? {} : null),
  compileShader(shader) { log.push(`compile ${shader.id}`); },
  attachShader(program, shader) { log.push(`attach ${program.id} ${shader.id}`); },
  linkProgram(program) { log.push(`link ${program.id}`); },
  getProgramParameter(program, pname) {
    if (pname === COMPLETION_STATUS_KHR) return done.has(program.id);
    log.push(`read ${program.id}`);
    return true;
  },
  getUniformLocation(program) { log.push(`uniform ${program.id}`); return {}; },
  deleteProgram(program) { log.push(`delete ${program.id}`); },
  deleteShader(shader) { log.push(`deleteShader ${shader.id}`); },
};
const pacer = installProgramPacer(gl);
const build = (id) => {
  const program = { id };
  const vertex = { id: `${id}v` };
  const fragment = { id: `${id}f` };
  gl.compileShader(vertex);
  gl.compileShader(fragment);
  gl.attachShader(program, vertex);
  gl.attachShader(program, fragment);
  gl.linkProgram(program);
  return program;
};
const a = build("a");
const openLog = log.splice(0);
pacer.pace();
const b = build("b");
const c = build("c");
const d = build("d");
const queuedLog = log.splice(0);
const queuedReady = gl.getProgramParameter(b, COMPLETION_STATUS_KHR);
pacer.tick();
const firstTick = log.splice(0);
pacer.tick();
const busyTick = log.splice(0);
done.add("b");
pacer.tick();
const secondTick = log.splice(0);
gl.getUniformLocation(d, "map");
const forced = log.splice(0);
done.add("c");
done.add("d");
pacer.tick();
const drained = log.splice(0);
const e = pacer.paced(() => build("e"));
const lateLog = log.splice(0);
gl.deleteProgram(e);
const deleted = log.splice(0);
pacer.tick();
const afterDelete = log.splice(0);
["f", "g", "h", "i", "j", "k"].forEach(build);
log.splice(0);
pacer.tick();
const single = log.filter((line) => line.startsWith("link"));
log.splice(0);
done.add("f");
pacer.tick();
const burst = log.filter((line) => line.startsWith("link"));
log.splice(0);
["g", "h", "i", "j", "k"].forEach((id) => done.add(id));
for (let index = 0; index < 6; index += 1) pacer.tick();
const drainedOneByOne = log.filter((line) => line.startsWith("link"));
log.splice(0);
build("l");
log.splice(0);
pacer.hold(2);
pacer.tick();
pacer.tick();
const held = log.splice(0);
pacer.tick();
const released = log.filter((line) => line.startsWith("link"));
log.splice(0);

const idle = installProgramPacer({ linkProgram() {}, getExtension: () => null });
idle.pace();

let clock = 0;
const budget = createUploadBudget({ bytes: 100, refillMs: 10, now: () => clock });
const grants = [budget.take(60), budget.take(60), budget.take(30)];
clock += 5;
grants.push(budget.take(30));
clock += 20;
grants.push(budget.take(400));
grants.push(budget.take(10));
clock += 20;
grants.push(budget.take(10));
clock += 20;
grants.push(budget.take(10));
const bytes = [
  textureUploadBytes({ image: { width: 16, height: 8 }, generateMipmaps: true, minFilter: 1006 }),
  textureUploadBytes({ image: { width: 16, height: 8 }, generateMipmaps: true, minFilter: 1008 }),
  textureUploadBytes({ image: { width: 4, height: 4, depth: 3 }, generateMipmaps: false, minFilter: 1008 }),
];

console.log(JSON.stringify({
  openLog, queuedLog, queuedReady, firstTick, busyTick, secondTick, forced, drained,
  lateLog, deleted, afterDelete, single, burst, drainedOneByOne, held, released, stats: pacer.stats, idleStats: idle.stats, grants, bytes,
}));
"""


STRIPE_SCRIPT = r"""
import { createStripedUploads, createUploadBudget, stripeableTexture } from "./scene/gpu-pacer.js";

const calls = [];
const gl = {
  TEXTURE_2D: 3553, RGBA: 6408, UNSIGNED_BYTE: 5121, NONE: 0, BROWSER_DEFAULT_WEBGL: 37444,
  UNPACK_FLIP_Y_WEBGL: 1, UNPACK_PREMULTIPLY_ALPHA_WEBGL: 2, UNPACK_COLORSPACE_CONVERSION_WEBGL: 3,
  UNPACK_ALIGNMENT: 4, UNPACK_ROW_LENGTH: 5, UNPACK_SKIP_PIXELS: 6, UNPACK_SKIP_ROWS: 7,
  texSubImage2D(target, level, x, y, width, height) { calls.push(`sub ${y} ${height}`); },
  generateMipmap() { calls.push("mipmap"); },
};
const stores = {};
const state = {
  bindTexture(type, texture) { calls.push(`bind ${texture.id}`); },
  pixelStorei(name, value) { stores[name] = value; },
};
const properties = new Map();
const renderer = {
  getContext: () => gl,
  state,
  capabilities: { isWebGL2: true, maxTextureSize: 8192 },
  properties: {
    get(object) { if (!properties.has(object)) properties.set(object, {}); return properties.get(object); },
    has(object) { return properties.has(object); },
  },
  initTexture(texture) {
    const props = this.properties.get(texture);
    props.__webglTexture = { id: texture.name };
    props.__version = texture.version;
    calls.push(`init ${texture.name} ready=${texture.source.dataReady}`);
  },
};
const listeners = new Map();
const makeTexture = (name, width, height) => ({
  name, isTexture: true, format: 1023, type: 1009, version: 1, flipY: true, premultiplyAlpha: false,
  unpackAlignment: 4, colorSpace: "srgb", generateMipmaps: false, minFilter: 1006, mipmaps: [],
  source: { dataReady: true }, image: { width, height },
  addEventListener(type, fn) { listeners.set(this, fn); },
  removeEventListener() { listeners.delete(this); },
  dispose() { calls.push(`dispose ${name}`); listeners.get(this)?.(); },
});
let clock = 0;
const budget = createUploadBudget({ bytes: 1024 * 1024 * 64, now: () => clock });
const stripes = createStripedUploads({ renderer, THREE: null, budget, stripeBytes: 1024 * 4 * 256, perFrame: 2 });
const board = makeTexture("board", 1024, 1100);
const small = makeTexture("small", 256, 256);
const eligible = [stripeableTexture(board), stripeableTexture(small)];
const began = stripes.begin(board);
const beginLog = calls.splice(0);
const version = renderer.properties.get(board).__version;
stripes.step();
const first = calls.splice(0);
const skip = stores[gl.UNPACK_SKIP_ROWS];
stripes.step();
const second = calls.splice(0);
stripes.step();
const third = calls.splice(0);
const material = { map: makeTexture("old", 1024, 1000), needsUpdate: false };
renderer.properties.get(material.map).__webglTexture = { id: "old" };
const replacement = makeTexture("kiosk", 1024, 1024);
stripes.swapMap(material, replacement);
calls.splice(0);
const before = material.map.name;
stripes.step();
stripes.step();
const after = material.map.name;
const swapLog = calls.splice(0);
const repainted = makeTexture("repainted", 1024, 1024);
stripes.begin(repainted);
calls.splice(0);
repainted.version += 1;
stripes.step();
const dropLog = calls.splice(0);
const plate = makeTexture("plate", 512, 256);
renderer.properties.get(plate).__webglTexture = { id: "plate" };
renderer.properties.get(plate).__version = 1;
plate.version = 2;
clock += 10000;
budget.take(1024 * 1024 * 64);
const plateMaterial = { map: plate };
const deferredCount = stripes.deferDrawUploads(plateMaterial);
const pretended = renderer.properties.get(plate).__version;
calls.splice(0);
clock += 1000;
stripes.step();
const reuploadLog = calls.splice(0);
const reuploadStats = { deferred: stripes.stats.deferred, reuploaded: stripes.stats.reuploaded };
console.log(JSON.stringify({ deferredCount, pretended, reuploadLog, reuploadStats, eligible, began, beginLog, version, first, skip, second, third, before, after, swapLog, dropLog, stats: stripes.stats, pending: stripes.pending() }));
"""


@pytest.fixture(scope="module")
def stripes():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", STRIPE_SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(completed.stdout)


def test_large_canvas_textures_upload_in_stripes(stripes):
    assert stripes["eligible"] == [True, False]
    assert stripes["began"] is True
    assert stripes["beginLog"] == ["init board ready=false"]
    assert stripes["version"] == 1
    assert stripes["first"] == ["bind board", "sub 0 256", "bind board", "sub 256 256"]
    assert stripes["skip"] == 0
    assert stripes["second"] == ["bind board", "sub 512 256", "bind board", "sub 768 256"]
    assert stripes["third"] == ["bind board", "sub 1024 76"]


def test_a_new_board_texture_replaces_the_old_one_once_uploaded(stripes):
    assert stripes["before"] == "old"
    assert stripes["after"] == "kiosk"
    assert stripes["swapLog"][-1] == "dispose old"
    assert stripes["swapLog"].count("bind kiosk") == 4


def test_a_repaint_beyond_the_frame_budget_waits_for_the_next_frame(stripes):
    assert stripes["deferredCount"] == 1
    assert stripes["pretended"] == 2
    assert stripes["reuploadLog"] == ["init plate ready=true"]
    assert stripes["reuploadStats"] == {"deferred": 1, "reuploaded": 1}


def test_a_repainted_texture_is_left_to_three(stripes):
    assert stripes["dropLog"] == []
    assert stripes["stats"]["dropped"] == 1
    assert stripes["pending"] == 0


@pytest.fixture(scope="module")
def pacer():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", PACER_SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    return json.loads(completed.stdout)


def test_programs_link_at_once_until_the_first_view_is_ready(pacer):
    assert pacer["openLog"] == ["compile av", "compile af", "attach a av", "attach a af", "link a"]


def test_paced_programs_queue_and_report_not_ready(pacer):
    assert pacer["queuedLog"] == [
        "attach b bv", "attach b bf",
        "attach c cv", "attach c cf",
        "attach d dv", "attach d df",
    ]
    assert pacer["queuedReady"] is False


def test_one_program_links_per_frame_after_the_last_one_finished(pacer):
    assert pacer["firstTick"] == ["compile bv", "compile bf", "link b"]
    assert pacer["busyTick"] == []
    assert pacer["secondTick"] == ["compile cv", "compile cf", "link c"]


def test_a_synchronous_read_links_its_program_at_once(pacer):
    assert pacer["forced"] == ["compile dv", "compile df", "link d", "uniform d"]
    assert pacer["drained"] == []
    assert pacer["stats"]["forced"] == 1


def test_forced_pacing_and_deleted_programs(pacer):
    assert pacer["lateLog"] == ["attach e ev", "attach e ef"]
    assert pacer["deleted"] == ["delete e"]
    assert pacer["afterDelete"] == []
    assert pacer["afterDelete"] == []


def test_links_never_go_in_bursts(pacer):
    assert pacer["single"] == ["link f"]
    assert pacer["burst"] == ["link g"]
    assert pacer["drainedOneByOne"] == ["link h", "link i", "link j", "link k"]


def test_a_resize_holds_new_links_for_two_frames(pacer):
    assert pacer["held"] == []
    assert pacer["released"] == ["link l"]


def test_without_parallel_compile_nothing_is_paced(pacer):
    assert pacer["idleStats"]["paced"] is False


def test_the_upload_budget_spaces_large_textures(pacer):
    assert pacer["grants"] == [True, False, True, True, True, False, False, True]
    assert pacer["bytes"] == [512, 683, 192]


def test_the_scene_paces_programs_after_the_first_view():
    assert "const programPacer = installProgramPacer(renderer.getContext());" in SCENE
    assert "const uploadBudget = createUploadBudget();" in SCENE
    animate = world_scene_module("runtime/animate.js")
    render = animate.index("ctx.worldProfiler.endFrame();")
    assert animate.index("ctx.programPacer.tick();", render) > render
    diagnostics = world_scene_module("runtime/diagnostics.js")
    first_view = diagnostics[diagnostics.index("async function prepareFirstView("):]
    first_view = first_view[: first_view.index("function releaseConstructionCanvases")] if "function releaseConstructionCanvases" in first_view else first_view[: first_view.index("const warmLights")]
    assert first_view.index("ctx.firstViewHoldUntil = 0;") < first_view.index("ctx.programPacer.pace();")
    assert "pending = ctx.programPacer.paced(() =>" in diagnostics
    assert "if (!ctx.uploadBudget.takeTexture(texture)) break;" in diagnostics
    assert "ctx.stripedUploads.accepts(texture) || ctx.uploadBudget.takeTexture(texture)," in diagnostics
    assert "if (ctx.stripedUploads.begin(texture)) {" in diagnostics
    assert "ctx.stripedUploads.step(ctx.programPacer.busy() ? 1 : undefined);" in animate


def test_desktop_agent_shells_share_materials_per_model():
    bots = world_scene_module("agent-bots.js")
    assert 'return makeSharedMaterial(THREE, "#14272c", {' in bots
    assert "desktopAgentBallMaterial(THREE, appearance.shell)," in bots
    assert "desktopAgentBodyMaterial(THREE, appearance.shell)," in bots
    runtime = world_scene_module("runtime/bots.js")
    assert "avatar.userData.body.material = desktopAgentBodyMaterial(THREE, appearance.shell);" in runtime
    assert "avatar.userData.rollingBall.material = desktopAgentBallMaterial(THREE, appearance.shell);" in runtime
    assert "body.material.color.set(" not in runtime



WARM_SCRIPT = r"""
import { installShaderCompileGuard } from "./scene/shader-warmup.js";

let clock = 0;
const drawn = [];
const renderer = { info: { programs: [], render: { frame: 1 } } };
renderer.renderBufferDirect = function (camera, scene, geometry, material, object) {
  drawn.push(object.id);
  if (!material.program) {
    clock += 10;
    material.program = { program: {}, isReady() { return true; }, getUniforms() { return {}; } };
    renderer.info.programs.push(material.program);
  }
  material.program.getUniforms();
};
const guard = installShaderCompileGuard(renderer, () => true, () => clock);
const objects = Array.from({ length: 6 }, (_, id) => ({ id, material: {} }));
const pass = (resume) => guard.warm(
  () => objects.forEach((object) => renderer.renderBufferDirect({}, {}, {}, object.material, object, null)),
  null,
  25,
  resume,
);
const first = pass(false);
const firstDrawn = drawn.splice(0);
const second = pass(true);
const secondDrawn = drawn.splice(0);
const fresh = pass(false);
const freshDrawn = drawn.splice(0);
console.log(JSON.stringify({ first, firstDrawn, second, secondDrawn, fresh, freshDrawn }));
"""


def test_resumed_first_view_passes_skip_draws_already_warmed():
    if not NODE:
        pytest.skip("node is not installed")
    completed = subprocess.run(
        [NODE, "--input-type=module", "-e", WARM_SCRIPT],
        cwd=WORLD,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    result = json.loads(completed.stdout)
    assert result["first"] is False
    assert result["firstDrawn"] == [0, 1, 2]
    assert result["second"] is True
    assert result["secondDrawn"] == [3, 4, 5]
    assert result["fresh"] is True
    assert result["freshDrawn"] == [0, 1, 2, 3, 4, 5]
    core = world_scene_module("core.js")
    assert "`first-view-warm-${pass + 1}`," in core
    build = (WORLD / "scene" / "scene-build.js").read_text(encoding="utf-8")
    assert "export const SCENE_BUILD_FRAME_MS = 32;" in build
    assert "export const SCENE_BUILD_SLICE_MS = 12;" in build


def test_swaying_aquarium_meshes_stay_out_of_the_office_far_proxy():
    aquarium = world_scene_module("office-aquarium.js")
    assert "mesh.userData.farProxySkip = true;" in aquarium
    floors = world_scene_module("districts/office-floors.js")
    begin = floors[floors.index("function beginOfficeFarProxy() {"):]
    begin = begin[: begin.index("function finishOfficeFarProxy(")]
    assert begin.index("if (object.userData?.farProxySkip === true) townVisibility.set(object, false);") < begin.index(
        "mergeObjectBatchesSteps(THREE, high, {"
    )
