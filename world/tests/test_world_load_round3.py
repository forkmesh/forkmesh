import json
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


REPOSITORY = Path(__file__).resolve().parents[2]
WORLD = REPOSITORY / "world" / "public" / "world"
SCENE_DIR = WORLD / "scene"
WARMUP_MODULE = SCENE_DIR / "shader-warmup.js"
SCENE = world_scene_source()
APP = (WORLD / "world.js").read_text(encoding="utf-8")
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


def module_text(relative: str) -> str:
    return (SCENE_DIR / relative).read_text(encoding="utf-8")


def test_warm_pass_creates_programs_without_drawing_or_counting_deferrals():
    result = node(
        f"""
      import {{ installShaderCompileGuard }} from {json.dumps(WARMUP_MODULE.as_uri())};
      const renderer = {{ info: {{ programs: [], render: {{ frame: 1 }} }} }};
      let draws = 0;
      const makeProgram = () => ({{ program: {{}}, isReady() {{ return true; }}, getUniforms() {{ return {{}}; }} }});
      renderer.renderBufferDirect = function (camera, scene, geometry, material) {{
        if (!material.program) {{
          material.program = makeProgram();
          renderer.info.programs.push(material.program);
        }}
        material.program.getUniforms();
        draws += 1;
      }};
      const guard = installShaderCompileGuard(renderer, () => true, () => 0);
      const materials = [{{}}, {{}}, {{}}];
      guard.warm(() => materials.forEach((material) => renderer.renderBufferDirect({{}}, {{}}, {{}}, material, {{}}, null)));
      const afterWarm = {{ draws, programs: renderer.info.programs.length, warmed: guard.warmedDraws, deferred: guard.deferredDraws }};
      const waiting = guard.primeReadyPrograms(50);
      renderer.renderBufferDirect({{}}, {{}}, {{}}, materials[0], {{}}, null);
      process.stdout.write(JSON.stringify({{ afterWarm, waiting, primed: guard.primedPrograms, draws, deferred: guard.deferredDraws }}));
    """
    )
    assert result["afterWarm"] == {"draws": 0, "programs": 3, "warmed": 3, "deferred": 0}
    assert result["waiting"] == 0
    assert result["primed"] == 3
    assert result["draws"] == 1
    assert result["deferred"] == 0


def test_programs_are_never_primed_behind_a_compiling_program():
    result = node(
        f"""
      import {{ installShaderCompileGuard }} from {json.dumps(WARMUP_MODULE.as_uri())};
      const renderer = {{ info: {{ programs: [], render: {{ frame: 1 }} }}, renderBufferDirect() {{}} }};
      const guard = installShaderCompileGuard(renderer, () => true, () => 0);
      let reads = 0;
      const program = (ready) => ({{ program: {{}}, ready, isReady() {{ return this.ready; }}, getUniforms() {{ reads += 1; return {{}}; }} }});
      const ready = program(true);
      const compiling = program(false);
      renderer.info.programs.push(ready, compiling);
      const waiting = guard.primeReadyPrograms(50);
      process.stdout.write(JSON.stringify({{ waiting, reads }}));
    """
    )
    assert result == {"waiting": 1, "reads": 0}


def test_first_view_is_warmed_at_construction_and_held_until_compiled():
    tail = SCENE[SCENE.index("  worldBuilt = true;"):]
    order = [
        "canvasSized = true;",
        "firstViewHoldUntil = performance.now() + FIRST_VIEW_HOLD_MS;",
        "applyCanvasSize();",
        "warmFirstView({ budgetMs: FIRST_VIEW_WARM_SLICE_MS });",
        "renderer.setAnimationLoop(animate);",
    ]
    positions = [tail.index(step) for step in order]
    assert positions == sorted(positions)
    warm = block(SCENE, "  function warmFirstView({ shadows = true, budgetMs = 0, resume = false } = {}) {", "\n  async function prepareFirstView(")
    assert "updateSceneLevelOfDetail();" in warm
    assert warm.index("updateSceneLevelOfDetail();") < warm.index("shaderCompileGuard.warm(")
    assert "renderer.shadowMap.needsUpdate =\n        shadowUpdate || (shadows && renderer.shadowMap.enabled);" in warm
    assert "if (!shadows) renderer.shadowMap.needsUpdate = false;" in warm
    assert "shaderCompileGuard.warm(\n        () => settledCuller.render(renderer, camera)," in warm
    assert "renderer.info.render.frame = frame;" in warm
    prepare = block(SCENE, "  async function prepareFirstView(", "\n  function scheduleGpuWarmup() {")
    assert "firstViewHoldUntil = 0;" in prepare
    animate = module_text("runtime/animate.js")
    render = block(animate, "    ctx.worldProfiler.beginGpu();", 'ctx.worldProfiler.mark("render");')
    assert "if (!(ctx.firstViewHoldUntil > time)) {" in render
    resize = block(SCENE, "  const applyCanvasSize = () => {", "\n  // ResizeObserver is unavailable")
    assert "!(firstViewHoldUntil > performance.now()) &&" in resize
    warmup = block(SCENE, "  function scheduleGpuWarmup() {", "gpuWarmupTimer = window.setTimeout(() => {")
    assert "!running || firstViewHoldUntil > performance.now()" in warmup


