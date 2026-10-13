import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


REPOSITORY = Path(__file__).resolve().parents[2]
WORLD = REPOSITORY / "world" / "public" / "world"
SCENE_MODULE = WORLD / "world-scene.js"
BOOT_MODULE = WORLD / "world-boot.js"
SCENE = world_scene_source()
APP = (WORLD / "world.js").read_text(encoding="utf-8")
BOOT = BOOT_MODULE.read_text(encoding="utf-8")
LOADER = (WORLD / "world-loader.js").read_text(encoding="utf-8")
INDEX = (WORLD / "index.html").read_text(encoding="utf-8")
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


def build_site_assets():
    tools = REPOSITORY / "app" / "tools"
    if str(tools) not in sys.path:
        sys.path.insert(0, str(tools))
    import build_site_assets as module

    return module


def test_prefetch_plans_the_roster_and_the_viewers_profile():
    result = node(
        f"""
      import {{ startWorldBootPrefetch, worldBootPrefetchPlan, worldBootSession }}
        from {json.dumps(BOOT_MODULE.as_uri())};
      const calls = [];
      const storage = new Map([["forkmesh.session", JSON.stringify({{ nodeName: " Jett ", sessionToken: "tok" }})]]);
      let clock = 1000;
      const realNow = Date.now;
      Date.now = () => clock;
      const win = {{
        localStorage: {{ getItem: (key) => storage.get(key) ?? null }},
        fetch: (path, init) => {{ calls.push([path, init.headers.Authorization || "", init.credentials]); return Promise.resolve({{ path }}); }},
      }};
      const prefetch = startWorldBootPrefetch(win);
      const again = startWorldBootPrefetch(win);
      const roster = await prefetch.take("GET:public:/api/accounts/users?view=world");
      const repeat = prefetch.take("GET:public:/api/accounts/users?view=world");
      clock += 31_000;
      const expired = prefetch.take("GET:account:jett:/api/accounts/jett?viewer=jett");
      const blocked = {{ get localStorage() {{ throw new Error("blocked"); }}, fetch: () => Promise.reject(new Error("offline")) }};
      const guest = startWorldBootPrefetch(blocked);
      Date.now = realNow;
      process.stdout.write(JSON.stringify({{
        same: prefetch === again,
        calls,
        roster: roster.path,
        repeat,
        expired,
        guest: guest.pending(),
        badSession: worldBootSession({{ getItem: () => "{{not json" }}),
        plan: worldBootPrefetchPlan(null).map((item) => item.path),
      }}));
    """
    )
    assert result["same"] is True
    assert result["calls"] == [
        ["/api/accounts/users?view=world", "", "same-origin"],
        ["/api/accounts/jett?viewer=jett", "Bearer tok", "same-origin"],
    ]
    assert result["roster"] == "/api/accounts/users?view=world"
    assert result["repeat"] is None
    assert result["expired"] is None
    assert result["guest"] == ["GET:public:/api/accounts/users?view=world"]
    assert result["badSession"] is None
    assert result["plan"] == ["/api/accounts/users?view=world"]


def test_runtime_takes_prefetched_responses_and_starts_them_early():
    assert "module.startWorldBootPrefetch(window);" in LOADER
    assert "if (typeof window !== \"undefined\") startWorldBootPrefetch(window);" in APP
    fetch = block(APP, "  async fetchJSON(path, options = {}) {", "\n  async postJSON(")
    assert "window.__forkmeshWorldPrefetch?.take?.(requestKey)" in fetch
    assert "prefetched.catch(request)" in fetch
    assert fetch.index("const requestKey =") < fetch.index("__forkmeshWorldPrefetch")


