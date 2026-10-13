import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
SCENE = world_scene_source()
APP = (WORLD / "world.js").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")
NODE = shutil.which("node")


def node(script: str) -> dict:
    if not NODE:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        check=True,
        text=True,
        capture_output=True,
        timeout=60,
    )
    return json.loads(result.stdout)


def block(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin : source.index(end, begin)]


def test_shader_first_use_is_spread_across_frames():
    result = node(
        f"""
      import {{ installShaderCompileGuard }} from {json.dumps(SCENE_MODULE.as_uri())};
      let clock = 0;
      const renderer = {{
        info: {{ programs: [], render: {{ frame: 1 }} }},
        drawn: [],
        renderBufferDirect(camera, scene, geometry, material) {{
          material.program.getUniforms();
          this.drawn.push(material.name);
        }},
      }};
      const view = {{ camera: {{}}, scene: {{}} }};
      const guard = installShaderCompileGuard(
        renderer,
        (camera, scene) => camera === view.camera && scene === view.scene,
        () => clock,
      );
      const make = (name) => ({{ name, program: {{ program: {{}},
        isReady() {{ return true; }}, getUniforms() {{ clock += 40; return {{}}; }} }} }});
      const a = make("a");
      const b = make("b");
      renderer.info.programs.push(a.program, b.program);
      renderer.renderBufferDirect(view.camera, view.scene, null, a, {{}}, null);
      renderer.renderBufferDirect(view.camera, view.scene, null, b, {{}}, null);
      const firstFrame = [...renderer.drawn];
      renderer.info.render.frame = 2;
      renderer.renderBufferDirect(view.camera, view.scene, null, b, {{}}, null);
      process.stdout.write(JSON.stringify({{
        firstFrame,
        drawn: renderer.drawn,
        budgetDeferred: guard.budgetDeferredDraws,
        compiled: guard.compiledPrograms,
      }}));
    """
    )
    assert result["firstFrame"] == ["a"]
    assert result["drawn"] == ["a", "b"]
    assert result["budgetDeferred"] == 1
    assert result["compiled"] == 2


def test_flag_palette_is_sampled_once_per_glyph_on_a_cpu_canvas():
    shirt = block(SCENE, "function countryShirtTexture(", "\n}\n")
    assert "cachedFlagShirtPalette(flag)" in shirt
    assert "getImageData" not in shirt
    assert "fillText(flag" not in shirt
    cache = block(SCENE, "function cachedFlagShirtPalette(flag) {", "\n}\n")
    assert "flagShirtPalettes.get(flag)" in cache
    assert "flagShirtPalettes.set(flag, palette)" in cache
    assert 'getContext("2d", { willReadFrequently: true })' in cache
    assert "const flagShirtPalettes = new Map();" in SCENE


def test_members_circle_figures_are_built_nearest_first_in_idle_slices():
    lounge = block(SCENE, "  function updateMemberLounge(", "\n  function visitNeighborhoodHome(")
    assert "loungeJobs.push({ member, index, name, id });" in lounge
    assert "const dressLoungeMember = ({ member, index, name, id }) => {" in lounge
    assert "scheduleLoungeFigures(loungeJobs, dressLoungeMember," in lounge
    assert lounge.index("scheduleLoungeFigures(") < lounge.index("loungeMembers.forEach((figure, id) => {")
    scheduler = block(SCENE, "  function scheduleLoungeFigures(", "\n  // Public account facts")
    assert "loungeBuildGeneration += 1;" in scheduler
    assert "generation !== loungeBuildGeneration" in scheduler
    assert "disposed ||" in scheduler
    assert "Math.hypot(" in scheduler
    assert ".sort((a, b) => distance.get(a) - distance.get(b)" in scheduler
    assert "if (dressed > 0 && performance.now() - started >= LOUNGE_FIGURE_SLICE_MS) {" in scheduler
    assert "window.requestIdleCallback(step, { timeout: 120 })" in scheduler
    assert "cancelIdleCallback" in scheduler
    assert re.search(r"const LOUNGE_FIGURE_SLICE_MS = \d+;", SCENE)


