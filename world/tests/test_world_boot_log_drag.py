import functools
import http.server
import json
import shutil
import subprocess
import threading
from pathlib import Path

import pytest


REPOSITORY = Path(__file__).resolve().parents[2]
PUBLIC = REPOSITORY / "world" / "public"
WORLD = PUBLIC / "world"
INDEX = (WORLD / "index.html").read_text(encoding="utf-8")
BOOT = (WORLD / "world-boot.js").read_text(encoding="utf-8")
NODE = shutil.which("node")
PLAYWRIGHT_CANDIDATES = (
    REPOSITORY / "app" / "browser_tests" / "node_modules" / "playwright",
    REPOSITORY / "world" / "browser_tests" / "node_modules" / "playwright",
)


def run_node(script: str, *, cwd: Path = WORLD, timeout: int = 60) -> dict:
    if not NODE:
        pytest.skip("node is not installed")
    result = subprocess.run(
        [NODE, "--input-type=module", "-e", script],
        cwd=cwd,
        text=True,
        capture_output=True,
        check=True,
        timeout=timeout,
    )
    return json.loads(result.stdout)


def test_clamp_keeps_the_panel_inside_the_viewport():
    result = run_node(
        """
        import { clampBootLogPosition, WORLD_BOOT_LOG_MARGIN } from "./world-boot.js";
        const size = { width: 440, height: 300 };
        const viewport = { width: 1200, height: 800 };
        process.stdout.write(JSON.stringify({
          margin: WORLD_BOOT_LOG_MARGIN,
          inside: clampBootLogPosition({ left: 100.4, top: 200.6 }, size, viewport),
          farRight: clampBootLogPosition({ left: 5000, top: 5000 }, size, viewport),
          negative: clampBootLogPosition({ left: -90, top: -40 }, size, viewport),
          tiny: clampBootLogPosition({ left: 300, top: 300 }, size, { width: 320, height: 200 }),
          invalid: clampBootLogPosition({ left: "x", top: 4 }, size, viewport),
          missing: clampBootLogPosition(null, size, viewport),
        }));
        """
    )
    margin = result["margin"]
    assert result["inside"] == {"left": 100, "top": 201}
    assert result["farRight"] == {"left": 1200 - 440 - margin, "top": 800 - 300 - margin}
    assert result["negative"] == {"left": margin, "top": margin}
    assert result["tiny"] == {"left": margin, "top": margin}
    assert result["invalid"] is None
    assert result["missing"] is None


def test_prefs_round_trip_the_position_and_survive_broken_storage():
    result = run_node(
        """
        import { readWorldBootPrefs, writeWorldBootPrefs } from "./world-boot.js";
        const data = new Map();
        const storage = {
          getItem: (key) => (data.has(key) ? data.get(key) : null),
          setItem: (key, value) => data.set(key, String(value)),
        };
        writeWorldBootPrefs(storage, { collapsed: true, admin: false, position: { left: 40.2, top: 70.7 } });
        const saved = readWorldBootPrefs(storage);
        writeWorldBootPrefs(storage, { collapsed: false, admin: false, position: null });
        const cleared = readWorldBootPrefs(storage);
        data.set("forkmesh.world.bootlog.v1", "{not json");
        const corrupt = readWorldBootPrefs(storage);
        const throwing = {
          getItem() { throw new Error("blocked"); },
          setItem() { throw new Error("blocked"); },
        };
        process.stdout.write(JSON.stringify({
          saved,
          cleared,
          corrupt,
          blockedRead: readWorldBootPrefs(throwing),
          blockedWrite: writeWorldBootPrefs(throwing, { position: { left: 1, top: 2 } }),
        }));
        """
    )
    assert result["saved"]["position"] == {"left": 40, "top": 71}
    assert result["saved"]["collapsed"] is True
    assert result["cleared"]["position"] is None
    assert result["corrupt"]["position"] is None
    assert result["blockedRead"]["position"] is None
    assert result["blockedWrite"] is False