def test_resuming_a_running_scene_does_not_render_synchronously():
    animate = module_text("runtime/animate.js")
    paused = block(animate, "  function setPaused(paused) {", "\n  function setPreviewCadence(")
    assert paused.index("if (ctx.running === !paused) return;") < paused.index("ctx.running = !paused;")
    assert "if (!(ctx.firstViewHoldUntil > performance.now())) {" in paused


def test_construction_defers_panel_paints_and_paints_the_first_view_before_reveal():
    canvas = module_text("canvas.js")
    assert "if (Array.isArray(deferredCanvasPaints) && worldPanelArtDepth === 0) {" in canvas
    core = module_text("core.js")
    assert core.index("setDeferredCanvasPaints([]);") < core.index("const scene = new THREE.Scene();")
    tail = core[core.index("  canvasSized = true;"):]
    assert tail.index("constructionCanvasPaints = deferredCanvasPaints || [];") < tail.index("setDeferredCanvasPaints(null);")
    assert tail.index("setDeferredCanvasPaints(null);") < tail.index("warmFirstView({ budgetMs: FIRST_VIEW_WARM_SLICE_MS });")
    boards = module_text("districts/office-boards.js")
    assert "setDeferredCanvasPaints" not in boards
    diagnostics = module_text("runtime/diagnostics.js")
    prepare = block(diagnostics, "  async function prepareFirstView(", "\n  function scheduleGpuWarmup() {")
    assert "paintFirstViewCanvases(sliceMs)" in prepare
    assert prepare.index("ctx.firstViewHoldUntil = 0;") < prepare.index("releaseConstructionCanvases();")
    warm = block(diagnostics, "  function warmFirstView({ shadows = true, budgetMs = 0, resume = false } = {}) {", "\n  async function prepareFirstView(")
    assert "noteFirstViewTextures" in warm
    animate = module_text("runtime/animate.js")
    assert "if (ctx.constructionCanvasPaints.length) fns.releaseConstructionCanvases();" in animate


def test_sliced_batch_merge_matches_the_one_shot_merge():
    three = WORLD / "vendor" / "three-0.184.0" / "three.module.min.js"
    batching = SCENE_DIR / "batching.js"
    result = node(
        f"""
      import * as THREE from {json.dumps(three.as_uri())};
      import {{ mergeObjectBatches, mergeObjectBatchesSteps }} from {json.dumps(batching.as_uri())};
      const build = () => {{
        const root = new THREE.Group();
        for (let index = 0; index < 5; index += 1) {{
          const group = new THREE.Group();
          group.position.set(index, 0, 0);
          const mesh = new THREE.Mesh(
            new THREE.BoxGeometry(1, 1 + index, 1),
            new THREE.MeshStandardMaterial({{ color: index % 2 ? "#ff0000" : "#00ff00", roughness: 0.8 }}),
          );
          mesh.position.y = index;
          group.add(mesh);
          root.add(group);
        }}
        return root;
      }};
      const summary = (group) => group.children.map((mesh) => [
        mesh.geometry.attributes.position.count,
        Array.from(mesh.geometry.attributes.position.array.slice(0, 12)).map((value) => Math.round(value * 1000)),
      ]);
      const whole = mergeObjectBatches(THREE, build());
      const steps = mergeObjectBatchesSteps(THREE, build());
      let yields = 0;
      let step = steps.next();
      while (!step.done) {{
        yields += 1;
        step = steps.next();
      }}
      process.stdout.write(JSON.stringify({{ same: JSON.stringify(summary(whole)) === JSON.stringify(summary(step.value)), yields, draws: step.value.userData.drawCount }}));
    """
    )
    assert result["same"] is True
    assert result["yields"] == 12
    assert result["draws"] >= 1


def test_construction_builds_the_office_far_proxy_in_slices():
    core = module_text("core.js")
    assert "await rebuildOfficeFarProxySliced(sceneSlice);" in core
    floors = module_text("districts/office-floors.js")
    schedule = block(floors, "  function scheduleOfficeFarProxyRebuild() {", "\n  function syncOfficeDistanceLevel(")
    assert "rebuildOfficeFarProxySliced(pause, { warm: true })" in schedule
    sliced = block(floors, "  async function rebuildOfficeFarProxySliced(", "\n  function scheduleOfficeFarProxyRebuild(")
    assert sliced.index("if (warm) await fns.warmBeforeReveal(step.value);") < sliced.index(
        "return finishOfficeFarProxy(build, step.value);"
    )
    finish = block(floors, "  function finishOfficeFarProxy(", "\n  function rebuildOfficeFarProxy(")
    assert finish.index("low.remove(child);") < finish.index("low.add(...proxy.children);")


