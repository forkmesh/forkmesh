import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


REPOSITORY = Path(__file__).resolve().parents[2]
WORLD = REPOSITORY / "world" / "public" / "world"
SCENE_DIR = WORLD / "scene"
APP = (WORLD / "world.js").read_text(encoding="utf-8")
INDEX = (WORLD / "index.html").read_text(encoding="utf-8")
CSS = (WORLD / "world.css").read_text(encoding="utf-8")
LOADER = (WORLD / "world-loader.js").read_text(encoding="utf-8")
BOOT_MODULE = WORLD / "world-boot.js"
DEBUG_MODULE = WORLD / "world-debug-log.js"
WARMUP_MODULE = SCENE_DIR / "shader-warmup.js"
BUILD_MODULE = SCENE_DIR / "scene-build.js"
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


def scene_text(relative: str) -> str:
    return (SCENE_DIR / relative).read_text(encoding="utf-8")


def test_closed_hud_panels_stay_out_of_the_first_layout():
    template = block(APP, "function worldTemplate(", "\nclass ForkMeshWorld")
    for panel in (
        'class="world-brand-nav" data-world-dormant',
        'class="world-detail" data-world-dormant',
        'data-world-office-lobby data-world-dormant',
        'class="world-office-task-panel" data-world-dormant',
        'class="world-office-task-checkin" data-world-dormant',
        'data-world-office-room data-world-dormant',
        'class="world-office-chat" data-world-dormant',
        'class="world-account" data-world-dormant',
        'aria-hidden="true" inert data-world-dormant>',
        'data-world-tour data-world-dormant',
    ):
        assert panel in APP
    assert "data-world-dormant" not in block(template, "worldPerfBarMarkup()", "\n")
    assert re.search(r"\[data-world-dormant\] \{\s*content-visibility: hidden;\s*\}", CSS)
    watch = block(APP, "  watchDormantPanels() {", "\n  markWorldInteractive() {")
    assert 'attributeFilter: ["data-open"]' in watch
    assert 'if (target?.dataset?.open !== "true") continue;' in watch
    assert 'target.querySelectorAll?.("[data-world-dormant]").forEach(wake);' in watch
    assert "this.whenWorldSettled(WORLD_DORMANT_PANEL_WAKE_MS).then(wakeNext)" in watch
    connected = block(APP, "  async connectedCallback() {", "\n  disconnectedCallback() {")
    assert connected.index("this.innerHTML = worldTemplate(") < connected.index("this.watchDormantPanels();")
    assert connected.index("await nextWorldFrame();") < connected.index("this.innerHTML = worldTemplate(")


def test_the_hud_fonts_are_preloaded_and_date_formatting_is_warmed_during_the_download():
    assert '<link rel="preload" as="font" type="font/woff2" href="/assets/fonts/Geist-Variable.woff2" crossorigin />' in INDEX
    assert '<link rel="preload" as="font" type="font/woff2" href="/assets/fonts/GeistMono-Variable.woff2" crossorigin />' in INDEX
    assert "ABCFavorit-Bold.woff2\" crossorigin" not in INDEX
    assert INDEX.index('as="font"') < INDEX.index('<link rel="stylesheet" href="/world/world.css" />')
    warm = LOADER.index("new Intl.DateTimeFormat(undefined, { hour: \"numeric\", minute: \"2-digit\" })")
    assert warm < LOADER.index('runtime = await import("./world-runtime.js");')


def test_the_perf_bar_keeps_its_boot_layout_until_the_world_is_interactive():
    sync = block(APP, "  syncPerfBar() {", "\n  bootPerfBarWanted() {")
    assert "if (!this.worldInteractiveDone || !this.worldTicketResolved) {" in sync
    assert "this.perfBarBootShown ??= this.bootPerfBarWanted();" in sync
    assert "void this.afterWorldInteractive().then(() => {" in sync
    assert "if (bar.hidden === shown) bar.hidden = !shown;" in sync
    assert "return shown;" in sync
    assert "this.perfBarBootTools ??= this.bootPerfBarAdmin();" in sync
    assert "element.hidden = !tools;" in sync
    wanted = block(APP, "  bootPerfBarWanted() {", "\n  bindPerfBar() {")
    assert "readWorldBootPrefs(storage).admin === true" in wanted
    assert "if (!validWorldSession()) return false;" in wanted