def test_the_header_is_a_pointer_drag_handle_that_never_blocks_the_world():
    log = INDEX[INDEX.index("<aside"):INDEX.index("</aside>")]
    assert "<header data-world-boot-log-handle" in log
    header_rule = INDEX.split(".world-boot-log header {", 1)[1].split("}", 1)[0]
    assert "touch-action: none" in header_rule
    assert "cursor: grab" in header_rule
    assert "user-select: none" in header_rule
    drag = BOOT[BOOT.index("const bindDrag = (root) =>"):BOOT.index("const bindUI = () =>")]
    for event in ("pointerdown", "pointermove", "pointerup", "pointercancel", "lostpointercapture"):
        assert f'addEventListener("{event}"' in drag
    assert "setPointerCapture" in drag
    assert "releasePointerCapture" in drag
    assert "requestAnimationFrame" in drag
    assert '"resize"' in drag
    assert "ResizeObserver" in drag
    assert "writeWorldBootPrefs(storage, prefs)" in drag
    assert "mousemove" not in drag
    assert "window.addEventListener(\"pointermove\"" not in drag
    first_paint = INDEX[INDEX.index('boot.dataset.parsed = "true"') - 400:]
    first_paint = first_paint[: first_paint.index("</script>")]
    assert "prefs.position" in first_paint
    assert "catch (error)" in first_paint


def playwright_module() -> Path | None:
    for candidate in PLAYWRIGHT_CANDIDATES:
        if (candidate / "package.json").is_file():
            return candidate
    return None


@pytest.fixture()
def boot_log_page(tmp_path):
    styles = INDEX[INDEX.index("<style>"):INDEX.index("</style>") + len("</style>")]
    aside = INDEX[INDEX.index("<aside"):INDEX.index("</aside>") + len("</aside>")]
    pill = INDEX[INDEX.index('<button type="button" class="world-boot-log-pill"'):]
    pill = pill[: pill.index("</button>") + len("</button>")]
    page = f"""<!doctype html>
<html><head><meta charset="utf-8">{styles}</head>
<body style="margin:0;height:100vh;background:#123">
<canvas data-test-canvas style="position:fixed;inset:0;width:100%;height:100%"></canvas>
{aside}{pill}
<script type="module">
import {{ installWorldBoot }} from "/world/world-boot.js";
window.__canvasPointer = 0;
document.querySelector("[data-test-canvas]").addEventListener("pointermove", () => {{ window.__canvasPointer += 1; }});
const boot = installWorldBoot(window);
boot.setAccess({{ debugPanel: true }});
for (let i = 0; i < 12; i += 1) boot.mark(`line-${{i}}`, `line ${{i}}`);
window.__bootReady = true;
</script></body></html>"""
    root = tmp_path / "site"
    (root / "world").mkdir(parents=True)
    for name in ("world-boot.js", "world-debug-log.js"):
        shutil.copy(WORLD / name, root / "world" / name)
    (root / "drag.html").write_text(page, encoding="utf-8")
    handler = functools.partial(
        type("Quiet", (http.server.SimpleHTTPRequestHandler,), {"log_message": lambda *a: None}),
        directory=str(root),
    )
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_address[1]}/drag.html"
    finally:
        server.shutdown()
        server.server_close()