def test_the_renderer_is_prepared_while_the_scene_modules_stream():
    quality = SCENE_DIR / "quality.js"
    result = node(
        f"""
      const store = new Map([["forkmesh.world.gpu.v1", "ANGLE (AMD, AMD Radeon RX 580 Series)"]]);
      globalThis.localStorage = {{ getItem: (key) => store.get(key) ?? null, setItem: (key, value) => store.set(key, value) }};
      const {{ planWorldRenderer, prepareWorldRendererContext }} = await import({json.dumps(quality.as_uri())});
      const win = {{
        matchMedia: () => ({{ matches: false }}),
        navigator: {{ hardwareConcurrency: 12, deviceMemory: 16 }},
        devicePixelRatio: 1,
        innerWidth: 1600,
        innerHeight: 900,
        screen: {{ width: 1920, height: 1080 }},
      }};
      const container = {{ getBoundingClientRect: () => ({{ width: 1600, height: 900 }}) }};
      const first = planWorldRenderer({{ container, win }});
      const second = planWorldRenderer({{ container, win }});
      const compact = planWorldRenderer({{ container, win, forceCompactRenderer: true }});
      let created = null;
      const doc = {{ createElement: () => ({{ getContext: (type, attributes) => (created = {{ type, attributes }}) }}) }};
      class WebGLRenderer {{ constructor(options) {{ this.options = options; this.capabilities = {{ getMaxAnisotropy: () => 16 }}; }} }}
      const prepared = prepareWorldRendererContext(first, {{ WebGLRenderer }}, doc);
      process.stdout.write(JSON.stringify({{
        same: first.key === second.key,
        compactDiffers: compact.key !== first.key,
        tier: first.bootQualityTier,
        type: created.type,
        antialias: created.attributes.antialias,
        rendererContext: prepared.renderer.options.context === prepared.context,
        key: prepared.key === first.key,
      }}));
    """
    )
    assert result == {
        "same": True,
        "compactDiffers": True,
        "tier": "desktop-high",
        "type": "webgl2",
        "antialias": True,
        "rendererContext": True,
        "key": True,
    }
    app = APP
    prepare = block(app, "  prepareWorldRenderer() {", "\n  async bootstrap() {")
    assert 'import("./scene/quality.js"),' in prepare
    assert 'import("./scene/environment.js"),' in prepare
    assert "prepared.sky = environment.bakeSkyEnvironment(THREE, prepared.renderer, sky);" in prepare
    assert prepare.index("prepareWorldRendererContext(plan, THREE, document)") < prepare.index("await new Promise((resolve) => setTimeout(() => resolve(), 0));")
    assert prepare.index("await new Promise((resolve) => setTimeout(() => resolve(), 0));") < prepare.index("environment.prepareSkyEnvironment?.(THREE, prepared.renderer)")
    assert prepare.index("environment.skyEnvironmentProgramsReady?.(prepared.renderer) === false") < prepare.index("prepared.sky = environment.bakeSkyEnvironment(")
    boot = block(app, "  async bootstrap() {", "WORLD_BOOT?.endScene();")
    assert boot.index("const preparedRenderer = this.prepareWorldRenderer();") < boot.index("await this.trackBootStep(")
    assert "preparedRenderer: prepared," in boot
    assert boot.index("const prepared = await preparedRenderer;") < boot.index("WORLD_BOOT?.beginScene();")
    core = module_text("core.js")
    assert "preparedRenderer?.key === rendererPlan.key" in core
    assert "const renderer = preparedContext?.renderer || new THREE.WebGLRenderer({" in core


def test_warm_pass_reports_every_drawn_material():
    result = node(
        f"""
      import {{ installShaderCompileGuard }} from {json.dumps(WARMUP_MODULE.as_uri())};
      const renderer = {{ info: {{ programs: [], render: {{ frame: 1 }} }}, renderBufferDirect() {{}} }};
      const guard = installShaderCompileGuard(renderer, () => true, () => 0);
      const seen = [];
      guard.warm(() => {{
        renderer.renderBufferDirect({{}}, {{}}, {{}}, {{ id: 1 }}, {{}}, null);
        renderer.renderBufferDirect({{}}, null, {{}}, {{ id: 2 }}, {{}}, null);
      }}, (material) => seen.push(material.id));
      renderer.renderBufferDirect({{}}, {{}}, {{}}, {{ id: 3 }}, {{}}, null);
      process.stdout.write(JSON.stringify({{ seen }}));
    """
    )
    assert result == {"seen": [1, 2]}