def test_interactive_waiters_resume_one_per_task():
    drain = block(APP, "  drainWorldInteractiveWaiters() {", "\n  whenWorldSettled(")
    assert "const next = this.worldInteractiveWaiters?.shift();" in drain
    assert "window.setTimeout(resumeNextWorldInteractiveWaiter, 0);" in drain
    after = block(APP, "  afterWorldInteractive() {", "\n  revealWorld() {")
    assert "(this.worldInteractiveWaiters ||= []).push(resolve);" in after
    assert "if (this.worldInteractiveDone && !this.worldInteractiveDraining) {" in after


def test_non_critical_account_reads_wait_until_the_world_settles():
    profile = block(APP, "  async loadWorldFediverseProfile(", "\n  async toggleWorldFediverseFollow")
    assert profile.index("await this.whenWorldSettled(WORLD_SELF_FEED_DELAY_MS);") < profile.index("/contributions`")
    assert "if (target.self === true) {" in profile
    preferences = block(APP, "  async loadWorldPreferences() {", "\n  async syncWorldPreferences() {")
    assert "const upload = this.worldPreferencesNeedUpload(payload);" in preferences
    assert "if (upload) await this.syncWorldPreferences();" in preferences
    need = block(APP, "  worldPreferencesNeedUpload(payload) {", "\n  mergeWorldPreferenceSnapshot(payload) {")
    assert "if (this.settingsUpdatedAt > remoteSettingsAt) return true;" in need
    assert "return !remote || view.updatedAt > remote.updatedAt;" in need
    chat = block(APP, '        "forkmesh:world-interactive",\n        () => {\n          void this.whenWorldSettled(WORLD_CHAT_BOOT_DELAY_MS)', "{ once: true }")
    assert "this.loadNativeWorldChat();" in chat


def test_the_sky_programs_compile_before_the_bake():
    env = scene_text("environment.js")
    prepare = block(env, "export function prepareSkyEnvironment(THREE, renderer) {", "\nexport function skyEnvironmentProgramsReady(")
    assert prepare.index("renderer.setRenderTarget(target);") < prepare.index("renderer.compile(prepared.envScene")
    assert "pmrem._blurMaterial, pmrem._ggxMaterial" in prepare
    assert 'resources.uniqueMaterial(THREE, {\n        name: "PMREM.Background",' in prepare
    assert "renderer.setRenderTarget(output);" in prepare
    assert "target?.dispose?.();" in prepare
    renderer = block(APP, "  prepareWorldRenderer() {", "\n  async bootstrap() {")
    assert renderer.index("environment.prepareSkyEnvironment?.(THREE, prepared.renderer)") < renderer.index("skyEnvironmentProgramsReady")
    assert "performance.now() < compileUntil" in renderer
    assert "setTimeout(resolve" not in renderer.replace("setTimeout(() => resolve(), 0)", "").replace("setTimeout(() => resolve(), 8)", "")