def test_scene_trace_does_not_charge_yields_to_elements():
    result = node(
        f"""
      import {{ createWorldSceneTrace }} from {json.dumps(BOOT_MODULE.as_uri())};
      let clock = 0;
      const trace = createWorldSceneTrace({{ now: () => clock }});
      trace.begin();
      clock = 10;
      trace.element("a", "A", []);
      clock = 22;
      trace.yielded(0);
      clock = 72;
      trace.yielded(50);
      clock = 80;
      trace.element("b", "B", []);
      clock = 95;
      const elements = trace.end();
      process.stdout.write(JSON.stringify({{ elements, slices: trace.slices }}));
    """
    )
    by_id = {element["id"]: element["ms"] for element in result["elements"]}
    assert by_id == {"a": 10, "b": 20, "finish": 15}
    assert result["slices"]["count"] == 2
    assert result["slices"]["yieldedMs"] == 50
    assert result["slices"]["longestMs"] == 23


def test_scene_build_yields_between_elements():
    assert "export async function createWorldScene({" in SCENE
    assert "sceneSliceMs = SCENE_BUILD_SLICE_MS," in SCENE
    assert "const SCENE_BUILD_SLICE_MS = 12;" in SCENE
    helper = block(SCENE, "function yieldSceneBuild() {", "\nexport async function createWorldScene(")
    assert 'typeof scheduler?.yield === "function"' in helper
    assert "new MessageChannel()" in helper
    assert "setTimeout(resolve, 0)" in helper
    slice_fn = block(SCENE, "  async function sceneSlice(", "\n  // Phones frequently")
    assert "bootTrace?.sceneYield?.(sceneSliceStartedAt - reached);" in slice_fn
    build = SCENE
    assert build.count("await sceneSlice(") >= 55
    for checkpoint in (
        '"webgl-context"',
        '"sky-environment"',
        '"office-lobby"',
        '"office-floors"',
        '"office-elevator"',
        '"scene-state"',
        '"office-far-proxy"',
    ):
        assert f"await sceneSlice({checkpoint}" in build
    assert "this.world = await scene.createWorldScene({" in APP
    ended = block(APP, "      WORLD_BOOT?.endScene();\n      if (this.destroyed) {", 'this.finishBootStep("scene");')
    assert "this.world?.dispose?.();" in ended


def test_weather_is_built_only_when_precipitation_shows():
    assert "  let weather = null;" in SCENE
    assert 'registerWorldElement("weather", "Weather (rain & snow)", "Systems", []);' in SCENE
    ensure = block(SCENE, "  function ensureWeather() {", "\n  function syncWeatherField() {")
    assert "weather = createWeather(THREE, scene);" in ensure
    sync = block(SCENE, "  function syncWeatherField() {", "\n  // What a visitor")
    assert "if (!weather && !rainVisible && !snowVisible) return;" in sync
    assert sync.index("return;") < sync.index("ensureWeather();")
    assert "if (weather && worldElementEnabled(\"weather\")) {" in SCENE
    state = block(SCENE, "  function weatherFieldState() {", "\n  function getEnvironmentState(")
    assert "if (!weather) {" in state
    budget = block(SCENE, "  function applyWeatherParticleBudget(count) {", "\n  /**")
    assert "weatherParticleBudget = budget;" in budget
    assert "if (!weather) return budget;" in budget


def test_canvas_is_sized_once_after_construction_without_forced_layout():
    resize = block(SCENE, "  const resize = (entries) => {", "\n  // ResizeObserver is unavailable")
    assert "entries[entries.length - 1]?.contentRect" in resize
    assert "if (resizeFrame || disposed || !canvasSized) return;" in resize
    assert "if (!observedCanvasSize) {\n      const measured = container.getBoundingClientRect();" in resize
    assert "const rect = observedCanvasSize;" in resize
    assert "observedCanvasSize = null;" not in resize
    assert "renderer.info.render.frame > 0 &&" in resize
    tail = SCENE[SCENE.index("  worldBuilt = true;"):]
    assert tail.index("canvasSized = true;") < tail.index("applyCanvasSize();") < tail.index(
        "renderer.setAnimationLoop(animate);"
    )
    setup = block(SCENE, "  container.appendChild(renderer.domElement);", '  await sceneSlice("webgl-context"')
    assert "rendererContextInfo();" in setup
    assert "this.syncPerfBar();" in block(APP, "    this.worldTicketResolved = true;", "const country =")


