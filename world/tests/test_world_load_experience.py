import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest
from world_scene_source import world_scene_source


WORLD = Path(__file__).resolve().parents[1] / "public" / "world"
INDEX = (WORLD / "index.html").read_text(encoding="utf-8")
APP = (WORLD / "world.js").read_text(encoding="utf-8")
LOADER = (WORLD / "world-loader.js").read_text(encoding="utf-8")
SCENE = world_scene_source()
CSS = (WORLD / "world.css").read_text(encoding="utf-8")
BOOT_MODULE = WORLD / "world-boot.js"
NODE = shutil.which("node")


def method(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    return source[start : source.index(next_signature, start)]


def run_node(script: str) -> dict:
    if not NODE:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        cwd=WORLD,
        text=True,
        capture_output=True,
        check=True,
        timeout=60,
    )
    return json.loads(result.stdout)


def test_exactly_one_loader_exists_and_it_is_static_in_the_document():
    assert len(re.findall(r"data-world-loading[\s>]", INDEX)) == 1
    assert INDEX.count('class="world-loading-curtain"') == 1
    template = method(APP, "function worldTemplate(", "\nclass ForkMeshWorld")
    assert "data-world-loading" not in template
    assert "world-loading-curtain" not in template
    assert "innerHTML" not in LOADER
    assert "world-loading-curtain" not in LOADER
    assert ".world-loading-curtain" not in CSS
    assert "world-loader-shimmer" not in CSS
    assert 'loading.dataset.ready = "true"' in APP


def test_static_ledger_rows_match_the_runtime_boot_steps():
    steps = re.findall(r'\{ id: "([a-z]+)", label: "([^"]+)" \}', APP)
    assert len(steps) == 7
    rows = re.findall(
        r'data-world-boot-step="([a-z]+)"[\s\S]*?world-loading-step-label">([^<]+)<',
        INDEX,
    )
    assert rows == steps


def test_shell_hides_the_world_ui_until_it_is_ready():
    assert (
        '<forkmesh-world data-world-mode="public" data-world-booting inert></forkmesh-world>'
        in INDEX
    )
    hidden = INDEX.split("forkmesh-world[data-world-booting] {", 1)[1].split("}", 1)[0]
    assert "opacity: 0" in hidden
    backdrop = INDEX.split(".world-boot {", 1)[1].split("}", 1)[0]
    assert "position: fixed" in backdrop
    assert "inset: 0" in backdrop
    assert "background: #f5f6f8" in backdrop
    assert '.world-boot:not([data-parsed="true"]) > *' in INDEX
    assert 'boot.dataset.parsed = "true"' in INDEX
    assert INDEX.index('class="world-boot"') < INDEX.index('boot.dataset.parsed = "true"')
    reveal = method(APP, "  revealWorld() {", "\n  removeBootOverlay() {")
    assert 'this.removeAttribute("data-world-booting");' in reveal
    assert 'this.removeAttribute("inert");' in reveal
    bootstrap = method(APP, "  async bootstrap() {", "\n  bootCurtain() {")
    ready = bootstrap[bootstrap.index('loading.dataset.ready = "true";'):]
    assert ready.index("this.revealWorld();") < ready.index("this.removeBootOverlay();")
    assert ready.index("this.revealWorld();") < ready.index("this.markWorldInteractive();")
    assert ready.index("this.markWorldInteractive();") < ready.index(
        "this.removeBootOverlay();"
    )
    assert ready.index("this.markWorldInteractive();") < ready.index(
        'new CustomEvent("forkmesh:world-interactive")'
    )
    marked = method(APP, "  markWorldInteractive() {", "\n  releaseWorldInteractiveWaiters() {")
    assert "WORLD_BOOT?.interactive();" in marked
    fallback = method(APP, "  renderWebGLFallback() {", "\n    const wrap =")
    assert "this.revealWorld();" in fallback
    assert "this.removeBootOverlay();" in fallback
    assert 'document.querySelector("[data-world-boot]")' in INDEX
    assert 'document.querySelector("[data-world-boot]")?.remove();' in LOADER