def test_scene_slices_yield_to_a_painted_frame_once_a_frame_is_full():
    result = node(
        f"""
      const frames = [];
      globalThis.requestAnimationFrame = (callback) => {{ frames.push(callback); return frames.length; }};
      const {{ yieldSceneFrame, SCENE_BUILD_FRAME_MS, SCENE_BUILD_FRAME_WAIT_MS }} = await import({json.dumps(BUILD_MODULE.as_uri())});
      let resumed = false;
      const waiting = yieldSceneFrame().then(() => {{ resumed = true; }});
      await new Promise((resolve) => setTimeout(resolve, 5));
      const beforeFrame = resumed;
      frames.shift()();
      await waiting;
      const hidden = Date.now();
      globalThis.requestAnimationFrame = () => 0;
      await yieldSceneFrame();
      process.stdout.write(JSON.stringify({{ beforeFrame, resumed, frameMs: SCENE_BUILD_FRAME_MS, waitMs: SCENE_BUILD_FRAME_WAIT_MS, fallbackMs: Date.now() - hidden }}));
    """
    )
    assert result["beforeFrame"] is False
    assert result["resumed"] is True
    assert result["frameMs"] <= 40
    assert result["fallbackMs"] >= result["waitMs"] - 5
    core = scene_text("core.js")
    slice_fn = block(core, "  async function sceneSlice(", "\n  // Phones frequently")
    assert "if (reached - sceneFrameStartedAt >= SCENE_BUILD_FRAME_MS) {" in slice_fn
    assert "await yieldSceneFrame();" in slice_fn
    diagnostics = scene_text("runtime/diagnostics.js")
    prepare = block(diagnostics, "  async function prepareFirstView(", "\n  function scheduleGpuWarmup() {")
    assert "} else if (performance.now() - frameStartedAt >= SCENE_BUILD_FRAME_MS) {" in prepare


def test_the_first_view_warm_pass_is_budgeted_across_slices():
    result = node(
        f"""
      import {{ installShaderCompileGuard }} from {json.dumps(WARMUP_MODULE.as_uri())};
      let clock = 0;
      const renderer = {{ info: {{ programs: [], render: {{ frame: 1 }} }} }};
      renderer.renderBufferDirect = function (camera, scene, geometry, material) {{
        if (!material.program) {{
          clock += 10;
          material.program = {{ program: {{}}, isReady() {{ return true; }}, getUniforms() {{ return {{}}; }} }};
          renderer.info.programs.push(material.program);
        }}
        material.program.getUniforms();
      }};
      const guard = installShaderCompileGuard(renderer, () => true, () => clock);
      const materials = Array.from({{ length: 6 }}, () => ({{}}));
      const pass = () => guard.warm(() => materials.forEach((material) => renderer.renderBufferDirect({{}}, {{}}, {{}}, material, {{}}, null)), null, 25);
      const passes = [];
      for (let index = 0; index < 5; index += 1) {{
        const complete = pass();
        passes.push({{ complete, programs: renderer.info.programs.length, flag: guard.warmComplete }});
        if (complete) break;
      }}
      const unbudgeted = guard.warm(() => {{}});
      process.stdout.write(JSON.stringify({{ passes, unbudgeted }}));
    """
    )
    passes = result["passes"]
    assert [entry["complete"] for entry in passes] == [False, True]
    assert [entry["programs"] for entry in passes] == [3, 6]
    assert all(entry["complete"] == entry["flag"] for entry in passes)
    assert result["unbudgeted"] is True
    core = scene_text("core.js")
    tail = core[core.index("  canvasSized = true;"):]
    assert "warmFirstView({ budgetMs: FIRST_VIEW_WARM_SLICE_MS });" in tail
    assert "shaderCompileGuard.warmComplete === false && pass < FIRST_VIEW_WARM_PASSES" in tail
    assert "warmFirstView({ budgetMs: FIRST_VIEW_WARM_SLICE_MS, resume: true, shadows: false });" in tail


def test_figure_layer_copies_follow_a_time_budget():
    batches = scene_text("figure-batches.js")
    assert "copyBudget = gpuBusy() ? 0 : frameCopyBudget();" in batches
    assert (
        "gpuBusy: () =>\n      programPacer.stats.paced\n        ? programPacer.busy()\n"
        "        : shaderCompileGuard.pendingPrograms() > 0,"
    ) in scene_text("core.js")
    budget = block(batches, "  function frameCopyBudget() {", "\n  function flushCopies() {")
    assert "if (!(copyCostMs > 0)) return FIGURE_LAYER_COPIES_MIN;" in budget
    assert "Math.floor(FIGURE_LAYER_COPY_BUDGET_MS / copyCostMs)" in budget
    copy = block(batches, "  function copyLayer(layer, slot, texture) {", "\n  function staleLayers(")
    assert copy.index("rememberUnpackState();") < copy.index("renderer.copyTextureToTexture(")
    remember = block(batches, "  function rememberUnpackState() {", "\n  function copyLayer(")
    assert "state.pixelStorei(gl[name], 0);" in remember