def test_first_view_programs_are_primed_before_reveal():
    result = node(
        f"""
      import {{ installShaderCompileGuard }} from {json.dumps(SCENE_MODULE.as_uri())};
      let clock = 0;
      const renderer = {{ info: {{ programs: [], render: {{ frame: 1 }} }}, renderBufferDirect() {{}} }};
      const guard = installShaderCompileGuard(renderer, () => true, () => clock);
      const program = (ready, cost) => ({{ program: {{}}, ready, isReady() {{ return this.ready; }}, getUniforms() {{ clock += cost; return {{}}; }} }});
      const a = program(true, 5);
      const b = program(true, 5);
      const d = program(true, 5);
      const c = program(false, 5);
      renderer.info.programs.push(a, b, d, c);
      const first = guard.primeReadyPrograms(8);
      const second = guard.primeReadyPrograms(8);
      const primedWhileCompiling = guard.primedPrograms;
      c.ready = true;
      const third = guard.primeReadyPrograms(8);
      const fourth = guard.primeReadyPrograms(8);
      process.stdout.write(JSON.stringify({{ first, second, third, fourth, primedWhileCompiling, primed: guard.primedPrograms, compiled: guard.compiledPrograms }}));
    """
    )
    assert result["first"] == 1
    assert result["second"] == 1
    assert result["primedWhileCompiling"] == 0
    assert result["third"] == 2
    assert result["fourth"] == 0
    assert result["primed"] == 4
    assert result["compiled"] == 4
    prepare = block(SCENE, "  async function prepareFirstView(", "\n  function scheduleGpuWarmup() {")
    assert "guard.primeReadyPrograms(sliceMs)" in prepare
    assert "warmFirstView()" in prepare
    assert "if (performance.now() >= deadline) break;" in prepare
    populate = block(APP, '      const firstView = await this.world.prepareFirstView', 'this.finishBootStep("populate");')
    assert "await this.world.prepareFirstView?.({ timeoutMs: 900 })" in populate


def test_interactive_is_marked_when_the_player_can_move():
    bootstrap = block(APP, "  async bootstrap() {", "\n  bootCurtain() {")
    ready = bootstrap[bootstrap.index('loading.dataset.ready = "true";'):]
    assert ready.index("this.revealWorld();") < ready.index("this.markWorldInteractive();")
    assert ready.index("this.markWorldInteractive();") < ready.index("window.setTimeout(")
    assert 'WORLD_BOOT?.mark("overlay-removed"' in ready
    marked = block(APP, "  markWorldInteractive() {", "\n  releaseWorldInteractiveWaiters() {")
    assert marked.index("WORLD_BOOT?.interactive();") < marked.index("requestAnimationFrame")
    assert marked.count("requestAnimationFrame(") == 2
    nearby = block(APP, "  loadNearbyMemberData() {", "\n  loadFountainData(")
    assert nearby.index("Promise.allSettled") < nearby.index("await this.afterWorldInteractive();")
    assert nearby.index("await this.afterWorldInteractive();") < nearby.index(
        "this.memberDirectory = normalizeMemberDirectory"
    )
    assert "this.renderPeers();" not in nearby
    refresh = block(APP, "  async refreshMemberDirectory(", "\n  patchOwnDirectoryRow(")
    assert "await this.afterWorldInteractive();" in refresh
    profile = block(APP, "  async loadWorldFediverseProfile(", "\n  async toggleWorldFediverseFollow")
    assert "await this.afterWorldInteractive();" in profile
    note = block(APP, "  noteDirectoryMembers(names) {", "\n  // Shares the edge-cached")
    assert "if (this.nearbyMemberDataLoad || !this.worldInteractiveDone) return false;" in note
    complete = block(APP, "  completeBootTimeline(", "\n  // Yield one paint")
    assert "WORLD_BOOT_BACKGROUND_STEPS.has(entry.id)" in complete
    assert "this.bootBackgroundOpen.add(entry.id);" in complete