def test_boot_log_drags_by_mouse_and_touch_persists_and_stays_on_screen(boot_log_page, tmp_path):
    module = playwright_module()
    if module is None or not NODE:
        pytest.skip("playwright is not installed")
    script = tmp_path / "drag.cjs"
    script.write_text(
        """
const { chromium } = require(process.argv[2]);
(async () => {
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ viewport: { width: 1000, height: 700 }, hasTouch: true });
  const page = await context.newPage();
  const url = process.argv[3];
  const box = async () => page.evaluate(() => {
    const r = document.querySelector("[data-world-boot-log]").getBoundingClientRect();
    return { left: Math.round(r.left), top: Math.round(r.top), right: Math.round(r.right), bottom: Math.round(r.bottom) };
  });
  await page.goto(url);
  await page.waitForFunction(() => window.__bootReady === true);
  await page.waitForTimeout(150);
  const start = await box();
  const header = await page.locator("[data-world-boot-log-handle] strong").boundingBox();
  await page.mouse.move(header.x + 5, header.y + 5);
  await page.mouse.down();
  await page.mouse.move(header.x + 205, header.y - 195, { steps: 8 });
  const midDrag = await page.evaluate(() => document.querySelector("[data-world-boot-log]").dataset.dragging || "");
  await page.mouse.up();
  await page.waitForTimeout(50);
  const moved = await box();
  const canvasHits = await page.evaluate(() => window.__canvasPointer);
  const stored = await page.evaluate(() => JSON.parse(localStorage.getItem("forkmesh.world.bootlog.v1")));
  const collapseLabel = await page.locator("[data-world-boot-log-collapse]").textContent();
  await page.locator("[data-world-boot-log-collapse]").click();
  await page.waitForFunction(() => document.querySelector("[data-world-boot-log]").dataset.collapsed === "true", null, { timeout: 5000 }).catch(() => {});
  const collapsedAfterClick = await page.evaluate(() => document.querySelector("[data-world-boot-log]").dataset.collapsed);
  await page.locator("[data-world-boot-log-collapse]").click();
  await page.waitForFunction(() => document.querySelector("[data-world-boot-log]").dataset.collapsed === "false", null, { timeout: 5000 }).catch(() => {});
  await page.mouse.move(header.x + 205, header.y - 195);
  await page.mouse.down();
  await page.mouse.move(5000, 5000, { steps: 4 });
  await page.mouse.up();
  await page.waitForTimeout(50);
  const cornered = await box();
  await page.reload();
  await page.waitForFunction(() => window.__bootReady === true);
  await page.waitForTimeout(50);
  const reloaded = await box();
  await page.setViewportSize({ width: 600, height: 420 });
  await page.waitForTimeout(150);
  const resized = await box();
  const handle = await page.locator("[data-world-boot-log-handle] strong").boundingBox();
  const touchStart = { x: handle.x + 4, y: handle.y + 4 };
  await page.evaluate(async ({ x, y }) => {
    const target = document.querySelector("[data-world-boot-log-handle] strong");
    const fire = (type, cx, cy, on = target) => on.dispatchEvent(new PointerEvent(type, {
      pointerId: 7, pointerType: "touch", isPrimary: true, bubbles: true, cancelable: true, clientX: cx, clientY: cy, button: 0, buttons: 1,
    }));
    fire("pointerdown", x, y);
    const handle = document.querySelector("[data-world-boot-log-handle]");
    for (let i = 1; i <= 6; i += 1) fire("pointermove", x - i * 30, y - i * 20, handle);
    await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    fire("pointerup", x - 180, y - 120, handle);
  }, touchStart);
  await page.waitForTimeout(50);
  const touched = await box();
  await page.locator("[data-world-boot-log-handle] strong").dblclick();
  await page.waitForTimeout(50);
  const reset = await page.evaluate(() => ({
    placed: document.querySelector("[data-world-boot-log]").dataset.worldBootLogPlaced || "",
    stored: JSON.parse(localStorage.getItem("forkmesh.world.bootlog.v1")).position,
  }));
  process.stdout.write(JSON.stringify({ start, moved, midDrag, canvasHits, stored, collapseLabel, collapsedAfterClick, cornered, reloaded, resized, touched, touchStart, reset }));
  await browser.close();
})().catch((error) => { console.error(error); process.exit(1); });
""",
        encoding="utf-8",
    )
    try:
        completed = subprocess.run(
            [NODE, str(script), str(module), boot_log_page],
            text=True,
            capture_output=True,
            timeout=90,
        )
    except subprocess.TimeoutExpired:
        pytest.skip("headless chromium did not start in time")
    if completed.returncode != 0 and "Executable doesn't exist" in completed.stderr:
        pytest.skip("playwright chromium is not installed")
    assert completed.returncode == 0, completed.stderr
    result = json.loads(completed.stdout)
    start, moved = result["start"], result["moved"]
    assert result["midDrag"] == "true"
    assert abs((moved["left"] - start["left"]) - 200) <= 2
    assert abs((moved["top"] - start["top"]) + 200) <= 2
    assert result["canvasHits"] == 0
    assert result["stored"]["position"] == {"left": moved["left"], "top": moved["top"]}
    assert result["collapseLabel"] == "Collapse"
    assert result["collapsedAfterClick"] == "true"
    cornered = result["cornered"]
    assert cornered["right"] <= 1000 - 8 + 1
    assert cornered["bottom"] <= 700 - 8 + 1
    assert result["reloaded"]["left"] == cornered["left"]
    assert result["reloaded"]["top"] == cornered["top"]
    resized = result["resized"]
    assert resized["left"] >= 8 and resized["top"] >= 8
    assert resized["right"] <= 600 - 8 + 1
    assert resized["bottom"] <= 420 - 8 + 1
    touched = result["touched"]
    assert touched["left"] < resized["left"]
    assert touched["top"] < resized["top"]
    assert result["reset"] == {"placed": "", "stored": None}