def test_canvas_resizes_wait_for_compiling_programs():
    core = scene_text("core.js")
    apply = block(core, "  const applyCanvasSize = () => {", "\n  // ResizeObserver is unavailable")
    assert "const compiling = programPacer.stats.paced\n      ? programPacer.busy()\n      : shaderCompileGuard.pendingPrograms() > 0;" in apply
    assert "if (renderer.info.render.frame > 0 && compiling) {" in apply
    assert apply.index("viewportRect.height = height;") < apply.index("programPacer.hold();")
    assert "if (now - resizeDeferredSince < CANVAS_RESIZE_COMPILE_WAIT_MS) {" in apply
    assert "resizeFrame = window.requestAnimationFrame(applyCanvasSize);" in apply
    assert "shaderCompileGuard.pendingPrograms() === 0" in apply
    assert apply.index("resizeDeferredSince = 0;") < apply.index("applyRendererResolution();")


def test_boot_timers_with_native_callbacks_are_attributed_to_their_caller():
    result = node(
        f"""
      const boot = await import({json.dumps(BOOT_MODULE.as_uri())});
      const debug = await import({json.dumps(DEBUG_MODULE.as_uri())});
      const site = boot.timerCallSite("Error\\n    at worldBootTimerRegistration (https://x/world/world-boot.js?v=1:1:9)\\n    at prepare (https://x/world/world-runtime.js?v=abc:1561:6450)\\n");
      const bare = boot.timerCallSite("Error\\n    at https://x/world/world-boot.js:1:2\\n    at https://x/world/world.js:8176:22");
      const native = boot.isNativeCallback(Promise.prototype.then) && !boot.isNativeCallback(() => 1);
      const text = debug.describeLongAnimationFrame(
        {{ duration: 190, blockingDuration: 140, scripts: [{{ invoker: "TimerHandler:setTimeout", sourceFunctionName: "worldBootTimerCallback", sourceURL: "https://x/world/world-boot.js", duration: 120, startTime: 10 }}] }},
        {{ resolveScript: (script) => script.sourceFunctionName === "worldBootTimerCallback" ? "native callback queued at world.js:8176:22" : "" }},
      ).text;
      process.stdout.write(JSON.stringify({{ site, bare, native, text }}));
    """
    )
    assert result["site"] == "prepare @ world-runtime.js:1561:6450"
    assert result["bare"] == "world.js:8176:22"
    assert result["native"] is True
    assert "TimerHandler:setTimeout → native callback queued at world.js:8176:22 120 ms" in result["text"]


def test_installed_boot_log_wraps_timers_until_it_settles():
    result = node(
        f"""
      const listeners = [];
      const timers = [];
      const win = {{
        location: {{ origin: "https://x" }},
        performance: {{ now: () => 5, getEntriesByType: () => [], mark() {{}}, measure() {{}} }},
        localStorage: {{ getItem: () => null, setItem() {{}} }},
        setTimeout(callback) {{ timers.push(callback); return timers.length; }},
        setInterval(callback) {{ return 0; }},
        requestAnimationFrame: () => 0,
        addEventListener: (...args) => listeners.push(args),
        document: {{ readyState: "complete", querySelector: () => null, addEventListener() {{}} }},
      }};
      const original = win.setTimeout;
      const {{ installWorldBoot }} = await import({json.dumps(BOOT_MODULE.as_uri())});
      const boot = installWorldBoot(win);
      const wrapped = win.setTimeout !== original;
      let fired = 0;
      win.setTimeout(Math.max, 0);
      const plain = () => {{ fired += 1; }};
      win.setTimeout(plain, 0);
      const nativeWrapped = timers[0] !== Math.max;
      const plainKept = timers[1] === plain;
      boot.fail("test");
      while (timers.length) timers.shift()();
      const restored = win.setTimeout === original;
      process.stdout.write(JSON.stringify({{ wrapped, nativeWrapped, plainKept, restored, fired, settled: boot.settled }}));
    """
    )
    assert result == {
        "wrapped": True,
        "nativeWrapped": True,
        "plainKept": True,
        "restored": True,
        "fired": 1,
        "settled": True,
    }