def test_office_board_plates_paint_after_construction_in_idle_slices():
    texture = block(SCENE, "function canvasTexture(", "\n}\n")
    assert "Array.isArray(deferredCanvasPaints)" in texture
    assert "pendingCanvasPaints.set(canvas, job);" in texture
    repaint = block(SCENE, "function repaintCanvasTexture(", "\n}\n")
    assert "pendingCanvasPaints.delete(canvas);" in repaint
    job = block(SCENE, "function paintCanvasJob(", "\n}\n")
    assert "pendingCanvasPaints.get(job.canvas) !== job" in job
    assert "job.texture.needsUpdate = true;" in job
    painter = block(SCENE, "function paintDeferredCanvases(", "\n}\n")
    assert "const job = queue.shift();" in painter
    assert "if (paintCanvasJob(job) && upload) {" in painter
    assert "upload(job.texture);" in painter
    assert "DEFERRED_CANVAS_SLICE_MS" in painter
    assert "requestIdleCallback" in painter
    start = SCENE.index("  setDeferredCanvasPaints([]);")
    stop = SCENE.index("  setDeferredCanvasPaints(null);")
    assert start < SCENE.index("officeBoardsDistrict.build(", start) < stop
    assert (
        "paintDeferredCanvases(\n      jobs,\n      () => disposed,\n"
        "      compactRenderer\n        ? null\n"
        "        : (texture) => stripedUploads.begin(texture) || renderer.initTexture(texture),\n"
        "      (texture) =>\n        stripedUploads.accepts(texture) || uploadBudget.takeTexture(texture),\n    );"
    ) in SCENE


def test_hud_restyles_are_not_triggered_by_unrelated_dom_writes():
    assert "html:has(" not in CSS
    assert "html.world-active {" in CSS
    assert 'document.documentElement.classList.add("world-active");' in APP
    assert 'document.documentElement.classList.remove("world-active");' in APP
    for selector in re.findall(r"\.fm-world:has\(([^)]*)", CSS):
        assert selector.startswith("> .world-hud > "), selector
    identity = block(APP, "  updateIdentityUI() {", "\n  updateMetrics() {")
    assert "if (quickInitial.textContent !== initial)" in identity
    assert "if (quickAvatar.hidden !== !avatarPng)" in identity
    status = block(APP, "  updateWorldStatusUI() {", "\n  syncInactivePresence() {")
    assert 'preview.textContent !== (copy || "Off")' in status
    screen = block(SCENE, "  function repaintScreen(current) {", "\n  function sync(")
    assert "if (key === screenKey) return false;" in screen
    proximity = block(SCENE, "  function updateOfficeAquariumProximity(time) {", "\n  function updateEngineeringDebugPanel(")
    assert "officeAquarium.setInView(false);" in proximity
    assert "syncAquariumPanel(time);" in proximity


def test_chat_appends_do_not_pay_for_has_rules_or_interleaved_layout():
    chat = (
        Path(__file__).resolve().parents[2] / "app" / "public" / "dashboard-chat.js"
    ).read_text(encoding="utf-8")
    assert "dialog:has(" not in CSS
    assert ".world-top-link:has(" not in CSS
    assert CSS.count("dialog[data-chat-thread-dialog]") >= 40
    thread = block(chat, "  function ensureThreadDialog() {", "\n  }\n")
    assert 'dialog.dataset.chatThreadDialog = "true";' in thread
    scroll = block(chat, '    fullLog?.addEventListener("scroll", () => {', "    }, {")
    sync = scroll.index("syncChatScrollThumb();")
    assert scroll.index("stickToNewest = fullLogAtNewest();") < sync
    assert scroll.index("historyPreloadThreshold()") < sync
    assert sync < scroll.index("revealOlderHistory({ fillBuffer: true });")


def test_startup_reads_layout_before_the_hud_is_inserted_and_defers_chat():
    connected = block(APP, "  async connectedCallback() {", "\n  disconnectedCallback() {")
    assert connected.index("this.syncViewportHeight();") < connected.index("this.innerHTML = worldTemplate(")
    resize = block(APP, "  bindDetailResize() {", "\n    let activePointerId = null;")
    assert "getBoundingClientRect" not in resize
    assert "clientWidth" not in resize
    assert 'grip.addEventListener("pointerenter", syncOnce);' in resize
    bind = block(APP, "  bindUI() {", "\n    this.addEventListener(\"click\", (event) => {")
    chat = bind[bind.rindex("if (this.bootCurtain()) {"):]
    assert '"forkmesh:world-interactive"' in chat
    assert "this.loadNativeWorldChat();" in chat
    resolution = block(SCENE, "  function applyRendererResolution() {", "\n  function applyWeatherParticleBudget(")
    assert "renderer.setDrawingBufferSize(" in resolution
    assert "renderer.setPixelRatio(" not in resolution
    assert "renderer.getPixelRatio() === ratio" in resolution