def test_member_lounge_updates_are_coalesced_and_skip_identical_rosters():
    sync = block(APP, "  syncMemberLounge() {", "\n  flushMemberLounge() {")
    assert "queueMicrotask(" in sync
    assert "this.memberLoungeQueued = true;" in sync
    lounge = block(SCENE, "  function updateMemberLounge(", "\n  function visitNeighborhoodHome(")
    assert "if (loungeSignature === memberLoungeSignature) return;" in lounge
    for task in ('"aquarium"', '"member-facts"', '"leaderboard"', '"seat-labels"'):
        assert f"queueLoungeTask({task}" in lounge
    queue = block(SCENE, "  function queueLoungeTask(name, task) {", "\n  function scheduleLoungeFigures(")
    assert "loungeTasks.delete(name);" in queue
    assert "window.setTimeout(step, 0)" in queue


def test_member_directory_faces_come_from_thumbnails():
    avatar = block(APP, "function memberDirectoryAvatar(user) {", "\nfunction memberAchievementBadges(")
    assert avatar.index("avatarThumb") < avatar.index("avatarUrl") < avatar.index("avatarPng")
    result = node(
        f"""
      const source = {json.dumps(block(APP, "const MEMBER_AVATAR_THUMB_RE", "\nfunction memberAchievementBadges("))};
      const sanitizePresenceText = (value, fallback) => String(value || fallback);
      const memberDirectoryAvatar = new Function("sanitizePresenceText", source + "; return memberDirectoryAvatar;")(sanitizePresenceText);
      process.stdout.write(JSON.stringify([
        memberDirectoryAvatar({{ avatarThumb: "data:image/webp;base64,UklGRg==", avatarUrl: "/api/accounts/a/avatar?v=1" }}),
        memberDirectoryAvatar({{ avatarThumb: "javascript:alert(1)", avatarUrl: "/api/accounts/a/avatar?v=0123abcd" }}),
        memberDirectoryAvatar({{ avatarUrl: "https://evil.example/x.png" }}),
        memberDirectoryAvatar({{ avatarPng: "iVBORw0KGgo=" }}),
      ]));
    """
    )
    assert result == [
        "data:image/webp;base64,UklGRg==",
        "/api/accounts/a/avatar?v=0123abcd",
        "",
        "data:image/png;base64,iVBORw0KGgo=",
    ]


def test_idle_diagnostics_skip_rendering_while_hidden():
    diagnostics = block(APP, "  startDiagnostics() {", "\n  collectDiagnostics(")
    assert "if (!document.hidden) this.renderDiagnostics();" in diagnostics
    assert "this.beatCrashGuard();" in diagnostics
    stats = block(SCENE, "  function getDiagnostics(", "\n    const contextInfo")
    assert "(diagnosticsSceneStats === null ||" in stats
    assert "collectSceneComplexity()" not in SCENE


def test_favorit_font_ships_as_woff2():
    font = REPOSITORY / "www" / "public" / "assets" / "fonts" / "ABCFavorit-Bold.woff2"
    assert font.read_bytes()[:4] == b"wOF2"
    assert font.stat().st_size < 60_000
    face = block(CSS, '  font-family: "ForkMesh Favorit";', "}")
    assert 'src:\n    url("/assets/fonts/ABCFavorit-Bold.woff2") format("woff2");' in face
    assert not (font.parent / "ABCFavorit-Bold.otf").exists()
    prompt = (REPOSITORY / "app" / "public" / "membership-prompt.js").read_text(encoding="utf-8")
    assert 'url("/assets/fonts/ABCFavorit-Bold.woff2") format("woff2")' in prompt
    assert 'href + (href.indexOf("?") < 0 ? "?" : "&") + "retry=1"' in INDEX