def test_load_summary_lists_critical_apis_and_regressions_against_the_last_load():
    result = node(
        f"""
      const boot = await import({json.dumps(BOOT_MODULE.as_uri())});
      const measure = {{
        interactiveMs: 1100,
        firstFrameMs: 900,
        overlayRemovedMs: 1450,
        phases: [{{ name: "engine", ms: 100 }}, {{ name: "populate", ms: 700 }}, {{ name: "session", ms: 900, background: true }}],
        sceneSlices: {{ count: 3, longestMs: 40 }},
        longFrames: [{{ ms: 120, at: 500, text: "Main thread blocked 120 ms (60 ms blocking) · render" }}],
        resources: [
          {{ kind: "api", name: "/api/world/ticket", ms: 685, serverMs: 604, start: 300, at: 985, cache: "network", bytes: 300 }},
          {{ kind: "api", name: "/api/accounts/users", ms: 250, serverMs: null, start: 200, at: 450, cache: "network", bytes: 3000 }},
          {{ kind: "api", name: "/api/world/preferences", ms: 700, serverMs: 650, start: 1300, at: 2000, cache: "network", bytes: 30 }},
        ],
      }};
      const previous = {{ at: Date.now() - 120000, interactiveMs: 900, firstFrameMs: 850, phases: {{ engine: 110, populate: 139 }}, blockedMs: 100, worstMs: 100, apis: {{}}, networkBytes: 0 }};
      const apis = boot.worldLoadApis(measure);
      const record = boot.worldLoadRecord(measure);
      const regressions = boot.compareWorldLoads(previous, record);
      const summary = boot.worldLoadSummary(measure, regressions, previous);
      const tips = boot.computeWorldLoadSuggestions({{ ...measure, regressions }});
      const compact = boot.compactBootLines([
        {{ kind: "module", text: "/a.js · 2 KB · cache · 3 ms", at: 1 }},
        {{ kind: "font", text: "/f.woff2 · 2 KB · cache · 3 ms", at: 2 }},
        {{ kind: "api", text: "/api/x · 1 KB · network · 9 ms", at: 3 }},
        {{ kind: "module", text: "/b.js · 2 KB · network · 3 ms", at: 4 }},
      ]);
      const store = new Map();
      const storage = {{ getItem: (key) => store.get(key) ?? null, setItem: (key, value) => store.set(key, value) }};
      boot.writeWorldLoadRuns(storage, Array.from({{ length: 8 }}, (_, index) => ({{ at: index }})));
      const kept = boot.readWorldLoadRuns(storage).map((run) => run.at);
      const broken = boot.readWorldLoadRuns({{ getItem: () => {{ throw new Error("blocked"); }} }});
      process.stdout.write(JSON.stringify({{ apis, record, regressions, summary, tips, compact, kept, broken }}));
    """
    )
    assert [api["name"] for api in result["apis"]] == ["/api/world/ticket", "/api/accounts/users"]
    assert result["apis"][0]["networkMs"] == 81
    assert result["apis"][0]["slow"] is True
    assert result["apis"][1]["slow"] is False
    assert result["record"]["phases"] == {"engine": 100, "populate": 700, "session": 900}
    assert [item["label"] for item in result["regressions"]] == ["populate phase"]
    summary = "\n".join(result["summary"])
    assert "interactive 1.10 s · first frame 900 ms · loader removed 1.45 s" in summary
    assert "/api/world/ticket 685 ms (server 604 ms, network 81 ms)" in summary
    assert "/api/world/preferences" not in summary
    assert "populate phase 139 ms → 700 ms" in summary
    ids = [tip["id"] for tip in result["tips"]]
    assert "api:critical" in ids
    assert "regressions" in ids
    assert result["compact"][0]["text"] == "2 files served from the browser cache (1 module, 1 font)"
    assert [line["kind"] for line in result["compact"]] == ["cache", "api", "module"]
    assert result["kept"] == [3, 4, 5, 6, 7]
    assert result["broken"] == []