def test_loader_and_log_are_accessible():
    assert 'role="progressbar"' in INDEX
    assert 'aria-valuenow="2"' in INDEX
    assert 'aria-valuemin="0"' in INDEX and 'aria-valuemax="100"' in INDEX
    assert 'data-world-loading-stage role="status" aria-live="polite"' in INDEX
    log = INDEX[INDEX.index("<aside"):INDEX.index("</aside>")]
    assert 'role="log"' in log
    assert 'aria-live="polite"' in log
    assert "hidden" in log
    for control in ("data-world-boot-log-copy", "data-world-boot-log-collapse", "data-world-boot-log-dismiss"):
        assert control in log
    assert "data-world-boot-log-pill" in INDEX
    progress = method(APP, "  setLoadingProgress(percent, label) {", "\n  formatBootSeconds(")
    assert 'bar.setAttribute("aria-valuenow"' in progress


def test_log_panel_can_show_at_first_paint_for_debug_users():
    script = INDEX[INDEX.index('boot.dataset.parsed = "true"') - 400:]
    script = script[: script.index("</script>")]
    assert 'localStorage.getItem("forkmesh.world.settings.v1")' in script
    assert 'localStorage.getItem("forkmesh.world.bootlog.v1")' in script
    assert "settings.debugPanel === true" in script
    assert "prefs.admin === true" in script
    assert "try {" in script and "catch (error)" in script


def test_runtime_reports_phases_scene_and_access_to_the_boot_log():
    assert "  installWorldBoot,\n  readWorldBootPrefs,\n  startWorldBootPrefetch,\n} from \"./world-boot.js\";" in APP
    assert "WORLD_BOOT?.begin(id, entry.note" in APP
    assert "WORLD_BOOT?.end(id," in APP
    assert "WORLD_BOOT?.beginScene();" in APP
    assert "WORLD_BOOT?.endScene();" in APP
    assert "bootTrace: WORLD_BOOT," in APP
    assert "WORLD_BOOT?.setAuditProvider(() => this.world?.auditScene?.() || null);" in APP
    sync = method(APP, "  syncBootLogAccess() {", "\n  syncPerfBar() {")
    assert "isAdmin: this.identity?.isAdmin === true" in sync
    assert "debugPanel: this.settings?.debugPanel === true" in sync
    assert "this.syncBootLogAccess();" in method(APP, "  syncPerfBar() {", "\n  }\n")
    assert "bootTrace?.sceneElement?.(id, label, roots);" in SCENE
    guard = method(SCENE, "export function installShaderCompileGuard(", "\n  return stats;")
    assert "stats.compiledPrograms += 1;" in guard
    assert "const spent = now() - firstUseAt;" in guard
    assert "stats.firstUseMs += spent;" in guard
    assert "shaderCompileStats: () => shaderCompileGuard," in SCENE


def test_boot_log_buffer_is_bounded_and_prints_in_time_order():
    result = run_node(
        """
        import { createWorldBootLog } from "./world-boot.js";
        const log = createWorldBootLog({ limit: 3, now: () => 7 });
        log.add("a", "first", 30);
        log.add("b", "second", 10);
        log.add("c", "third", 20);
        log.add("d", "fourth  with\\n spaces", 40);
        process.stdout.write(JSON.stringify({
          size: log.size,
          dropped: log.dropped,
          kinds: log.entries().map((line) => line.kind),
          since: log.since(3).map((line) => line.kind),
          text: log.text({ suggestions: [{ level: "high", title: "T", detail: "D" }] }),
        }));
        """
    )
    assert result["size"] == 3
    assert result["dropped"] == 1
    assert result["kinds"] == ["b", "c", "d"]
    assert result["since"] == ["d"]
    lines = result["text"].splitlines()
    assert lines[:3] == ["Optimization suggestions", "- [high] T — D", ""]
    assert lines[3] == "(1 older lines dropped)"
    assert [line.split()[2] for line in lines[4:7]] == ["b", "c", "d"]
    assert "fourth with spaces" in lines[6]