def test_module_urls_are_versioned_deps_first(tmp_path):
    build = build_site_assets()
    dist = tmp_path / "dist"
    (dist / "world" / "elements").mkdir(parents=True)
    (dist / "marketplace" / "packages" / "p" / "content" / "world").mkdir(parents=True)
    (dist / "world" / "a.js").write_text(
        'import "./b.js";\nimport("./elements/c.js");\n'
        'const panel = import(`./b.js?retry=${n}`);\n'
        'const pkg = () => import("../../../marketplace/packages/p/content/world/index.js");\n'
        'const q = import("../qr.js");\nconst app = import("../account-events.js");\n',
        encoding="utf-8",
    )
    (dist / "world" / "b.js").write_text('export const b = "/world/elements/c.js";\n', encoding="utf-8")
    (dist / "world" / "elements" / "c.js").write_text("export const c = 1;\n", encoding="utf-8")
    (dist / "marketplace" / "packages" / "p" / "content" / "world" / "index.js").write_text(
        "export default 1;\n", encoding="utf-8"
    )
    (dist / "qr.js").write_text("globalThis.q = 1;\n", encoding="utf-8")
    (dist / "world" / "vendor").mkdir()
    (dist / "world" / "vendor" / "three.js").write_text("export {};\n", encoding="utf-8")
    files = build.world_module_files(dist)
    assert "world/vendor/three.js" not in files
    versions = build.version_world_modules(dist, files)
    content = {relative: (dist / relative).read_text(encoding="utf-8") for relative in files}
    for relative in files:
        assert versions[relative] == build.dashboard_shell.content_version(content[relative])
    a = content["world/a.js"]
    assert f'import "./b.js?v={versions["world/b.js"]}";' in a
    assert f'import("./elements/c.js?v={versions["world/elements/c.js"]}")' in a
    assert f'`./b.js?v={versions["world/b.js"]}&retry=${{n}}`' in a
    assert f'index.js?v={versions["marketplace/packages/p/content/world/index.js"]}' in a
    assert f'"../qr.js?v={versions["qr.js"]}"' in a
    assert '"../account-events.js"' in a
    assert f'"/world/elements/c.js?v={versions["world/elements/c.js"]}"' in content["world/b.js"]
    (dist / "world" / "elements" / "c.js").write_text('import "../a.js";\n', encoding="utf-8")
    (dist / "world" / "a.js").write_text('import "./elements/c.js";\n', encoding="utf-8")
    with pytest.raises(RuntimeError, match="import cycle"):
        build.world_module_order(dist, ["world/a.js", "world/elements/c.js"])


def test_minified_modules_keep_their_behaviour_and_source_maps(tmp_path):
    build = build_site_assets()
    esbuild = build.WORLD_TOOLS / "node_modules" / ".bin" / "esbuild"
    if not esbuild.exists() or not NODE:
        pytest.skip("esbuild is not installed in world/tools")
    dist = tmp_path / "dist"
    (dist / "world").mkdir(parents=True)
    (dist / "world" / "world-backoff.js").write_text(
        (WORLD / "world-backoff.js").read_text(encoding="utf-8"), encoding="utf-8"
    )
    (dist / "world" / "use.js").write_text(
        'import { createWorldBackoff } from "./world-backoff.js";\n'
        "export const kind = typeof createWorldBackoff;\n",
        encoding="utf-8",
    )
    files = build.world_module_files(dist)
    build.minify_world_modules(dist, files)
    versions = build.version_world_modules(dist, files)
    minified = (dist / "world" / "world-backoff.js").read_text(encoding="utf-8")
    assert len(minified) < len((WORLD / "world-backoff.js").read_text(encoding="utf-8"))
    assert minified.rstrip().endswith("//# sourceMappingURL=world-backoff.js.map")
    source_map = json.loads((dist / "world" / "world-backoff.js.map").read_text(encoding="utf-8"))
    assert source_map["sources"] == ["world/public/world/world-backoff.js"]
    use = (dist / "world" / "use.js").read_text(encoding="utf-8")
    assert f'./world-backoff.js?v={versions["world/world-backoff.js"]}' in use
    result = node(
        f"""
      const module = await import({json.dumps((dist / "world" / "use.js").as_uri())});
      process.stdout.write(JSON.stringify({{ kind: module.kind }}));
    """
    )
    assert result == {"kind": "function"}