def test_suggestions_rank_measured_problems_and_reuse_the_scene_audit():
    result = run_node(
        """
        import { computeWorldLoadSuggestions } from "./world-boot.js";
        const resources = [
          { name: "/world/world-scene.js", kind: "module", cache: "network", bytes: 1600000, ms: 900, at: 800 },
          { name: "/world/world.css", kind: "style", cache: "cache", bytes: 420000, ms: 3, at: 100 },
          { name: "/api/world/ticket", kind: "api", cache: "network", bytes: 300, ms: 1800, at: 900 },
          ...Array.from({ length: 70 }, (_, i) => ({ name: `/m${i}.js`, kind: "module", cache: "revalidated", bytes: 2000, ms: 5, at: 200 })),
        ];
        const tips = computeWorldLoadSuggestions({
          interactiveMs: 6000,
          firstFrameMs: 3000,
          interactiveFrameMs: 6600,
          phases: [
            { name: "scene", ms: 2400 },
            { name: "session", ms: 5000, background: true },
            { name: "populate", ms: 400 },
            { name: "restore", ms: 2 },
          ],
          elements: [
            { label: "Office", ms: 320, objects: 900, meshes: 400 },
            { label: "Bench", ms: 2, objects: 3, meshes: 3 },
          ],
          resources,
          longFrames: [{ ms: 700, text: "Main thread blocked 700 ms" }, { ms: 80, text: "" }],
          shader: { programs: 90, firstUseMs: 900 },
        }, {
          findings: [
            { id: "duplicate-geometry", level: "high", title: "337 identical geometries", fix: "Cache shapes", weight: 5 },
            { id: "sprites", level: "info", title: "Sprites", fix: "", weight: 1 },
          ],
        });
        const empty = computeWorldLoadSuggestions({}, null);
        process.stdout.write(JSON.stringify({ tips, empty }));
        """
    )
    tips = result["tips"]
    ids = [tip["id"] for tip in tips]
    levels = [tip["level"] for tip in tips]
    assert levels == sorted(levels, key=["high", "caution", "info"].index)
    assert "phase:scene" in ids
    assert "phase:populate" in ids
    assert "phase:session" not in ids
    assert "phase:restore" not in ids
    assert "scene-elements" in ids
    assert "download:/world/world-scene.js" in ids
    assert next(t for t in tips if t["id"] == "download:/world/world-scene.js")["level"] == "high"
    assert next(t for t in tips if t["id"] == "download:/world/world.css")["level"] == "info"
    assert "requests" in ids
    assert "revalidated" in ids
    assert "api:critical" in ids
    assert "/api/world/ticket 1.80 s" in next(t for t in tips if t["id"] == "api:critical")["title"]
    assert "long-frames" in ids
    assert "shaders" in ids
    assert "reveal-frame" in ids
    assert "frame-to-interactive" in ids
    assert "audit:duplicate-geometry" in ids
    assert "audit:sprites" not in ids
    audit = next(t for t in tips if t["id"] == "audit:duplicate-geometry")
    assert audit["title"] == "Scene audit: 337 identical geometries"
    assert audit["detail"] == "Cache shapes"
    assert [tip["id"] for tip in result["empty"]] == ["ok"]


def test_boot_log_visibility_rules_and_preferences():
    result = run_node(
        """
        import { worldBootLogVisible, readWorldBootPrefs, writeWorldBootPrefs } from "./world-boot.js";
        const memory = new Map();
        const storage = { getItem: (k) => memory.get(k) ?? null, setItem: (k, v) => memory.set(k, v) };
        const throwing = { getItem() { throw new Error("blocked"); }, setItem() { throw new Error("blocked"); } };
        memory.set("forkmesh.world.settings.v1", JSON.stringify({ debugPanel: true }));
        const wrote = writeWorldBootPrefs(storage, { collapsed: true, admin: true });
        process.stdout.write(JSON.stringify({
          visitor: worldBootLogVisible({ isAdmin: false, debugPanel: false, ticketResolved: true }),
          admin: worldBootLogVisible({ isAdmin: true, debugPanel: false, ticketResolved: true }),
          debug: worldBootLogVisible({ isAdmin: false, debugPanel: true, ticketResolved: false }),
          rememberedBeforeTicket: worldBootLogVisible({ ticketResolved: false, rememberedAdmin: true }),
          rememberedAfterTicket: worldBootLogVisible({ ticketResolved: true, rememberedAdmin: true }),
          wrote,
          prefs: readWorldBootPrefs(storage),
          blocked: readWorldBootPrefs(throwing),
          blockedWrite: writeWorldBootPrefs(throwing, { collapsed: true }),
          none: readWorldBootPrefs(null),
        }));
        """
    )
    assert result["visitor"] is False
    assert result["admin"] is True
    assert result["debug"] is True
    assert result["rememberedBeforeTicket"] is True
    assert result["rememberedAfterTicket"] is False
    assert result["wrote"] is True
    assert result["prefs"] == {"collapsed": True, "admin": True, "debugPanel": True, "position": None}
    assert result["blocked"] == {"collapsed": False, "admin": False, "debugPanel": False, "position": None}
    assert result["blockedWrite"] is False
    assert result["none"] == {"collapsed": False, "admin": False, "debugPanel": False, "position": None}


def test_resource_entries_are_classified_by_cache_and_kind():
    result = run_node(
        """
        import { describeResourceEntry } from "./world-boot.js";
        const origin = "https://world.forkmesh.com";
        const entries = [
          { name: origin + "/world/world-scene.js?v=1", initiatorType: "script", transferSize: 0, encodedBodySize: 1000, decodedBodySize: 4000, duration: 2, startTime: 5 },
          { name: origin + "/world/world.css", initiatorType: "link", transferSize: 300, encodedBodySize: 400000, duration: 20, startTime: 5 },
          { name: origin + "/api/world/ticket", initiatorType: "fetch", transferSize: 900, encodedBodySize: 600, duration: 40, startTime: 10, responseStatus: 200 },
          { name: "https://cdn.example/font.woff2", initiatorType: "css", transferSize: 0, encodedBodySize: 0, duration: 9, startTime: 1 },
          { name: origin + "/world/assets/a.webp", initiatorType: "img", transferSize: 5000, encodedBodySize: 4800, duration: 9, startTime: 1, responseStatus: 404 },
        ];
        process.stdout.write(JSON.stringify(entries.map((entry) => describeResourceEntry(entry, origin))));
        """
    )
    assert [(r["kind"], r["cache"]) for r in result] == [
        ("module", "cache"),
        ("style", "revalidated"),
        ("api", "network"),
        ("font", "unknown"),
        ("texture", "network"),
    ]
    assert result[0]["name"] == "/world/world-scene.js"
    assert result[3]["name"] == "cdn.example/font.woff2"
    assert "HTTP 200" in result[2]["text"]
    assert "HTTP 404" in result[4]["text"]
    assert result[2]["at"] == 50


def test_boot_controller_records_hidden_stops_after_load_and_audits_only_when_visible():
    result = run_node(
        """
        import { installWorldBoot } from "./world-boot.js";
        let clock = 0;
        const frames = [];
        const timers = [];
        const observers = [];
        class FakeObserver {
          static supportedEntryTypes = ["resource", "paint", "long-animation-frame"];
          constructor(callback) { this.callback = callback; this.connected = true; observers.push(this); }
          observe({ type }) { this.type = type; }
          disconnect() { this.connected = false; }
        }
        const win = {
          performance: { now: () => clock, getEntriesByType: () => [], mark() {}, measure() {} },
          PerformanceObserver: FakeObserver,
          requestAnimationFrame: (callback) => frames.push(callback),
          setTimeout: (callback) => timers.push(callback),
          location: { origin: "https://world.forkmesh.com" },
          localStorage: { getItem() { throw new Error("blocked"); }, setItem() { throw new Error("blocked"); } },
        };
        const runFrames = () => { const due = frames.splice(0); due.forEach((cb) => cb()); return due.length; };
        const runTimers = () => { const due = timers.splice(0); due.forEach((cb) => cb()); };
        const boot = installWorldBoot(win);
        const again = installWorldBoot(win);
        let audits = 0;
        boot.setAuditProvider(() => { audits += 1; return { findings: [{ id: "x", level: "high", title: "X", fix: "F" }] }; });
        observers.find((o) => o.type === "resource").callback({ getEntries: () => [
          { name: "https://world.forkmesh.com/world/world-scene.js", initiatorType: "script", transferSize: 2000000, encodedBodySize: 1900000, duration: 500, startTime: 10 },
        ] });
        observers.find((o) => o.type === "long-animation-frame").callback({ getEntries: () => [
          { duration: 900, blockingDuration: 850, startTime: 40, scripts: [] },
        ] });
        clock = 100;
        boot.begin("scene");
        boot.beginScene();
        const tree = (count) => ({ traverse(visit) { for (let i = 0; i < count; i += 1) visit({ isMesh: i > 0 }); } });
        clock = 400;
        boot.sceneElement("town", "Town square", [tree(5)]);
        clock = 450;
        boot.sceneElement("bench", "Benches", tree(2));
        boot.sceneElement("bench", "Benches", tree(2));
        clock = 470;
        const elements = boot.endScene();
        boot.end("scene");
        const renderer = { info: { render: { frame: 0 } }, capabilities: { isWebGL2: true, maxTextureSize: 4096 }, getPixelRatio: () => 1 };
        boot.renderer(renderer, "Test GPU");
        boot.setShaderProvider(() => ({ compiledPrograms: 70, firstUseMs: 300, deferredDraws: 4 }));
        clock = 600; runFrames();
        renderer.info.render.frame = 1;
        clock = 700; runFrames();
        clock = 900; boot.interactive();
        runFrames();
        renderer.info.render.frame = 2;
        clock = 950; runFrames();
        runTimers();
        runFrames();
        const afterSettle = { frames: frames.length, connected: observers.filter((o) => o.connected).length };
        runTimers();
        const hiddenAudits = audits;
        boot.setAccess({ isAdmin: true, debugPanel: false, ticketResolved: true });
        runTimers();
        process.stdout.write(JSON.stringify({
          same: boot === again,
          measure: boot.measure,
          elements,
          afterSettle,
          hiddenAudits,
          audits,
          settled: boot.settled,
          suggestions: boot.suggestions.map((tip) => tip.id),
          text: boot.text(),
          visibleAfterAdmin: boot.visible(),
        }));
        """
    )
    assert result["same"] is True
    measure = result["measure"]
    assert measure["firstFrameMs"] == 700
    assert measure["interactiveMs"] == 900
    assert measure["interactiveFrameMs"] == 950
    assert measure["shader"] == {"programs": 70, "firstUseMs": 300, "deferredDraws": 4}
    assert measure["phases"] == [{"name": "scene", "ms": 370, "background": False}]
    town, bench, finish = result["elements"]
    assert town == {"id": "town", "label": "Renderer setup + Town square", "ms": 300, "objects": 5, "meshes": 4, "count": 1}
    assert bench["ms"] == 50 and bench["objects"] == 4 and bench["count"] == 2
    assert finish["label"] == "Finishing construction" and finish["ms"] == 20
    assert result["afterSettle"] == {"frames": 0, "connected": 0}
    assert result["hiddenAudits"] == 0
    assert result["audits"] == 1
    assert result["settled"] is True
    assert result["visibleAfterAdmin"] is True
    for tip in ("long-frames", "download:/world/world-scene.js", "shaders", "audit:x"):
        assert tip in result["suggestions"]
    text = result["text"]
    assert "WebGL2 context · Test GPU" in text
    assert "First frame rendered" in text
    assert "First interactive frame" in text
    assert "Town square" in text
    assert "70 programs compiled" in text
    assert "Optimization suggestions" in text
    assert text.index("Optimization suggestions") < text.index("First frame rendered")
